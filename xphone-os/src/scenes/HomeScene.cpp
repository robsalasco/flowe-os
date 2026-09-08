#include "HomeScene.h"

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <Preferences.h>
#include <SDCardManager.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "../BatteryGauge.h"
#include "../BlockStatusStore.h"
#include "../Fonts.h"
#include "../Gfx.h"
#include "../IconStyle.h"
#include "../PrioritiesStore.h"
#include "../StatusBar.h"
#include "../TodayStore.h"
#include "../art/LauncherIcons.h"
#include "../ble/CompanionBleService.h"
#include "../reader/FbpBook.h"
#include "../reader/ReadingStats.h"
#include "../reader/Epub.h"
#include "../reader/ReaderSettings.h"
#include "AppScenes.h"

namespace {

// Same NVS namespace the reader uses for its resume pointer.
constexpr const char* kPrefsNamespace = "xphone";
constexpr const char* kPrefsBookKey = "rdBook";     // ReaderScene's key, read-only here
constexpr const char* kPrefsLayoutKey = "homeLay";   // 0 = Tiles, 1 = Widget
constexpr const char* kPrefsTilesKey = "homeTiles";   // csv of builtin ids
constexpr const char* kPrefsWidgetsKey = "homeWdgts"; // csv of builtin ids
constexpr const char* kPrefsSlotsKey = "homeSlots";   // csv, launcher slots
constexpr const char* kDefaultSlots = "today,notifications,priorities,block,read,workout";


// Geometry (logical portrait; both panels). Same margins as the V2 mock,
// with vertical positions derived from the panel so X3 (748 content px)
// and X4 (756) both breathe.
constexpr int kMargin = 20;
constexpr int kStatusH = 38;

BatteryMonitor& battery() {
  static BatteryMonitor mon;
  return mon;
}

// The launcher's icon blitter (LauncherIcons.h format: CLEARED bit = ink),
// with the launcher's nearest-neighbor sampling for sub-native sizes.
void drawIcon(Gfx& gfx, const uint8_t* bitmap, const int x, const int y,
              const int size = XPhoneLauncherIconSize) {
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

// Approved home geometry (2026-09-01 design pass, "B final"). Tiles shrink
// to 78 px icons so the hero owns the page; the dots sit a fixed distance
// above the tile row. All rows derive from the panel height.
int tileIconY(const Gfx& gfx) {
  const int tileH = 78 + 6 + gfx.lineHeight(kFontRegular) + gfx.lineHeight(kFontSmall) + 2;
  return gfx.height() - Scene::SOFTKEY_BAR_H - tileH - 10;
}
int dotsY(const Gfx& gfx) { return tileIconY(gfx) - 34; }

// Stream a cover thumb (.bin, CoverThumb.h format: SET bit = ink) from SD
// straight into the framebuffer — no resident buffer, ~6.5 KB read per
// repaint, and repaints are rare on e-ink. Returns false when the file is
// missing or malformed (caller draws the text-only fallback).
bool streamThumb(Gfx& gfx, const char* path, const int x, const int y, uint16_t* wOut,
                 uint16_t* hOut) {
  FsFile f = SdMan.open(path, O_RDONLY);
  if (!f) return false;
  uint8_t head[8];
  if (f.read(head, 8) != 8 || head[0] != 0x54 || head[1] != 0x58 || head[2] != 1) {
    f.close();
    return false;
  }
  const uint16_t w = static_cast<uint16_t>(head[4] | (head[5] << 8));
  const uint16_t h = static_cast<uint16_t>(head[6] | (head[7] << 8));
  if (w == 0 || w > 400 || h == 0 || h > 520) {
    f.close();
    return false;
  }
  const int rowBytes = (w + 7) / 8;
  uint8_t row[64];  // 400 px max -> 50 bytes
  for (int ry = 0; ry < h; ++ry) {
    if (f.read(row, rowBytes) != rowBytes) break;
    for (int rx = 0; rx < w; ++rx) {
      if ((row[rx >> 3] >> (7 - (rx & 7))) & 1) gfx.drawPixel(x + rx, y + ry, true);
    }
  }
  f.close();
  *wOut = w;
  *hOut = h;
  return true;
}

}  // namespace

// ---------------------------------------------------------------- layout pref

HomeLayout homeLayout() {
  // Tiles is the default for 0.7 (Andrew, 2026-09-06 14:27: "ship it with the
  // tile grid for now"). The Widget home stays in the code for the
  // Customization thread; a stored choice or the phone's home.layout card
  // still selects it.
  if (!kExploreHomeApps) return HomeLayout::Tiles;  // the stored choice waits for the exploration build
  Preferences prefs;
  uint8_t v = 0;
  if (prefs.begin(kPrefsNamespace, /*readOnly=*/true)) {
    v = prefs.getUChar(kPrefsLayoutKey, 0);
    prefs.end();
  }
  return v == 0 ? HomeLayout::Tiles : HomeLayout::Widget;
}

void setHomeLayout(HomeLayout l) {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, /*readOnly=*/false)) {
    prefs.putUChar(kPrefsLayoutKey, static_cast<uint8_t>(l));
    prefs.end();
  }
}

