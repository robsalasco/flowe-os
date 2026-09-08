#include "WifiScene.h"

#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../ble/CompanionBleService.h"
#include "../net/WifiCreds.h"
#include "AppScenes.h"
#include "WifiTest.h"

namespace {
constexpr int kMarginX = 20;
constexpr int kHeaderH = 46;
constexpr int kRowH = 58;      // bold name + small state line
constexpr int kActionRowH = 44;
constexpr uint32_t kNoticeMs = 6000;

// Espressif auth modes (wifi_auth_mode_t) in plain words.
}  // namespace

const char* WifiScene::signalWord(const int rssi) {
  if (rssi <= -1000) return "";
  if (rssi >= -60) return "strong";
  if (rssi >= -72) return "ok";
  return "weak";
}

const char* WifiScene::authWord(const int auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "-";
  }
}

void WifiScene::onEnter() {
  _view = View::List;
  _sel = 0;
  reload();
  // Back from "Test this network": land on that network's page so the one
  // sentence is in view, instead of on the list (bench, 2026-09-05 00:20).
  if (gWifiTestAtMs && millis() - gWifiTestAtMs < 30000) {
    for (int i = 0; i < _count; i++) {
      if (strcmp(_nets[i].ssid, gWifiTestSsid) == 0) {
        _sel = i;
        _view = View::Detail;
        break;
      }
    }
  }
  markDirty();
}

// Saved networks from the store, each married to the last scan record and
// the last failure. No radio work here: the screen shows what is known.
void WifiScene::reload() {
  _count = 0;
  char last[WifiCreds::kMaxSsid] = {0};
  WifiCreds::lastJoinedSsid(last, sizeof(last));
  char failSsid[WifiCreds::kMaxSsid] = {0};
  int failReason = 0;
  const bool hadFail = WifiCreds::peekFailure(failSsid, sizeof(failSsid), &failReason);

  WifiCreds::Network net;
  for (int slot = 0; WifiCreds::get(slot, &net) && _count < kMaxNets; slot++) {
    Net& n = _nets[_count++];
    snprintf(n.ssid, sizeof(n.ssid), "%s", net.ssid);
    n.rssi = -1000;
    n.auth = -1;
    n.channel = 0;
    n.lastJoined = strcmp(net.ssid, last) == 0;
    n.failed = hadFail && strcmp(net.ssid, failSsid) == 0;
    n.failReason = n.failed ? failReason : 0;
  }

  char scan[512];
  _scanFresh = WifiCreds::loadScan(scan, sizeof(scan)) > 0;
  for (char* line = strtok(scan, "\n"); line; line = strtok(nullptr, "\n")) {
    char* f1 = strchr(line, '\t');
    if (!f1) continue;
    *f1++ = '\0';
    char* f2 = strchr(f1, '\t');
    if (!f2) continue;
    *f2++ = '\0';
    char* f3 = strchr(f2, '\t');
    if (!f3) continue;
    *f3++ = '\0';
    for (int i = 0; i < _count; i++) {
      if (strcmp(_nets[i].ssid, line) == 0 && atoi(f1) > _nets[i].rssi) {
        _nets[i].rssi = atoi(f1);
        _nets[i].auth = atoi(f2);
        _nets[i].channel = atoi(f3);
      }
    }
  }
  if (_sel >= _count + kActions) _sel = 0;
}

const char* WifiScene::stateLine(const Net& n, char* buf, const size_t bufSize) const {
  if (n.failed) {
    const bool wrongPassword = n.failReason == 2 || n.failReason == 15 || n.failReason == 204 || n.failReason == 202;
    snprintf(buf, bufSize, "%s", wrongPassword ? "Password did not work. Fix it in the app." : "Could not join last time.");
    return buf;
  }
  if (n.rssi > -1000) {
    snprintf(buf, bufSize, "In range, %s.%s", signalWord(n.rssi), n.lastJoined ? " Joined last." : "");
    return buf;
  }
  snprintf(buf, bufSize, "%s", _scanFresh ? "Not in range at the last scan." : "No scan yet.");
  return buf;
}

