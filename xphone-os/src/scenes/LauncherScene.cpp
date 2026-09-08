#include "LauncherScene.h"

#include <BatteryMonitor.h>

#include <cstdio>
#include <cstring>

#include "../BatteryGauge.h"
#include "../Fonts.h"
#include "../IconStyle.h"
#include "../StatusBar.h"
#include "../art/LauncherIcons.h"
#include "../ble/CompanionBleService.h"
#include "AppScenes.h"
#include "HomeScene.h"

#include <SDCardManager.h>
#include <ArduinoJson.h>

namespace {

// Six slots in a 3x2 grid. What fills them comes from NVS (Phase 3
// "homeSlots"): the six builtins by default, or installed apps in any
// slot. See loadSlots(). Icon bitmaps come from IconStyle (Settings ->
// Icon style packs); the XPhoneIconPacks columns in LauncherIcons.h share
// the builtin order Today, Notifications, Priorities, Block, Read, Workout.

// 1bpp blitter for the ported artwork (format per LauncherIcons.h header;
// a cleared bit is ink, only ink pixels are drawn so paper stays white).
//
// Masters are always XPhoneLauncherIconSize wide. `size` is the on-screen
// footprint — when it differs from the master we nearest-neighbor sample
// (same math as Settings' icon-style preview). Using `size` as the row
// stride was a bug: drawing at 88% of 104 made rowBytes 12 instead of 13
// and scrambled the bitmap into TV-static.
void drawIcon(Gfx& gfx, const uint8_t* bitmap, const int x, const int y, const int size) {
  const int srcSize = XPhoneLauncherIconSize;
  const int rowBytes = (srcSize + 7) / 8;
  for (int row = 0; row < size; ++row) {
    const int sy = (size == srcSize) ? row : (row * srcSize) / size;
    for (int col = 0; col < size; ++col) {
      const int sx = (size == srcSize) ? col : (col * srcSize) / size;
      const uint8_t byte = bitmap[sy * rowBytes + (sx >> 3)];
      if (((byte >> (7 - (sx & 7))) & 1) == 0) gfx.drawPixel(x + col, y + row, true);
    }
  }
}

// Layout (both panels are native landscape; everything derives from gfx dims).
constexpr int kMargin = 16;       // outer margin
constexpr int kStatusH = 40;      // status bar height incl. separator
constexpr int kGap = 28;          // gap between cells (row + column)
constexpr int kSelRadius = 12;    // tile rounded-corner radius
constexpr int kSelThick = 3;      // selection border thickness
constexpr int kBoxInset = 8;      // rounded box inset inside the cell (near-full tile)
constexpr int kTilePad = 12;      // inner padding between the box edge and icon/label
constexpr int kMaxTileSide = 186; // cap so 5 tiles don't fill the panel — smaller
                                  // boxes with comfortable margins (approved size)

int gridRows() { return (LauncherScene::APP_COUNT + LauncherScene::COLS - 1) / LauncherScene::COLS; }

// Lazily constructed so BoardConfig::ACTIVE is definitely set (no static-init
// order dependence). X3 reads the BQ27220 fuel gauge over I2C
// (FREEINK_BATTERY_I2C_GAUGE, SDA20/SCL0 from BoardConfig); X4 falls through
// to the ADC divider path in the same binary.
BatteryMonitor& battery() {
  static BatteryMonitor mon;
  return mon;
}

}  // namespace

void LauncherScene::onEnter() {
  loadSlots();
  markDirty();
}