namespace {
void putCsv(const char* key, const char* csv) {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, /*readOnly=*/false)) {
    prefs.putString(key, csv);
    prefs.end();
  }
}
}  // namespace

void setHomeTiles(const char* csv) { putCsv(kPrefsTilesKey, csv); }
void setHomeWidgets(const char* csv) { putCsv(kPrefsWidgetsKey, csv); }
void setHomeSlots(const char* csv) { putCsv(kPrefsSlotsKey, csv); }
const char* kDefaultSlotsCsv() { return kDefaultSlots; }

void homeSlotsCsv(char* out, size_t n) {
  out[0] = 0;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, /*readOnly=*/true)) {
    prefs.getString(kPrefsSlotsKey, out, n);
    prefs.end();
  }
  if (!out[0]) snprintf(out, n, "%s", kDefaultSlots);
}

// ---------------------------------------------------------------- lifecycle

void HomeScene::loadConfig() {
  char tiles[96] = {0}, widgets[96] = {0};
  {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/true)) {
      prefs.getString(kPrefsTilesKey, tiles, sizeof(tiles));
      prefs.getString(kPrefsWidgetsKey, widgets, sizeof(widgets));
      prefs.end();
    }
  }
  // Tiles: builtin id -> launcher icon index (LauncherScene app order).
  struct IdIcon { const char* id; uint8_t icon; };
  static constexpr IdIcon kIcons[] = {{"today", 0},  {"notifications", 1}, {"priorities", 2},
                                      {"block", 3},  {"read", 4},          {"workout", 5}};
  int t = 0;
  char* save = nullptr;
  for (char* tok = strtok_r(tiles, ",", &save); tok && t < 2;
       tok = strtok_r(nullptr, ",", &save)) {
    for (const IdIcon& e : kIcons) {
      if (!strcmp(tok, e.id)) {
        snprintf(_tileId[t], sizeof(_tileId[t]), "%s", e.id);
        _tileIcon[t] = e.icon;
        ++t;
        break;
      }
    }
  }
  if (t == 0) {  // defaults: Block, Today
    snprintf(_tileId[0], sizeof(_tileId[0]), "block");
    _tileIcon[0] = 3;
    snprintf(_tileId[1], sizeof(_tileId[1]), "today");
    _tileIcon[1] = 0;
  } else if (t == 1) {
    snprintf(_tileId[1], sizeof(_tileId[1]), "today");
    _tileIcon[1] = 0;
  }
  // Widget pages: only ids with a hero renderer today.
  int n = 0;
  save = nullptr;
  for (char* tok = strtok_r(widgets, ",", &save); tok && n < kMaxPages;
       tok = strtok_r(nullptr, ",", &save)) {
    if (!strcmp(tok, "read")) _pages[n++] = PageId::Read;
    else if (!strcmp(tok, "priorities")) _pages[n++] = PageId::Priorities;
    else if (!strcmp(tok, "today")) _pages[n++] = PageId::Today;
  }
  if (n == 0) {
    _pages[0] = PageId::Read;
    _pages[1] = PageId::Priorities;
    n = 2;
  }
  _pageCount = n;
  if (_page >= _pageCount) _page = 0;
}

void HomeScene::onEnter() {
  loadConfig();
  loadReadHero();
  _seenPrioRev = PRIORITIES_STORE.revision();
  _seenBlockRev = BLOCK_STATUS.get().revision;
  markDirty();
}

void HomeScene::onExit() {}

