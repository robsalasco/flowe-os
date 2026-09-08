#include "NotificationsScene.h"

#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../NotificationStore.h"
#include "../ble/CompanionAncsClient.h"  // appDisplayName(bundle id -> "Messages")
#include "AppScenes.h"

namespace {
constexpr int kMarginX = 20;
constexpr int kHeaderH = 46;
constexpr int kListTopPad = 8;  // gap between the header rule and row 0

// Copy `src` into `dst`, chopping whole UTF-8 sequences and appending "..."
// until it fits in `maxWidth` pixels. Fixed buffers + snprintf only.
void truncateToWidth(Gfx& gfx, const XpFont& font, const char* src, int maxWidth, char* dst, size_t dstSize) {
  snprintf(dst, dstSize, "%s", src ? src : "");
  if (gfx.textWidth(font, dst) <= maxWidth) return;

  size_t len = strlen(dst);
  while (len > 0) {
    // Strip one UTF-8 sequence (back over continuation bytes 0b10xxxxxx).
    do {
      len--;
    } while (len > 0 && (static_cast<uint8_t>(dst[len]) & 0xC0) == 0x80);
    dst[len] = '\0';
    char probe[192];
    snprintf(probe, sizeof(probe), "%s...", dst);
    if (gfx.textWidth(font, probe) <= maxWidth) {
      snprintf(dst, dstSize, "%s", probe);
      return;
    }
  }
}

// One list row: bold title line + regular message line + padding/separator.
int rowHeight(Gfx& gfx) { return gfx.lineHeight(kFontBold) + gfx.lineHeight(kFontRegular) + 14; }

constexpr uint32_t kSyncFlashMs = 1200;  // "SYNCING..." feedback duration
}  // namespace

void NotificationsScene::onEnter() {
  _view = View::List;
  _sel = 0;
  _scroll = 0;
  _syncFlashUntilMs = 0;
  // Open-triggers-sync, matching the card scenes' onEnter: show what the
  // store already has instantly, and kick a fresh ANCS replay so anything
  // missed mid-session (dropped burst, timed-out fetch) lands within a few
  // seconds — rows repaint live via the store-revision pump in main.cpp.
  // A replay that actually starts stamps the header's "synced" clock.
  if (COMPANION_ANCS.requestResync()) _lastSyncMs = millis();
}

// The one manual sync entry point (flowe-os#40, heyflorin's design): the
// key whose tab reads SYNC. Only a replay that actually starts flashes
// "SYNCING..." and stamps the clock — with no phone connected the header
// keeps saying what is true.
void NotificationsScene::syncNow() {
  if (!COMPANION_ANCS.requestResync()) return;
  _lastSyncMs = millis();
  _syncFlashUntilMs = millis() + kSyncFlashMs;
  if (_syncFlashUntilMs == 0) _syncFlashUntilMs = 1;  // 0 means idle
  markDirty(headerRect());
}

const char* const* NotificationsScene::softKeys() const {
  // List: CONFIRM opens the selected row's detail view; its long-press (dot
  // on the tab) clears the whole inbox — CLEAR-the-lot moved off the tap so
  // one stray press can no longer wipe the list.
  //
  // Sync rides the UP key at the top of the list (flowe-os#40, heyflorin):
  // with the cursor on the first row, up has nowhere left to go, so the tab
  // relabels to SYNC and the press refreshes — the button-world cousin of
  // pull-to-refresh. An empty inbox is "at the top" by definition.
  static constexpr const char* kList[4] = {"BACK", "OPEN", SoftKey::Up, SoftKey::Down};
  static constexpr const char* kListTop[4] = {"BACK", "OPEN", "SYNC", SoftKey::Down};
  static constexpr const char* kListEmpty[4] = {"BACK", nullptr, "SYNC", nullptr};
  static constexpr const char* kDetail[4] = {"BACK", "CLEAR", SoftKey::Up, SoftKey::Down};
  if (_view == View::Detail) return kDetail;
  if (NOTIFICATION_STORE.count() == 0) return kListEmpty;
  return _sel == 0 ? kListTop : kList;
}

uint8_t NotificationsScene::longPressSlots() const {
  // Bit 1 marks the list's OPEN tab while there is something to clear
  // (long-press CONFIRM = clear all); none on BACK (see Scene::longPressSlots).
  if (_view == View::List && NOTIFICATION_STORE.count() > 0) return 0x02;
  return 0;
}

