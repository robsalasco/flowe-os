#pragma once

// xphone-os R2 — File Transfer scene: Wi-Fi on, HTTP server up, books move.
//
// Modeled on CrossPoint's CrossPointWebServerActivity (x4-os
// src/activities/network/CrossPointWebServerActivity.cpp) with the setup
// maze removed: instead of an on-device network picker + password keyboard,
// STA credentials arrive from the Flowe app over BLE (WifiCreds/NVS) and the
// phone can start/stop the whole mode remotely ("transfer.start" command —
// main.cpp pumps it into showFileTransferAutoStart()).
//
// Station mode only: the device joins the user's Wi-Fi. (A softAP/hotspot
// fallback shipped briefly but was unreliable and saved almost no RAM to
// remove-at-compile-time, so it's gone — provisioning happens in the app.)
//
// States: Idle -> Connecting -> Running | Failed. The scene's handleInput
// tick pumps FileTransferServer::handleClient(); all SD access stays on the
// main loop.
//
// Exit (rewritten 2026-09-04, the no-restart exit): every way out of a
// session goes through endSession(): the phone's POST /stop, BLE
// "transfer.stop", the EXIT/BACK key, BACK while joining, the idle and
// hotspot guards, the failed-screen linger, and the OS launcher jump. It
// stops the server, turns Wi-Fi fully off (esp_wifi_deinit), brings BLE
// back in place, and returns to the launcher. Until then every one of
// those paths called esp_restart() (CrossPoint's silentRestart lesson,
// "rebooting defragments after Wi-Fi"), which lost the clock, the card
// stores, and ~15 s of the user's time per sync. The 10-cycle bench in
// docs/plans/2026-09-03-transfer-scope.md is the evidence the heap holds.

#include <WiFiGeneric.h>

#include "../Scene.h"
#include "../net/FileTransferServer.h"

class FileTransferScene : public Scene {
 public:
  void onEnter() override;
  void onExit() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

  // Phone-initiated start (BLE "transfer.start"): skip the Idle menu and
  // bring up STA with saved creds; reports "needs-wifi" if none saved.
  void autoStart();
  // Phone-initiated stop (BLE "transfer.stop"). BLE is only up on the parked
  // screens (Idle, needs-wifi, Failed), so this ends whatever is there.
  void stopSession();

  // W2: phone-initiated Direct mode (BLE "transfer.direct").
  void autoStartDirect();
  // A transfer card arrived while this scene is ALREADY up. Behave exactly
  // like a fresh entry: a live session re-announces itself; anything parked
  // (Idle, needs-wifi, a failure screen) re-runs the start decision. The
  // old behavior — silently ignoring the card — left a reader stuck on the
  // needs-wifi screen eating every start request until a human pressed
  // BACK (found live, 2026-08-23).
  void restartFromCard(bool direct);

  // Sync in place (Andrew, 2026-09-04): a phone-started sync no longer
  // jumps to this page. The scene takes over the loop (it needs the memory
  // the reader held), but never repaints: the previous screen's picture
  // stays on glass and a small pill in the header says "Syncing...". The
  // full page comes up only when a person has to see it (the hotspot's
  // QR code after nobody joined, a failure). At the end the scene we came
  // from returns, the reader quietly (no "Opening book..." blink).
  void beginSilent(uint32_t returnSceneId);
  bool silent() const { return _silent; }
  bool suppressRepaint() const override { return _silent; }

  bool radioActive() const { return _state != State::Idle && _state != State::Failed; }

 private:
  enum class State : uint8_t { Idle, Connecting, Running, Failed };

  void startSta();
  void startAp();            // W2 Direct mode: the device's own hotspot (BLE notice, then raiseHotspot)
  // The radio half of the hotspot: AP up, server up. Also the landing of
  // switchToHotspot(), which arrives with BLE already down.
  void raiseHotspot();
  // Mid-session fallback (2026-09-04): the shared network did not work out
  // (not seen, join failed, or nobody knocked), so become the hotspot in
  // the SAME session. The phone mirrors this after its probe budget.
  void switchToHotspot(const char* why);
  void drawHotspotScreen(Gfx& gfx, int y);
  void pollConnecting();
  void startServerOrFail();
  void exitScene();  // endSession when the radio was ever up, else launcher
  // The one way out of a session: server down, Wi-Fi off, BLE back,
  // launcher. `reason` goes to the log and, as the detail of the queued
  // "stopped", to the phone. queueStopped=false when a failure breadcrumb
  // already waits for the same delivery slot.
  void endSession(const char* reason, bool queueStopped = true, bool backToWifi = false);
  void finishTest(const char* sentence);  // the Wi-Fi screen's "Test this network"
  // Wi-Fi fully off: event hook removed, STA/AP down, esp_wifi_deinit.
  void teardownRadio();

