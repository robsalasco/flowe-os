#pragma once
#include <Arduino.h>

// M2.1b (power lever 3): CPU clock after boot, MHz. ESP32-C3 BLE requires
// >= 80 MHz (the ESP-IDF BT port refuses lower; the BLE modem clock itself is
// sourced independently of the CPU PLL), so 80 is the floor AND the target —
// ~30-40% lower active/idle core power vs the prebuilt core's 160 MHz
// default. Override without editing code: build_flags -DXP_CPU_MHZ=160.
#ifndef XP_CPU_MHZ
#define XP_CPU_MHZ 80
#endif

// Race-to-idle scope guard: hold 160 MHz exactly while CPU-bound reader work
// runs (chapter indexing, page compose, cover decode), restore the XP_CPU_MHZ
// park on every exit path — including early returns and failWith() paths.
//
// Boost-per-work beats boost-per-scene: the reader's wall-clock is dominated
// by WAITING (panel refresh ~400 ms, button idle, hours-long reading
// sessions), and a waiting core at 160 MHz burns roughly double for zero
// speedup. Scoping the boost to the work sites keeps the battery ledger
// strictly positive: same joules per job finished sooner, park the rest.
//
// Depth-counted so nested guards (a compose inside a work unit) restore only
// at the outermost exit. Main loop is single-threaded; no locking needed.
//
// Under CONFIG_PM_ENABLE (the x3sleep/x3lean/x3ls packages) the power manager
// owns the clock: boot calls esp_pm_configure(max 160, min 40), the CPU idles
// at 40 and the BLE controller's APB lock lifts it to 80 for radio events.
// A direct setCpuFrequencyMhz() would be undone at the next lock transition,
// so the boost is an ESP_PM_CPU_FREQ_MAX lock instead: held -> max_freq_mhz,
// released -> whatever the manager decides. Same depth counting.
#if CONFIG_PM_ENABLE
#include <esp_pm.h>
class CpuBoost {
 public:
  CpuBoost() {
    if (depth()++ == 0) esp_pm_lock_acquire(lock());
  }
  ~CpuBoost() {
    if (--depth() == 0) esp_pm_lock_release(lock());
  }
  CpuBoost(const CpuBoost&) = delete;
  CpuBoost& operator=(const CpuBoost&) = delete;

 private:
  static esp_pm_lock_handle_t lock() {
    static esp_pm_lock_handle_t h = nullptr;
    if (!h) esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "cpuboost", &h);
    return h;
  }
  static int& depth() {
    static int d = 0;
    return d;
  }
};
#else
class CpuBoost {
 public:
  CpuBoost() {
    if (depth()++ == 0 && getCpuFrequencyMhz() < 160) setCpuFrequencyMhz(160);
  }
  ~CpuBoost() {
    if (--depth() == 0 && getCpuFrequencyMhz() != XP_CPU_MHZ) setCpuFrequencyMhz(XP_CPU_MHZ);
  }
  CpuBoost(const CpuBoost&) = delete;
  CpuBoost& operator=(const CpuBoost&) = delete;

 private:
  static int& depth() {
    static int d = 0;
    return d;
  }
};
#endif  // CONFIG_PM_ENABLE