int NotificationsScene::rowsPerPage(Gfx& gfx) const {
  const int avail = gfx.height() - kHeaderH - Scene::SOFTKEY_BAR_H - kListTopPad;
  const int rows = avail / rowHeight(gfx);
  return rows > 0 ? rows : 1;
}

XpRect NotificationsScene::listRect() const {
  if (_hCache <= 0) return XpRect{};  // no layout yet -> full-panel fallback
  return XpRect{0, kHeaderH, _wCache, static_cast<int16_t>(_hCache - kHeaderH - Scene::SOFTKEY_BAR_H)};
}

XpRect NotificationsScene::rowRect(const int visibleIndex) const {
  if (_hCache <= 0 || visibleIndex < 0 || visibleIndex >= _rowsPerPageCache) return XpRect{};
  // Row body is 2 text lines + 6px pad (64px); the selection border draws
  // 4px above/around it — 8px slop on every side keeps the rect honest.
  const int rowH = 29 + 29 + 14;  // matches rowHeight() (advanceY is 29 for both 12pt fonts)
  const int y = kHeaderH + kListTopPad + visibleIndex * rowH;
  return XpRect{0, static_cast<int16_t>(y - 8), _wCache, static_cast<int16_t>(rowH + 8)};
}

XpRect NotificationsScene::headerRect() const {
  if (_hCache <= 0) return XpRect{};  // no layout yet -> full-panel fallback
  return XpRect{0, 0, _wCache, kHeaderH};
}

void NotificationsScene::moveSelection(const int delta) {
  const int prev = _sel;
  _sel += delta;
  const int oldScroll = _scroll;
  if (_sel < _scroll) _scroll = _sel;
  if (_sel >= _scroll + _rowsPerPageCache) _scroll = _sel - _rowsPerPageCache + 1;
  if (_scroll != oldScroll) {
    markDirty(listRect());  // rows shifted — repaint the whole list region
    return;
  }
  // Same page: repaint only the two affected rows (the header SYNC pill
  // doesn't depend on _sel while the cursor stays among the rows).
  XpRect dirty = rowRect(prev - _scroll);
  dirty.unionWith(rowRect(_sel - _scroll));
  markDirty(dirty);
}

void NotificationsScene::handleInput(Input& in) {
  const int count = static_cast<int>(NOTIFICATION_STORE.count());
  // Entries may have arrived/expired since the last tick (revision pump) —
  // re-clamp before any index is used.
  if (_sel > count - 1) _sel = count > 0 ? count - 1 : 0;
  if (_scroll > _sel && _sel >= 0) _scroll = _sel;
  if (_scroll < 0) _scroll = 0;

  // Sync-press feedback expiry (input tick, like BlockScene's transients).
  if (_syncFlashUntilMs != 0 && static_cast<int32_t>(millis() - _syncFlashUntilMs) >= 0) {
    _syncFlashUntilMs = 0;
    if (_view == View::List) markDirty(headerRect());
  }

  if (_view == View::Detail) {
    if (count == 0) {  // store emptied under us -> fall back to the (empty) list
      _view = View::List;
      _sel = 0;
      _scroll = 0;
      markDirty();
      return;
    }
    if (in.wasPressed(Btn::Back)) {  // back to the list, selection preserved
      _view = View::List;
      markDirty();
      return;
    }
    // PREV/NEXT: front Left/Right (under the PREV/NEXT tabs) and the top-edge
    // pair. Clamped at the ends — the list doesn't wrap, so neither does this.
    if ((in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) && _sel > 0) {
      _sel--;
      markDirty();  // whole content + header position indicator change
      return;
    }
    if ((in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) && _sel < count - 1) {
      _sel++;
      markDirty();
      return;
    }
    if (in.wasPressed(Btn::Confirm)) {  // CLEAR: remove THIS notification
      NotificationStore::Entry cleared;
      if (NOTIFICATION_STORE.get(static_cast<size_t>(_sel), cleared) &&
          NOTIFICATION_STORE.removeAt(static_cast<size_t>(_sel))) {
        // The local row disappears immediately. A dated date+title tombstone
        // then suppresses/retries any future replay with a fresh session UID.
        // Undated notifications intentionally skip the tombstone (title alone
        // is not a safe identity) but still get this immediate action attempt.
        NOTIFICATION_STORE.recordTombstone(cleared.sortKey, cleared.title);
        COMPANION_ANCS.dismissNotification(cleared.uid, cleared.categoryId, cleared.flags,
                                           cleared.sessionId);
        const int left = count - 1;
        if (left == 0) {  // inbox now empty -> back to the (empty) list
          _view = View::List;
          _sel = 0;
          _scroll = 0;
        } else if (_sel > left - 1) {
          _sel = left - 1;  // removed the oldest -> show the previous one
        }
        // else: _sel now addresses the next (older) notification — stay.
        markDirty();
      }
    }
    return;
  }

  // --- List view -------------------------------------------------------------
  if (in.wasPressed(Btn::Back)) {
    showLauncher();
    return;
  }
  if (in.wasLongPressed(Btn::Confirm) && count > 0) {
    NOTIFICATION_STORE.clearAll();
    _sel = 0;
    _scroll = 0;
    markDirty();
    return;
  }
  if (in.wasPressed(Btn::Confirm)) {
    if (count > 0) {  // OPEN the selected row
      _view = View::Detail;
      markDirty();
    }
    return;
  }
  // Selection: top-edge Up/Down pair AND the front Left/Right buttons — the
  // latter sit directly under the soft-key bar's tabs. UP at the first row
  // (or an empty inbox) is the SYNC press — its tab said so.
  if (in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) {
    if (_sel > 0) {
      moveSelection(-1);
      // Landing on row 0 relabels the up tab to SYNC at the panel's bottom
      // edge — outside moveSelection's two-row window, so repaint it all.
      if (_sel == 0) markDirty();
    } else {
      syncNow();
    }
  }
  if (in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) {
    if (_sel < count - 1) {
      const bool leftTop = _sel == 0;
      moveSelection(+1);
      if (leftTop) markDirty();  // SYNC tab returns to an up arrow
    }
  }
}