void LauncherScene::loadSlots() {
  // Builtin id -> launcher icon index + label (LauncherIcons.h app order).
  struct Builtin { const char* id; const char* label; int8_t icon; };
  static constexpr Builtin kBuiltins[] = {
      {"today", "Today", 0},        {"notifications", "Notifications", 1},
      {"priorities", "Priorities", 2}, {"block", "Block", 3},
      {"read", "Read", 4},          {"workout", "Workout", 5}};
  char csv[224];
  homeSlotsCsv(csv, sizeof(csv));
  // A saved layout that names an app the card no longer holds is stale, and
  // in 0.7 it can only be developer residue: no phone can write a layout
  // yet. Patching the gap put Notifications wherever the dead entry happened
  // to sit — first on Andrew's X3, when it belongs right after Today. So a
  // stale list is dropped whole and the documented default order stands.
  // Once a phone can arrange the home screen this wants revisiting, because
  // then a saved list is a person's choice and only the dead entry should go.
  {
    char probe[224];
    snprintf(probe, sizeof(probe), "%s", csv);
    char* psave = nullptr;
    bool stale = false;
    for (char* tok = strtok_r(probe, ",", &psave); tok && !stale; tok = strtok_r(nullptr, ",", &psave)) {
      bool isBuiltin = false;
      for (const Builtin& b : kBuiltins) isBuiltin = isBuiltin || !strcmp(tok, b.id);
      if (isBuiltin) continue;
      char path[64];
      snprintf(path, sizeof(path), "/apps/%s/app.json", tok);
      if (!((SdMan.ready() || SdMan.begin()) && SdMan.exists(path))) stale = true;
    }
    if (stale) {
      Serial.printf("[xphone-os] launcher: saved slots name a missing app; using the default order\n");
      snprintf(csv, sizeof(csv), "%s", kDefaultSlotsCsv());
      setHomeSlots(csv);
    }
  }
  int n = 0;
  char* save = nullptr;
  for (char* tok = strtok_r(csv, ",", &save); tok && n < APP_COUNT; tok = strtok_r(nullptr, ",", &save)) {
    Slot& s = _slots[n];
    bool builtin = false;
    for (const Builtin& b : kBuiltins) {
      if (!strcmp(tok, b.id)) {
        snprintf(s.id, sizeof(s.id), "%s", b.id);
        snprintf(s.label, sizeof(s.label), "%s", b.label);
        s.icon = b.icon;
        builtin = true;
        break;
      }
    }
    if (!builtin) {
      // An app that is no longer installed must not keep its tile. Removing
      // an app deletes its files but never touched the saved slot list, so
      // the home screen went on offering a tile that opens nothing — Andrew
      // still saw Transit after it was removed (2026-09-08). When the app
      // file is gone the NEXT unplaced builtin takes that position, in the
      // order they are declared. Skipping the slot instead pushed the
      // displaced builtin to the end of the grid, which put Notifications
      // last on Andrew's X3 when it belongs right after Today.
      {
        char probe[64];
        snprintf(probe, sizeof(probe), "/apps/%s/app.json", tok);
        if (!((SdMan.ready() || SdMan.begin()) && SdMan.exists(probe))) {
          const Builtin* fill = nullptr;
          for (const Builtin& b : kBuiltins) {
            bool used = false;
            for (int i = 0; i < n && !used; ++i) used = !strcmp(_slots[i].id, b.id);
            if (used) continue;
            // Not already placed here, and not named later in the saved list
            // either — otherwise this would steal a slot the person chose.
            if (strstr(save ? save : "", b.id)) continue;
            fill = &b;
            break;
          }
          if (!fill) continue;
          snprintf(s.id, sizeof(s.id), "%s", fill->id);
          snprintf(s.label, sizeof(s.label), "%s", fill->label);
          s.icon = fill->icon;
          ++n;
          continue;
        }
      }
      // An installed app: label from its app.json "name" (filtered parse,
      // first 256 bytes), else the dir name with a capital.
      snprintf(s.id, sizeof(s.id), "%s", tok);
      snprintf(s.label, sizeof(s.label), "%s", tok);
      if (s.label[0] >= 'a' && s.label[0] <= 'z') s.label[0] = static_cast<char>(s.label[0] - 32);
      s.icon = -1;
      char path[64];
      snprintf(path, sizeof(path), "/apps/%s/app.json", tok);
      if (SdMan.ready() || SdMan.begin()) {
        FsFile f = SdMan.open(path, O_RDONLY);
        if (f) {
          char buf[256];
          const int got = f.read(buf, sizeof(buf) - 1);
          f.close();
          if (got > 0) {
            buf[got] = 0;
            JsonDocument filter;
            filter["name"] = true;
            JsonDocument doc;
            if (!deserializeJson(doc, buf, static_cast<size_t>(got),
                                 DeserializationOption::Filter(filter)) &&
                doc["name"].as<const char*>())
              snprintf(s.label, sizeof(s.label), "%s", doc["name"].as<const char*>());
          }
        }
      }
    }
    ++n;
  }
  // Fill any short list with the builtins that are not yet placed.
  for (const Builtin& b : kBuiltins) {
    if (n >= APP_COUNT) break;
    bool placed = false;
    for (int i = 0; i < n; ++i) placed = placed || !strcmp(_slots[i].id, b.id);
    if (placed) continue;
    snprintf(_slots[n].id, sizeof(_slots[n].id), "%s", b.id);
    snprintf(_slots[n].label, sizeof(_slots[n].label), "%s", b.label);
    _slots[n].icon = b.icon;
    ++n;
  }
  _slotsLoaded = true;
  if (_sel >= APP_COUNT) _sel = 0;
}