  void tryNextCandidate();   // W1: advance the scan-ordered join list

  void silentTick();                   // per-loop: keep the pill current, escalate when needed
  void paintPill(const char* text);    // the inverted sync bar over the soft keys (2026-09-07)
  void leaveSilent(const char* why);   // show the full page from now on
  bool _silent = false;                // no repaints: the pill over the previous picture
  bool _inPlace = false;               // started from another screen; go back there at the end
  uint32_t _returnSceneId = 0;         // SceneId of the screen we came from
  uint32_t _silentSinceMs = 0;
  char _pillShown[40] = {0};

  State _state = State::Idle;
  bool _radioWasUp = false;  // any WiFi.mode() call happened -> exit tears Wi-Fi down
  wifi_event_id_t _staEventHandle = 0;  // the STA disconnect-reason hook, one per session
  bool _directMode = false;  // W2: we ARE the access point (no STA link)
  // The route the phone asked for (transfer.start "ssid"/"pass"). Given =
  // join that network only; empty = the old walk over every saved network.
  bool _targetGiven = false;
  char _targetSsid[64] = {0};
  char _targetPass[64] = {0};
  bool _targetSessionCreds = false;  // the pass came with the card (the phone's own hotspot), not from the store
  uint32_t _servedSinceMs = 0;   // STA server up; the reach test clock
  uint32_t _lastAnnounceMs = 0;  // phone-hotspot sessions: UDP "here I am" to the gateway
  bool _showApPassword = false;  // hotspot screen: QR by default, text on demand
  int _shownApClients = -1;  // last reported hotspot client count
  uint32_t _apStartedMs = 0;
  uint32_t _apClientSeenMs = 0;
  // Idle guard for the INFRASTRUCTURE (STA) path. The hotspot path has had
  // a no-client watchdog since W2, but a LAN session had none: when the
  // phone died mid-upload the device sat in Wi-Fi mode for ever, with BLE
  // down, so the phone then reported "couldn't reach the device". Found by
  // killing the app mid-send (2026-08-19).
  uint32_t _lastActivityMs = 0;
  uint16_t _lastRequestCount = 0;
  uint32_t _lastMovedBytes = 0;
  char _ssid[64] = {0};
  char _password[64] = {0};
  // W1 scan-then-join: saved networks ordered by the session-start scan
  // (strongest seen first, last-joined tie-break, one hidden-network try).
  int _candidateSlots[8] = {0};  // indexes into WifiCreds::get order
  // Signal of each candidate at scan time, in the same order. Diagnostic
  // only: a 4-way handshake that times out (reason 15) looks exactly like
  // a wrong password in the logs, and the RSSI is what tells the two
  // apart. Added 2026-08-21 chasing an X3 that joined fine on a USB cable
  // and stopped once it moved to the pogo rig.
  int _candidateRssi[8] = {0};
  int _candidateCount = 0;
  int _candidateIdx = 0;
  char _ip[16] = {0};
  const char* _failReason = "";
  // Non-zero only for radio-down failures (join timeout / server start):
  // arms the Failed-state auto-restart that brings BLE back for the phone.
  uint32_t _failedAtMs = 0;
  // When this scene started PARKING (Idle with no session, or the
  // needs-wifi screen). An unattended reader must never sit on a dead-end
  // screen forever; after 2 minutes it returns to the launcher on its own.
  uint32_t _parkedSinceMs = 0;
  uint32_t _connectStartMs = 0;
  uint32_t _lastPollMs = 0;

  // On-glass activity line, refreshed only on meaningful change (e-ink).
  uint16_t _shownRequests = 0;
  uint32_t _shownKb = 0;

  FileTransferServer _server;
};