// A short radio swap: BLE down, one scan, BLE back. About five seconds; the
// phone sees a brief drop and reconnects. The screen then shows fresh
// states. (A scan with BLE up does not fit in memory: WiFi.mode(STA) costs
// ~49 KB and the launcher has ~55 KB with BLE up.)
void WifiScene::scanNow() {
  _noticeTitle = "Scanning...";
  _noticeBody = "Bluetooth pauses for a few seconds.";
  _noticeUntilMs = 0;
  _view = View::Notice;
  markDirty();
  SCENES.renderNow();

  COMPANION_BLE.shutdownForTransfer();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  const int found = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  int order[24];
  int n = found < 24 ? found : 24;
  if (n < 0) n = 0;
  for (int i = 0; i < n; i++) order[i] = i;
  for (int i = 1; i < n; i++) {
    int j = i;
    while (j > 0 && WiFi.RSSI(order[j - 1]) < WiFi.RSSI(order[j])) {
      const int t = order[j];
      order[j] = order[j - 1];
      order[j - 1] = t;
      j--;
    }
  }
  char scan[512];
  char seen[384];
  size_t len = 0, seenLen = 0;
  scan[0] = '\0';
  seen[0] = '\0';
  for (int k = 0; k < n; k++) {
    const int i = order[k];
    const String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    // One line per name: a mesh shows the same SSID once per node and band,
    // and the strongest copy (sorted first) is the only one worth listing.
    bool dup = false;
    for (int m = 0; m < k && !dup; m++) dup = WiFi.SSID(order[m]) == s;
    if (dup) continue;
    if (len + 48 < sizeof(scan)) {
      len += (size_t)snprintf(scan + len, sizeof(scan) - len, "%s%s\t%d\t%d\t%d", len ? "\n" : "", s.c_str(), WiFi.RSSI(i),
                              static_cast<int>(WiFi.encryptionType(i)), WiFi.channel(i));
    }
    if (seenLen + 34 < sizeof(seen)) {
      seenLen += (size_t)snprintf(seen + seenLen, sizeof(seen) - seenLen, "%s%s", seenLen ? "\n" : "", s.c_str());
    }
  }
  WiFi.scanDelete();
  WifiCreds::saveScan(scan);
  WifiCreds::saveSeen(seen);
  WiFi.mode(WIFI_OFF);
  delay(100);
  COMPANION_BLE.resumeAfterTransfer(nullptr, nullptr);
  Serial.printf("[xphone-os] wifi: scan from the Wi-Fi screen saw %d networks (heap %u)\n", found, ESP.getFreeHeap());

  reload();
  _view = View::List;
  markDirty();
}

void WifiScene::forgetSelected() {
  if (_sel < 0 || _sel >= _count) return;
  char ssid[64];
  snprintf(ssid, sizeof(ssid), "%s", _nets[_sel].ssid);
  WifiCreds::removeSsid(ssid);
  COMPANION_BLE.sendWifiForgot(ssid);  // or the phone's vault pushes it straight back
  Serial.printf("[xphone-os] wifi: forgot %s\n", ssid);
  reload();
  _view = View::List;
  _noticeTitle = "Forgotten";
  _noticeBody = "The Flowe app forgets it too.";
  _noticeUntilMs = millis() + kNoticeMs;
  _view = View::Notice;
  markDirty();
}

const char* const* WifiScene::softKeys() const {
  static constexpr const char* kListKeys[4] = {"BACK", "OPEN", SoftKey::Up, SoftKey::Down};
  static constexpr const char* kDetailKeys[4] = {"BACK", "TEST", "FORGET", nullptr};
  static constexpr const char* kNoticeKeys[4] = {"BACK", nullptr, nullptr, nullptr};
  switch (_view) {
    case View::List: return kListKeys;
    case View::Detail: return kDetailKeys;
    case View::Notice: return kNoticeKeys;
  }
  return kListKeys;
}