void LauncherScene::moveSelection(const int dCol, const int dRow) {
  int sel = _sel;
  // Front Left/Right = linear PREV/NEXT with wrap (Workout↔Today).
  // Up/Down move by row and still clamp (no wrap) so a short last row feels
  // predictable.
  if (dCol != 0) {
    sel = (sel + dCol) % APP_COUNT;
    if (sel < 0) sel += APP_COUNT;
  }
  if (dRow < 0 && sel >= COLS) sel -= COLS;
  if (dRow > 0) {
    if (sel + COLS < APP_COUNT) {
      sel += COLS;
    } else if (sel / COLS < (APP_COUNT - 1) / COLS) {
      sel = APP_COUNT - 1;  // clamp into a short last row
    }
  }
  if (sel != _sel) {
    const int prev = _sel;
    _sel = sel;
    // M2.1a: a selection move repaints only the two affected cells (the
    // launcher's soft-key labels never change, the status bar is untouched by
    // selection). Before the first render the layout cache is empty and
    // cellRect() returns an empty rect — markDirty(empty) falls back to
    // full-panel, so this is safe in every state.
    XpRect dirty = cellRect(prev);
    dirty.unionWith(cellRect(_sel));
    markDirty(dirty);
  }
}

XpRect LauncherScene::cellRect(const int i) const {
  if (_side <= 0) return XpRect{};  // no layout yet -> full-panel fallback
  constexpr int16_t kSlop = 6;      // border rounding + label overhang past the tile edge
  const int col = i % COLS;
  const int row = i / COLS;
  const int x = _gridX + col * (_side + kGap);
  const int y = _gridY + row * (_cellH + kGap);
  return XpRect{static_cast<int16_t>(x - kSlop), static_cast<int16_t>(y - kSlop),
                static_cast<int16_t>(_side + 2 * kSlop), static_cast<int16_t>(_cellH + 2 * kSlop)};
}

void LauncherScene::handleInput(Input& in) {
  // Quick action: long-press the top-RIGHT button (Btn::Down) to start a
  // Deep Work block without opening the app. Checked before the tap handlers;
  // the Input state machine suppresses the tap-on-release once a long-press
  // fires, so Btn::Down won't also move the selection this press.
  if (in.wasLongPressed(Btn::Down)) {
    showBlockDeepWork();
    return;
  }
  if (in.wasPressed(Btn::Left)) moveSelection(-1, 0);
  if (in.wasPressed(Btn::Right)) moveSelection(+1, 0);
  if (in.wasPressed(Btn::Up)) moveSelection(0, -1);
  if (in.wasPressed(Btn::Down)) moveSelection(0, +1);
  if (in.wasPressed(Btn::Confirm)) {
    if (!_slotsLoaded) loadSlots();
    const char* id = _slots[_sel].id;
    if (strcmp(id, "block") == 0) {
      showBlock();
    } else if (strcmp(id, "priorities") == 0) {
      showPriorities();
    } else if (strcmp(id, "today") == 0) {
      showToday();
    } else if (strcmp(id, "notifications") == 0) {
      showNotifications();
    } else if (strcmp(id, "read") == 0) {
      showReader();
    } else if (strcmp(id, "workout") == 0) {
      showWorkout();
    } else {
      if (!showApp(id)) markDirty();  // a missing app just repaints the grid
    }
  }
  // BACK soft-key (short tap) opens Settings. SceneManager intercepts the
  // LONG-press BACK for the OS-wide go-home (a no-op on the launcher), so only
  // a short tap reaches here.
  if (in.wasPressed(Btn::Back)) showSettings();
}

