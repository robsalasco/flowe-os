#pragma once
// Loop-task stack probe (2026-09-07). Two "Stack protection fault" panics
// in one night, both in loopTask right after the BLE-down -> Wi-Fi-up
// stretch, and the 60 s stats line showed loopHWM=48 B after every Wi-Fi
// session: the 8 KB loop stack is spent to the last 48 bytes somewhere in
// a session, and an interrupt landing at that moment overflows it. This
// probe logs each NEW low with a name, so one session names the frame.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

inline void stackProbe(const char* where) {
  static UBaseType_t lowest = 0xFFFFFFFFu;
  const UBaseType_t left = uxTaskGetStackHighWaterMark(nullptr);
  if (left < lowest) {
    lowest = left;
    Serial.printf("[xphone-os] stack: loop low %u B after %s\n", static_cast<unsigned>(left), where);
  }
}