void WifiScene::handleInput(Input& in) {
  switch (_view) {
    case View::List: {
      const int total = _count + kActions;
      if (in.wasPressed(Btn::Back)) {
        showSettings();
        return;
      }
      if (in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) {
        _sel = (_sel + total - 1) % total;
        markDirty();
      }
      if (in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) {
        _sel = (_sel + 1) % total;
        markDirty();
      }
      if (in.wasPressed(Btn::Confirm)) {
        if (_sel < _count) {
          _view = View::Detail;
          markDirty();
        } else if (_sel == _count) {
          scanNow();
        } else if (_sel == _count + 1) {
          showFileTransferAutoStartDirect();  // the hotspot screen with its QR code
          return;
        } else {
          COMPANION_BLE.sendWifiRequest();
          _noticeTitle = "Open the Flowe app";
          _noticeBody = COMPANION_BLE.isConnected() ? "Its Wi-Fi page is opening on your phone."
                                                    : "Your phone is not connected right now.";
          _noticeUntilMs = millis() + kNoticeMs;
          _view = View::Notice;
          markDirty();
        }
      }
      break;
    }
    case View::Detail:
      if (in.wasPressed(Btn::Back)) {
        _view = View::List;
        markDirty();
        return;
      }
      if (in.wasPressed(Btn::Confirm)) {
        // Test: one targeted join in the transfer scene; it comes back here
        // with a sentence (the phone is asked to knock over BLE first).
        gWifiTestMode = true;
        COMPANION_BLE.setTransferTarget(_nets[_sel].ssid, "");
        showFileTransferAutoStart();
        return;
      }
      if (in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) forgetSelected();
      break;
    case View::Notice:
      if (in.wasPressed(Btn::Back) || (_noticeUntilMs && millis() > _noticeUntilMs)) {
        _view = View::List;
        markDirty();
      }
      break;
  }
}

void WifiScene::drawHeader(Gfx& gfx, const char* title, const char* right) {
  gfx.drawText(kFontBold, kMarginX, 8, title);
  if (right && right[0]) {
    gfx.drawText(kFontRegular, gfx.width() - kMarginX - gfx.textWidth(kFontRegular, right), 8, right);
  }
  gfx.fillRect(0, kHeaderH - 2, gfx.width(), 2, true);
}

void WifiScene::render(Gfx& gfx) {
  switch (_view) {
    case View::List: renderList(gfx); break;
    case View::Detail: renderDetail(gfx); break;
    case View::Notice: renderNotice(gfx); break;
  }
}

void WifiScene::renderList(Gfx& gfx) {
  drawHeader(gfx, "Wi-Fi", "Off between syncs");
  const int w = gfx.width();
  int y = kHeaderH + 10;
  char line[96];
  for (int i = 0; i < _count; i++) {
    const bool sel = _sel == i;
    if (sel) gfx.fillRect(0, y, w, kRowH, true);
    gfx.drawText(kFontBold, kMarginX, y + 8, _nets[i].ssid, !sel);
    gfx.drawText(kFontSmall, kMarginX, y + 34, stateLine(_nets[i], line, sizeof(line)), !sel);
    if (_nets[i].rssi > -1000) {
      const char* bars = _nets[i].rssi >= -60 ? "|||" : _nets[i].rssi >= -72 ? "||" : "|";
      gfx.drawText(kFontRegular, w - kMarginX - gfx.textWidth(kFontRegular, bars), y + 8, bars, !sel);
    }
    y += kRowH;
  }
  if (_count == 0) {
    gfx.drawText(kFontRegular, kMarginX, y + 6, "No networks saved yet.");
    gfx.drawText(kFontSmall, kMarginX, y + 32, "Add one from the Flowe app.");
    y += kRowH;
  }
  gfx.fillRect(kMarginX, y + 6, w - 2 * kMarginX, 1, true);
  y += 16;
  static const char* kActionLabels[kActions] = {"Scan again", "Show the hotspot code", "Add a network from the app"};
  for (int a = 0; a < kActions; a++) {
    const bool sel = _sel == _count + a;
    if (sel) gfx.fillRect(0, y, w, kActionRowH, true);
    gfx.drawText(kFontBold, kMarginX, y + 10, kActionLabels[a], !sel);
    y += kActionRowH;
  }
  gfx.drawText(kFontSmall, kMarginX, y + 4, "Opens the Wi-Fi page on your phone.");
  const int h = gfx.height();
  gfx.drawText(kFontSmall, kMarginX, h - 78, "The device joins the network your phone is on.");
  gfx.drawText(kFontSmall, kMarginX, h - 60, "When it cannot, it makes its own hotspot.");
}