const char* const* LauncherScene::softKeys() const {
  // Slot 0 (BACK button) opens Settings on the launcher; About lives inside it.
  static constexpr const char* kKeys[4] = {"SETTINGS", "OPEN", SoftKey::Left, SoftKey::Right};
  return kKeys;
}

void LauncherScene::render(Gfx& gfx) {
  const int w = gfx.width();
  const int h = gfx.height();

  // --- Status bar: Flowe lockup left, battery icon + percent right ----------
  // Brand mark from primitives (brand/assets/mark-reference.png): a half sun
  // sitting on the horizon with shrinking water-ripple bars beneath, then the
  // lowercase "flowe" wordmark beside it.
  {
    constexpr int kSunD = 22;
    const int sunX = kMargin;
    const int sunTop = 6;
    const int horizonY = sunTop + kSunD / 2;
    gfx.fillRoundedRect(sunX, sunTop, kSunD, kSunD, kSunD / 2, true);
    gfx.fillRect(sunX - 2, horizonY, kSunD + 4, kSunD / 2 + 2, false);  // carve below the horizon
    const int sunCx = sunX + kSunD / 2;
    gfx.fillRect(sunCx - (kSunD + 4) / 2, horizonY + 3, kSunD + 4, 2, true);   // ripples
    gfx.fillRect(sunCx - (kSunD - 8) / 2, horizonY + 8, kSunD - 8, 2, true);
    gfx.fillRect(sunCx - (kSunD - 16) / 2, horizonY + 13, kSunD - 16, 2, true);
    gfx.drawText(kFontBold, sunX + kSunD + 10, 4, "flowe");
  }
  // CrossPoint-style indicator (StatusBar.h): 15x12 body + nub, proportional
  // fill, percent in kFontSmall to the left. Unknown reads draw "--%" and an
  // empty body so the layout stays stable. Charging bolt when the BQ27220
  // average current is positive (into the battery) — X4's ADC path has no
  // gauge, readAvgCurrentMa returns false there and the bolt is simply off.
  const int barH = kStatusH - 2;  // content height above the separator
  uint16_t pct = 0;
  const bool havePct = battery().readPercentageChecked(pct);
  int16_t avgMa = 0;
  const bool charging = BatteryGauge::readAvgCurrentMa(avgMa) && avgMa > 0;
  const int battLeft = StatusBar::drawBattery(gfx, w - kMargin, barH,
                                              havePct ? static_cast<int>(pct) : -1, charging);

  // M2: BLE status dot left of the battery cluster — solid filled dot when
  // the iPhone is connected, hollow circle while advertising. Nothing before
  // the radio starts (BLE begins after the first launcher paint; see main.cpp).
  if (COMPANION_BLE.isStarted()) {
    const int d = 12;  // circle drawn as a fully-rounded rect
    const int dotX = battLeft - d - 10;
    const int dotY = (barH - d) / 2;
    if (COMPANION_BLE.isConnected()) {
      gfx.fillRoundedRect(dotX, dotY, d, d, d / 2, true);
    } else {
      gfx.drawRoundedRect(dotX, dotY, d, d, d / 2, 2, true);
    }
  }
  gfx.fillRect(0, kStatusH - 2, w, 2, true);  // separator

  // --- App grid: square tiles with the LABEL INSIDE the rounded box ---------
  // Each cell is a plain square (no separate label row) so three rows fit with
  // room to spare. The tile side is the smaller of what the width and height
  // budgets allow, capped at kMaxTileSide so five tiles read as comfortable
  // cards rather than filling the panel; the whole block is then centered both
  // ways between the status bar and the soft-key bar.
  const int rows = gridRows();
  const int availTop = kStatusH;
  const int availH = h - Scene::SOFTKEY_BAR_H - availTop;
  const int sideFromW = (w - 2 * kMargin - (COLS - 1) * kGap) / COLS;
  const int sideFromH = (availH - (rows - 1) * kGap) / rows;  // cellH == side now
  int side = sideFromW < sideFromH ? sideFromW : sideFromH;
  if (side > kMaxTileSide) side = kMaxTileSide;
  const int cellH = side;  // label lives inside the tile

  const int gridBlockW = COLS * side + (COLS - 1) * kGap;
  const int gridH = rows * cellH + (rows - 1) * kGap;
  const int gridX = (w - gridBlockW) / 2;  // center horizontally
  int gridY = availTop + (availH - gridH) / 2;
  if (gridY < availTop + 4) gridY = availTop + 4;  // never collide with chrome

  // M2.1a: cache the layout for cellRect() (selection-move dirty rects).
  _gridX = static_cast<int16_t>(gridX);
  _gridY = static_cast<int16_t>(gridY);
  _side = static_cast<int16_t>(side);
  _cellH = static_cast<int16_t>(cellH);

  // Draw at the native master size. Padding lives in the art itself; do not
  // downscale here (a previous 88% draw used the wrong stride and scrambled
  // every icon into static — Settings preview looked fine because it samples
  // correctly from the 104px source).
  const int iconSize = XPhoneLauncherIconSize;
  const int labelLineH = gfx.lineHeight(kFontBold);

  if (!_slotsLoaded) loadSlots();
  for (int i = 0; i < APP_COUNT; i++) {
    const int col = i % COLS;
    const int row = i / COLS;
    const int cx = gridX + col * (side + kGap);
    const int cy = gridY + row * (cellH + kGap);

    // Rounded box = the near-full tile (small inset). Thin outline for every
    // app, thick when selected. Icon and label both live inside it.
    // Selection chrome only — unselected tiles have no outline so the icons
    // read as a calm grid of glyphs rather than a wall of boxes.
    const int boxX = cx + kBoxInset;
    const int boxY = cy + kBoxInset;
    const int boxSide = side - 2 * kBoxInset;
    if (i == _sel) {
      gfx.drawRoundedRect(boxX, boxY, boxSide, boxSide, kSelRadius, kSelThick, true);
    }

    // Icon centered horizontally, sitting in the region above the label. The
    // label is pinned near the bottom inside the box; the icon is vertically
    // centered in whatever space remains above it.
    const int iconAreaH = boxSide - 2 * kTilePad - labelLineH;
    int iconY = boxY + kTilePad + (iconAreaH > iconSize ? (iconAreaH - iconSize) / 2 : 0);
    const int iconX = cx + (side - iconSize) / 2;
    const Slot& slot = _slots[i];
    if (slot.icon >= 0) {
      if (const uint8_t* bmp = IconStyle::iconForApp(slot.icon)) {
        drawIcon(gfx, bmp, iconX, iconY, iconSize);
      }
    } else {
      // Installed app: a monogram tile (rounded box + first letter at 2x)
      // until the app format grows an icon field.
      const int d = 64;
      const int mx = cx + (side - d) / 2;
      const int my = iconY + (iconSize - d) / 2;
      gfx.drawRoundedRect(mx, my, d, d, 14, 3, true);
      char mono[2] = {slot.label[0] ? slot.label[0] : '?', 0};
      if (mono[0] >= 'a' && mono[0] <= 'z') mono[0] = static_cast<char>(mono[0] - 32);
      gfx.drawTextScaledCentered(kFontBold, mx + d / 2, my + 6, mono, 2);
    }

    // Label inside the box, near the bottom edge.
    const XpFont& f = (i == _sel) ? kFontBold : kFontRegular;
    const int labelY = boxY + boxSide - kTilePad - labelLineH;
    gfx.drawTextCentered(f, cx + side / 2, labelY, slot.label);
  }
}