void NotificationsScene::render(Gfx& gfx) {
  const int count = static_cast<int>(NOTIFICATION_STORE.count());
  _wCache = static_cast<int16_t>(gfx.width());
  _hCache = static_cast<int16_t>(gfx.height());
  _rowsPerPageCache = rowsPerPage(gfx);
  // Same revision-safety clamps as handleInput (render may run first).
  if (_sel > count - 1) _sel = count > 0 ? count - 1 : 0;
  if (count == 0 && _view == View::Detail) _view = View::List;
  if (_sel >= 0 && _sel < _scroll) _scroll = _sel;
  if (_sel >= _scroll + _rowsPerPageCache) _scroll = _sel - _rowsPerPageCache + 1;
  if (_scroll < 0) _scroll = 0;

  if (_view == View::Detail) {
    renderDetail(gfx, count);
  } else {
    renderList(gfx);
  }
}

void NotificationsScene::renderList(Gfx& gfx) {
  const int w = gfx.width();
  const int count = static_cast<int>(NOTIFICATION_STORE.count());
  const int perPage = _rowsPerPageCache;

  // Header.
  char line[192];
  snprintf(line, sizeof(line), "Notifications (%d)", count);
  gfx.drawText(kFontBold, kMarginX, 8, line);

  // Sync status (top-right, flowe-os#40): quiet words, not a button — the
  // SYNC control lives in the soft-key bar where every other control lives.
  // "SYNCING..." for a beat after the press, then how fresh the list is.
  {
    char ago[24];
    const char* status;
    if (_syncFlashUntilMs != 0) {
      status = "SYNCING...";
    } else if (_lastSyncMs == 0) {
      status = "NOT SYNCED";  // no phone reachable since this power-on
    } else {
      const uint32_t min = (millis() - _lastSyncMs) / 60000u;
      if (min == 0) snprintf(ago, sizeof(ago), "SYNCED JUST NOW");
      else if (min < 60) snprintf(ago, sizeof(ago), "SYNCED %luM AGO", static_cast<unsigned long>(min));
      else snprintf(ago, sizeof(ago), "SYNCED %luH AGO", static_cast<unsigned long>(min / 60));
      status = ago;
    }
    gfx.drawText(kFontSmall, w - kMarginX - gfx.textWidth(kFontSmall, status), 14, status);
  }
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  if (count == 0) {
    gfx.drawTextCentered(kFontBold, w / 2, gfx.height() / 2 - gfx.lineHeight(kFontBold), "No notifications");
    gfx.drawTextCentered(kFontRegular, w / 2, gfx.height() / 2 + 6, "Notifications from iPhone land here");
    return;
  }

  const int textW = w - 2 * kMarginX;
  const int rowH = rowHeight(gfx);
  int y = kHeaderH + kListTopPad;

  NotificationStore::Entry entry;
  for (int i = 0; i < perPage; i++) {
    if (!NOTIFICATION_STORE.get(static_cast<size_t>(_scroll + i), entry)) break;

    // Selection cursor: 3px rounded border around the row's two text lines
    // (text at kMarginX=20 sits 12px inside the border at x=8 — high-contrast
    // per the e-ink rules, and moving it repaints just two rows).
    if (_scroll + i == _sel) {
      const int borderH = gfx.lineHeight(kFontBold) + gfx.lineHeight(kFontRegular) + 10;
      gfx.drawRoundedRect(8, y - 4, w - 16, borderH, 10, 3, true);
    }

    // Line 1 (bold): title, right-aligned app NAME sharing the line. The store
    // keeps the raw bundle id (stable key); resolve it to the friendly iOS name
    // ("Messages") at render — a name landing mid-list just repaints on the next
    // getAppNameRevision() tick (see main.cpp).
    char appName[32];
    COMPANION_ANCS.appDisplayName(entry.appId, appName, sizeof(appName));
    char app[48];
    truncateToWidth(gfx, kFontRegular, appName, textW / 3, app, sizeof(app));
    const int appW = gfx.textWidth(kFontRegular, app);
    char title[96];
    truncateToWidth(gfx, kFontBold, entry.title, textW - appW - 12, title, sizeof(title));
    gfx.drawText(kFontBold, kMarginX, y, title);
    gfx.drawText(kFontRegular, w - kMarginX - appW, y, app);
    y += gfx.lineHeight(kFontBold);

    // Line 2 (regular): message.
    char msg[160];
    truncateToWidth(gfx, kFontRegular, entry.message, textW, msg, sizeof(msg));
    gfx.drawText(kFontRegular, kMarginX, y, msg);
    y += gfx.lineHeight(kFontRegular) + 6;

    if (_scroll + i != _sel) gfx.fillRect(kMarginX, y, textW, 1, true);  // separator
    y += 8;
    if (y + rowH > gfx.height() - Scene::SOFTKEY_BAR_H) break;  // keep clear of chrome
  }
}

