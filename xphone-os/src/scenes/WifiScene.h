#pragma once

// xphone-os — the device's Wi-Fi screen (roadmap E1, 2026-09-04).
//
// Three buttons and e-ink are enough for a good Wi-Fi screen when it never
// asks the user to type. It shows what the device knows and reports in plain
// words: every saved network with its state (in range + signal, not in
// range, password did not work), then three actions: Scan again (a short
// radio swap: BLE down, one scan, BLE back), Show the hotspot code (the
// transfer scene's QR screen), Add a network from the app (a BLE nudge that
// opens the phone's Wi-Fi page). A network's detail view has the facts and
// Forget (the phone is told, so its vault drops it too).
//
// Design source: tools/glass-twin/specs/wifi-list.json and wifi-detail.json.

#include "../Scene.h"

class WifiScene : public Scene {
 public:
  void onEnter() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

 private:
  enum class View : uint8_t { List, Detail, Notice };
  static constexpr int kMaxNets = 8;
  static constexpr int kActions = 3;  // Scan again, Show the hotspot code, Add from app

  struct Net {
    char ssid[64];
    int rssi = -1000;   // -1000 = not in the last scan
    int auth = -1;
    int channel = 0;
    bool lastJoined = false;
    bool failed = false;
    int failReason = 0;
  };

  void reload();
  void scanNow();
  void forgetSelected();
  void drawHeader(Gfx& gfx, const char* title, const char* right);
  void renderList(Gfx& gfx);
  void renderDetail(Gfx& gfx);
  void renderNotice(Gfx& gfx);
  const char* stateLine(const Net& n, char* buf, size_t bufSize) const;
  static const char* signalWord(int rssi);
  static const char* authWord(int auth);

  View _view = View::List;
  int _sel = 0;          // 0..count-1 networks, then actions
  int _count = 0;
  Net _nets[kMaxNets];
  bool _scanFresh = false;  // a scan record exists
  const char* _noticeTitle = "";
  const char* _noticeBody = "";
  uint32_t _noticeUntilMs = 0;
};
