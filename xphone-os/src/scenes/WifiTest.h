#pragma once
// The device Wi-Fi screen's "Test this network" (2026-09-04): the transfer
// scene runs one targeted join in test mode and reports one sentence.
#include <cstdint>
extern bool gWifiTestMode;          // set by WifiScene before showFileTransferAutoStart()
extern char gWifiTestResult[96];    // the sentence shown in the detail view
extern uint32_t gWifiTestAtMs;      // millis() when the last test finished (0 = none)
extern char gWifiTestSsid[64];