void NotificationsScene::renderDetail(Gfx& gfx, const int count) {
  const int w = gfx.width();
  NotificationStore::Entry entry;
  if (!NOTIFICATION_STORE.get(static_cast<size_t>(_sel), entry)) {  // belt-and-braces
    _view = View::List;
    renderList(gfx);
    return;
  }

  // Header: title + "n of m" position in the detail slot.
  gfx.drawText(kFontBold, kMarginX, 8, "Notification");
  char pos[24];
  snprintf(pos, sizeof(pos), "%d of %d", _sel + 1, count);
  gfx.drawText(kFontRegular, w - kMarginX - gfx.textWidth(kFontRegular, pos), 8, pos);
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  const int textW = w - 2 * kMarginX;
  const int bottom = gfx.height() - Scene::SOFTKEY_BAR_H - 6;
  int y = kHeaderH + 10;

  // App name line (regular, one line) + short rule under it.
  char appName[32];
  COMPANION_ANCS.appDisplayName(entry.appId, appName, sizeof(appName));
  char app[48];
  truncateToWidth(gfx, kFontRegular, appName, textW, app, sizeof(app));
  gfx.drawText(kFontRegular, kMarginX, y, app);
  y += gfx.lineHeight(kFontRegular) + 4;
  gfx.fillRect(kMarginX, y, textW, 1, true);
  y += 10;

  // Title: bold, word-wrapped (store field is 56 bytes -> <= 3 lines here).
  const int titleLines =
      gfx.drawTextWrapped(kFontBold, kMarginX, y, entry.title[0] ? entry.title : "(no title)", textW, 3);
  y += (titleLines > 0 ? titleLines : 1) * gfx.lineHeight(kFontBold) + 8;

  // Message: regular, word-wrapped into whatever fits above the soft keys;
  // drawTextWrapped ends the last line with "..." when clipped.
  if (entry.message[0]) {
    const int maxLines = (bottom - y) / gfx.lineHeight(kFontRegular);
    if (maxLines > 0) gfx.drawTextWrapped(kFontRegular, kMarginX, y, entry.message, textW, maxLines);
  }
}