const char* const* HomeScene::softKeys() const {
  static constexpr const char* kKeys[4] = {"APPS", "OPEN", SoftKey::Left, SoftKey::Right};
  return kKeys;
}

bool HomeScene::blockOverride() const { return BLOCK_STATUS.get().active; }

void HomeScene::loadReadHero() {
  _read.valid = false;
  _read.thumbPath[0] = 0;
  char path[160] = {0};
  {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/true)) {
      prefs.getString(kPrefsBookKey, path, sizeof(path));
      prefs.end();
    }
  }
  if (path[0] == '\0') return;
  if (!SdMan.ready() && !SdMan.begin()) return;
  if (!SdMan.exists(path)) return;

  // Metadata without opening the book. FBP packages (the common case —
  // every phone-synced book) answer from their header via readMeta; EPUBs
  // from the cached book.bin (~43 ms; never build here). The cover is the
  // unified "<path>.cov" sidecar both formats publish (XT bin format), with
  // the EPUB cache thumb as the fallback for pre-sidecar books.
  const char* dot = strrchr(path, '.');
  const bool isFbp = dot && strcasecmp(dot, ".fbp") == 0;
  if (isFbp) {
    if (!reader::FbpBook::readMeta(path, _read.title, sizeof(_read.title), _read.author,
                                   sizeof(_read.author)))
      return;
    if (_read.title[0] == '\0')
      snprintf(_read.title, sizeof(_read.title), "%s",
               strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
  } else {
    const std::unique_ptr<reader::Epub> epub(new (std::nothrow)
                                                 reader::Epub(path, reader::kReaderCacheRoot));
    if (!epub || !epub->load(/*buildIfMissing=*/false)) return;
    if (epub->getTitle().empty()) {
      snprintf(_read.title, sizeof(_read.title), "%s",
               strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    } else {
      snprintf(_read.title, sizeof(_read.title), "%s", epub->getTitle().c_str());
    }
    snprintf(_read.author, sizeof(_read.author), "%s", epub->getAuthor().c_str());
    const std::string thumb = epub->getCachePath() + "/cover_200x260.bin";
    if (SdMan.exists(thumb.c_str()) && thumb.size() < sizeof(_read.thumbPath))
      memcpy(_read.thumbPath, thumb.c_str(), thumb.size() + 1);
  }
  char cov[176];
  if (snprintf(cov, sizeof(cov), "%s.cov", path) < static_cast<int>(sizeof(cov)) &&
      SdMan.exists(cov)) {
    snprintf(_read.thumbPath, sizeof(_read.thumbPath), "%s", cov);
  }

  // Progress percent from the .pos sidecar (page + count trailer).
  char side[176];
  snprintf(side, sizeof(side), "%s.pos", path);
  FsFile pf = SdMan.open(side, O_RDONLY);
  if (pf) {
    uint32_t page = 0, count = 0;
    pf.read(&page, 4);
    const bool hasCount = pf.read(&count, 4) == 4;
    pf.close();
    if (hasCount && count > 0 && page < count) {
      uint32_t pct = static_cast<uint32_t>((static_cast<uint64_t>(page + 1) * 100) / count);
      if (pct > 100) pct = 100;
      if (pct == 0) pct = 1;
      _read.pct = static_cast<uint8_t>(pct);
    }
  }

  const reader::ReadingStats::Band band = reader::ReadingStats::band();
  _read.streakDays = band.clockValid ? band.streakDays : 0;
  _read.todayMinutes = reader::ReadingStats::todayMinutes();
  _read.valid = true;
}

// ---------------------------------------------------------------- input

void HomeScene::handleInput(Input& in) {
  if (in.wasPressed(Btn::Back)) {
    showLauncher();  // APPS: the grid lives behind the home screen
    return;
  }
  if (in.wasPressed(Btn::Confirm)) {
    if (blockOverride()) {
      showBlock();
    } else if (_pages[_page] == PageId::Read) {
      showReader();
    } else if (_pages[_page] == PageId::Today) {
      showToday();
    } else {
      showPriorities();
    }
    return;
  }
  const bool left = in.wasPressed(Btn::Left);
  const bool right = in.wasPressed(Btn::Right);
  if ((left || right) && !blockOverride()) {
    _page = (_page + (right ? 1 : _pageCount - 1)) % _pageCount;
    if (_pages[_page] == PageId::Read) loadReadHero();  // fresh numbers
    markDirty();
  }

  // Card revisions: repaint when the data behind the visible hero moved.
  const uint32_t prioRev = PRIORITIES_STORE.revision();
  const uint32_t blockRev = BLOCK_STATUS.get().revision;
  if (prioRev != _seenPrioRev || blockRev != _seenBlockRev) {
    _seenPrioRev = prioRev;
    _seenBlockRev = blockRev;
    markDirty();
  }
}

// ---------------------------------------------------------------- render

void HomeScene::render(Gfx& gfx) {
  renderStatusStrip(gfx);
  if (blockOverride()) {
    renderBlockHero(gfx);
  } else {
    if (_pages[_page] == PageId::Read) {
      renderReadHero(gfx);
    } else if (_pages[_page] == PageId::Today) {
      renderTodayHero(gfx);
    } else {
      renderPrioritiesHero(gfx);
    }
    renderDots(gfx);
  }
  renderTiles(gfx);
}

void HomeScene::renderStatusStrip(Gfx& gfx) const {
  // The launcher's brand lockup, verbatim (LauncherScene::render): half sun
  // on the horizon, ripple bars, wordmark. Battery cluster right, BLE dot.
  constexpr int kSunD = 22;
  const int sunX = 16;
  const int sunTop = 6;
  const int horizonY = sunTop + kSunD / 2;
  gfx.fillRoundedRect(sunX, sunTop, kSunD, kSunD, kSunD / 2, true);
  gfx.fillRect(sunX - 2, horizonY, kSunD + 4, kSunD / 2 + 2, false);
  const int sunCx = sunX + kSunD / 2;
  gfx.fillRect(sunCx - (kSunD + 4) / 2, horizonY + 3, kSunD + 4, 2, true);
  gfx.fillRect(sunCx - (kSunD - 8) / 2, horizonY + 8, kSunD - 8, 2, true);
  gfx.fillRect(sunCx - (kSunD - 16) / 2, horizonY + 13, kSunD - 16, 2, true);
  gfx.drawText(kFontBold, sunX + kSunD + 10, 4, "flowe");

  uint16_t pct = 0;
  const bool havePct = battery().readPercentageChecked(pct);
  int16_t avgMa = 0;
  const bool charging = BatteryGauge::readAvgCurrentMa(avgMa) && avgMa > 0;
  const int battLeft =
      StatusBar::drawBattery(gfx, gfx.width() - 16, kStatusH - 2, havePct ? pct : -1, charging);
  if (COMPANION_BLE.isStarted()) {
    const int d = 12;
    const int dotX = battLeft - d - 10;
    const int dotY = (kStatusH - 2 - d) / 2;
    if (COMPANION_BLE.isConnected()) {
      gfx.fillRoundedRect(dotX, dotY, d, d, d / 2, true);
    } else {
      gfx.drawRoundedRect(dotX, dotY, d, d, d / 2, 2, true);
    }
  }
  gfx.fillRect(0, kStatusH, gfx.width(), 1, true);
}

void HomeScene::renderReadHero(Gfx& gfx) const {
  const int w = gfx.width();
  if (!_read.valid) {
    gfx.drawTextCentered(kFontBold, w / 2, 200, "No book yet");
    gfx.drawTextCentered(kFontRegular, w / 2, 250, "Open Read to pick one.");
    return;
  }

  // Title across the top, always one line: bold if it fits, then regular,
  // then ellipsized regular. (Approved 2026-09-01.)
  const int titleW = w - 40;
  const XpFont* tf = &kFontBold;
  char title[64];
  snprintf(title, sizeof(title), "%s", _read.title);
  if (gfx.textWidth(*tf, title) > titleW) tf = &kFontRegular;
  if (gfx.textWidth(*tf, title) > titleW) {
    size_t len = strlen(title);
    while (len > 1) {
      title[len--] = 0;
      char probe[68];
      snprintf(probe, sizeof(probe), "%s...", title);
      if (gfx.textWidth(*tf, probe) <= titleW) {
        snprintf(title, sizeof(title), "%s", probe);
        break;
      }
    }
  }
  gfx.drawText(*tf, 20, 48, title);
  const int top = 48 + gfx.lineHeight(*tf) + 12;

  // Column width = the widest stat at 2x (or the widest label).
  char vals[3][12];
  snprintf(vals[0], sizeof(vals[0]), _read.pct ? "%u%%" : "--", _read.pct);
  snprintf(vals[1], sizeof(vals[1]), "%um", _read.todayMinutes);
  snprintf(vals[2], sizeof(vals[2]), "%ud", _read.streakDays);
  static constexpr const char* kLabels[3] = {"READ", "TODAY", "STREAK"};
  int colW = gfx.textWidth(kFontSmall, "STREAK");
  for (const auto& v : vals) {
    const int vw = gfx.textWidthScaled(kFontBold, v, 2);
    if (vw > colW) colW = vw;
  }

  // The cover slot takes everything else, capped by the dot row and the
  // 1:1.3 thumb aspect.
  const int availH = dotsY(gfx) - 16 - top;
  const int slotW = w - 40 - 24 - colW;
  const int slotH = availH < slotW * 13 / 10 ? availH : slotW * 13 / 10;

  uint16_t cw = 0, ch = 0;
  bool haveCover = false;
  int coverX = 20, coverY = top;
  if (_read.thumbPath[0]) {
    // Letterbox: today's 200x260 sidecars center inside the slot; a
    // hero-size cover fills it. The slot outline is drawn only when the
    // art undershoots, so the intended footprint stays visible.
    uint16_t tw = 0, th = 0;
    FsFile probe = SdMan.open(_read.thumbPath, O_RDONLY);
    uint8_t head[8];
    if (probe && probe.read(head, 8) == 8 && head[0] == 0x54 && head[1] == 0x58) {
      tw = static_cast<uint16_t>(head[4] | (head[5] << 8));
      th = static_cast<uint16_t>(head[6] | (head[7] << 8));
    }
    if (probe) probe.close();
    if (tw > 0 && th > 0) {
      coverX = 20 + (slotW - tw) / 2;
      coverY = top + (slotH - th) / 2;
      if (coverX < 20) coverX = 20;
      if (coverY < top) coverY = top;
      haveCover = streamThumb(gfx, _read.thumbPath, coverX, coverY, &cw, &ch);
    }
  }
  if (haveCover) {
    if (cw + 20 < slotW || ch + 20 < slotH)
      gfx.drawRect(18, top - 2, slotW + 4, slotH + 4, 1, true);  // the slot
    gfx.drawRect(coverX - 2, coverY - 2, cw + 4, ch + 4, 2, true);
  } else {
    gfx.drawRect(18, top - 2, slotW + 4, slotH + 4, 2, true);
    gfx.drawTextWrapped(kFontBold, 36, top + 40, _read.title, slotW - 32, 4);
    if (_read.author[0])
      gfx.drawTextWrapped(kFontRegular, 36, top + slotH - 80, _read.author, slotW - 32, 2);
  }

  // Right column: author words stacked in caps at the top, stats
  // bottom-anchored so the column ends where the cover slot ends.
  const int sx = 20 + slotW + 24;
  int yy = top + 2;
  if (_read.author[0]) {
    char word[24];
    const char* pch = _read.author;
    while (*pch && yy < top + 120) {
      size_t k = 0;
      while (*pch == ' ') ++pch;
      while (*pch && *pch != ' ' && k < sizeof(word) - 1)
        word[k++] = static_cast<char>(toupper(*pch++));
      word[k] = 0;
      if (!k) break;
      // A word wider than the column ellipsizes rather than bleeding
      // off the panel (seen on glass with "WOODWORTH", 2026-09-01).
      const int wordMax = w - 20 - sx;
      if (gfx.textWidth(kFontSmall, word) > wordMax) {
        while (k > 1) {
          word[--k] = 0;
          char probe[28];
          snprintf(probe, sizeof(probe), "%s.", word);
          if (gfx.textWidth(kFontSmall, probe) <= wordMax) {
            snprintf(word, sizeof(word), "%s", probe);
            break;
          }
        }
      }
      gfx.drawText(kFontSmall, sx, yy, word);
      yy += gfx.lineHeight(kFontSmall);
    }
  }
  const int blockH = 90 + gfx.lineHeight(kFontSmall);
  int gap = (top + slotH - yy - 3 * blockH - 10) / 2;
  if (gap < 8) gap = 8;
  int by = top + slotH - 3 * blockH - 2 * gap;
  if (by < yy + 10) by = yy + 10;
  for (int i = 0; i < 3; ++i) {
    gfx.drawTextScaled(kFontBold, sx, by, vals[i], 2);
    gfx.drawText(kFontSmall, sx, by + 90, kLabels[i]);
    by += blockH + gap;
  }
}

void HomeScene::renderPrioritiesHero(Gfx& gfx) const {
  const int w = gfx.width();
  gfx.drawText(kFontSmall, kMargin, 56, "TODAY'S PRIORITIES");
  const int count = static_cast<int>(PRIORITIES_STORE.count());
  if (count == 0) {
    gfx.drawTextCentered(kFontRegular, w / 2, 220, "Nothing yet.");
    gfx.drawTextCentered(kFontSmall, w / 2, 264, "Speak your morning into the app.");
    return;
  }
  int done = 0;
  const int rowH = 66;
  int y = 110;
  const int maxRows = 5;
  for (int i = 0; i < count; ++i) {
    PrioritiesStore::Item item;
    if (!PRIORITIES_STORE.get(static_cast<std::size_t>(i), item)) break;
    if (item.done) ++done;
    if (i >= maxRows) continue;
    gfx.drawRect(kMargin + 4, y + 2, 26, 26, 2, true);
    if (item.done) gfx.fillRect(kMargin + 10, y + 8, 14, 14, true);
    char title[64];
    snprintf(title, sizeof(title), "%s", item.title);
    gfx.drawText(item.done ? kFontRegular : kFontBold, kMargin + 50, y, title);
    y += rowH;
  }
  char foot[40];
  if (count > maxRows) {
    snprintf(foot, sizeof(foot), "%d of %d done  -  +%d more", done, count, count - maxRows);
  } else {
    snprintf(foot, sizeof(foot), "%d of %d done", done, count);
  }
  gfx.drawText(kFontSmall, kMargin, y + 12, foot);
}

void HomeScene::renderTodayHero(Gfx& gfx) const {
  const int w = gfx.width();
  gfx.drawText(kFontSmall, kMargin, 56, "TODAY");
  const char* weather = TODAY_STORE.weather();
  if (weather && weather[0])
    gfx.drawText(kFontSmall, w - kMargin - gfx.textWidth(kFontSmall, weather), 56, weather);
  const int count = static_cast<int>(TODAY_STORE.count());
  if (count == 0) {
    gfx.drawTextCentered(kFontBold, w / 2, 210, "All clear");
    gfx.drawTextCentered(kFontRegular, w / 2, 254, "Nothing on the calendar today.");
    return;
  }
  const int rowH = 62;
  const int maxRows = 6;
  int y = 104;
  for (int i = 0; i < count && i < maxRows; ++i) {
    TodayStore::Item item;
    if (!TODAY_STORE.get(static_cast<std::size_t>(i), item)) break;
    if (item.reminder) {
      gfx.drawRect(kMargin + 4, y + 4, 22, 22, 2, true);
    } else {
      gfx.drawText(kFontSmall, kMargin, y + 4, item.time[0] ? item.time : "");
    }
    char title[64];
    snprintf(title, sizeof(title), "%s", item.title);
    // Truncate to the column so a long event never bleeds off the panel.
    const int maxW = w - kMargin - (kMargin + 96);
    while (title[0] && gfx.textWidth(kFontRegular, title) > maxW) title[strlen(title) - 1] = 0;
    gfx.drawText(kFontRegular, kMargin + 96, y, title);
    y += rowH;
  }
  if (count > maxRows) {
    char more[24];
    snprintf(more, sizeof(more), "+%d more", count - maxRows);
    gfx.drawText(kFontSmall, kMargin, y + 8, more);
  }
}

bool HomeScene::renderDormant(Gfx& gfx) {
  if (blockOverride()) return false;  // the poster's block line says it
  if (_pages[_page] != PageId::Read) return false;
  loadReadHero();
  if (!_read.valid) return false;
  renderReadHero(gfx);
  return true;
}

void HomeScene::renderBlockHero(Gfx& gfx) const {
  const BlockStatusStore::Status s = BLOCK_STATUS.get();
  const int w = gfx.width();
  char eyebrow[48];
  snprintf(eyebrow, sizeof(eyebrow), "BLOCK%s%s", s.preset[0] ? " - " : "", s.preset);
  for (char* p = eyebrow; *p; ++p) *p = static_cast<char>(toupper(*p));
  gfx.drawText(kFontSmall, kMargin, 56, eyebrow);

  // Minutes, not MM:SS — the phone reports minutes and there is no RTC.
  char big[16];
  snprintf(big, sizeof(big), "%d", s.remainingMinutes);
  gfx.drawTextScaledCentered(kFontBold, w / 2, 150, big, 4);
  gfx.drawTextCentered(kFontRegular, w / 2, 300, "minutes left");
  if (s.endsAtLabel[0]) {
    char until[32];
    snprintf(until, sizeof(until), "until %s", s.endsAtLabel);
    gfx.drawTextCentered(kFontRegular, w / 2, 344, until);
  }
  char sub[48];
  snprintf(sub, sizeof(sub), "%d today  -  streak %d", s.blocksToday, s.streak);
  gfx.drawTextCentered(kFontSmall, w / 2, 392, sub);

  if (s.durationMinutes > 0 && s.remainingMinutes <= s.durationMinutes) {
    const int barY = 440;
    const int done = s.durationMinutes - s.remainingMinutes;
    gfx.drawRect(84, barY, w - 168, 12, 2, true);
    gfx.fillRect(88, barY + 4, (w - 176) * done / s.durationMinutes, 4, true);
  }
}

void HomeScene::renderDots(Gfx& gfx) const {
  const int y = dotsY(gfx);
  const int cx = gfx.width() / 2;
  const int startX = cx - (_pageCount * 10 + (_pageCount - 1) * 14) / 2;
  for (int i = 0; i < _pageCount; ++i) {
    const int x = startX + i * 24;
    if (i == _page) {
      gfx.fillRoundedRect(x, y, 10, 10, 5, true);
    } else {
      gfx.drawRoundedRect(x, y, 10, 10, 5, 1, true);
    }
  }
}

void HomeScene::renderTiles(Gfx& gfx) const {
  const int w = gfx.width();
  const int iconY = tileIconY(gfx);
  const int icon = 78;
  const int side = 160;
  const int gap = 24;
  const int gridX = (w - (2 * side + gap)) / 2;

  struct Tile {
    char name[16];
    int iconIndex;
    char status[32];
  } tiles[2];
  static constexpr const char* kNames[] = {"Today", "Notifications", "Priorities",
                                           "Block", "Read", "Workout"};
  for (int i = 0; i < 2; ++i) {
    tiles[i].iconIndex = _tileIcon[i];
    snprintf(tiles[i].name, sizeof(tiles[i].name), "%s", kNames[_tileIcon[i]]);
    tiles[i].status[0] = 0;
    if (!strcmp(_tileId[i], "block")) {
      const BlockStatusStore::Status bs = BLOCK_STATUS.get();
      if (bs.streak > 0) snprintf(tiles[i].status, sizeof(tiles[i].status), "streak %dd", bs.streak);
    } else if (!strcmp(_tileId[i], "today")) {
      const std::size_t events = TODAY_STORE.count();
      if (events > 0)
        snprintf(tiles[i].status, sizeof(tiles[i].status), "%u today",
                 static_cast<unsigned>(events));
    }
  }

  for (int i = 0; i < 2; ++i) {
    const int tx = gridX + i * (side + gap);
    if (const uint8_t* bmp = IconStyle::iconForApp(tiles[i].iconIndex)) {
      drawIcon(gfx, bmp, tx + (side - icon) / 2, iconY, icon);
    }
    const int labelY = iconY + icon + 6;
    gfx.drawTextCentered(kFontRegular, tx + side / 2, labelY, tiles[i].name);
    if (tiles[i].status[0])
      gfx.drawTextCentered(kFontSmall, tx + side / 2, labelY + gfx.lineHeight(kFontRegular) + 2,
                           tiles[i].status);
  }
}

void HomeScene::debugDump() {
  loadReadHero();
  Serial.printf("[xphone-os] home: valid=%d title=\"%s\" author=\"%s\" pct=%u today=%u streak=%u thumb=%s page=%d block=%d\n",
                _read.valid ? 1 : 0, _read.title, _read.author, _read.pct, _read.todayMinutes,
                _read.streakDays, _read.thumbPath[0] ? _read.thumbPath : "-", _page,
                blockOverride() ? 1 : 0);
}