void WifiScene::renderDetail(Gfx& gfx) {
  if (_sel < 0 || _sel >= _count) return;
  const Net& n = _nets[_sel];
  drawHeader(gfx, n.ssid, "Saved");
  const int w = gfx.width();
  const int lineH = 30;
  int y = kHeaderH + 16;
  auto row = [&](const char* label, const char* value) {
    gfx.drawText(kFontRegular, kMarginX, y, label);
    gfx.drawText(kFontRegular, w - kMarginX - gfx.textWidth(kFontRegular, value), y, value);
    y += lineH;
  };
  char v[48];
  if (n.rssi > -1000) {
    snprintf(v, sizeof(v), "%s (%d dBm)", signalWord(n.rssi), n.rssi);
    row("Signal", v);
    row("Security", authWord(n.auth));
    snprintf(v, sizeof(v), "%d", n.channel);
    row("Channel", v);
  } else {
    row("Signal", _scanFresh ? "not in range" : "no scan yet");
  }
  row("Joined last", n.lastJoined ? "yes" : "no");
  if (n.failed) {
    snprintf(v, sizeof(v), "%d", n.failReason);
    row("Last join failed, reason", v);
  }
  gfx.fillRect(kMarginX, y + 4, w - 2 * kMarginX, 1, true);
  y += 20;
  gfx.fillRect(0, y, w, kActionRowH, true);
  gfx.drawText(kFontBold, kMarginX, y + 10, "Test this network", false);
  y += kActionRowH + 8;
  gfx.drawText(kFontSmall, kMarginX, y, "Joins, gets an address, waits for your phone");
  gfx.drawText(kFontSmall, kMarginX, y + 16, "to knock, then comes back with one sentence.");
  y += 44;
  gfx.drawText(kFontBold, kMarginX, y, "Forget this network");
  gfx.drawText(kFontSmall, kMarginX, y + 26, "The Flowe app forgets it too.");
  y += 60;
  if (gWifiTestAtMs && strcmp(gWifiTestSsid, n.ssid) == 0) {
    gfx.drawRoundedRect(kMarginX, y, w - 2 * kMarginX, 100, 14, 2, true);
    char when[40];
    const uint32_t ago = (millis() - gWifiTestAtMs) / 1000;
    if (ago < 90) snprintf(when, sizeof(when), "LAST TEST, just now");
    else snprintf(when, sizeof(when), "LAST TEST, %lu min ago", static_cast<unsigned long>(ago / 60));
    gfx.drawText(kFontSmall, kMarginX + 16, y + 10, when);
    // Two lines: "Joined in 3 s at 192.168.4.23. Your phone reached it." does
    // not fit one (bench photo, 2026-09-05 00:30).
    gfx.drawTextWrapped(kFontRegular, kMarginX + 16, y + 32, gWifiTestResult, w - 2 * kMarginX - 32, 2);
  }
}

void WifiScene::renderNotice(Gfx& gfx) {
  drawHeader(gfx, "Wi-Fi", nullptr);
  const int w = gfx.width();
  const int lineBold = gfx.lineHeight(kFontBold);
  gfx.drawTextCentered(kFontBold, w / 2, gfx.height() / 2 - lineBold, _noticeTitle);
  gfx.drawTextCentered(kFontRegular, w / 2, gfx.height() / 2 + 6, _noticeBody);
}
