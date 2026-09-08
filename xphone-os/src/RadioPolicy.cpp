// See RadioPolicy.h. Storage follows IconStyle.cpp: one NVS byte in the
// shared "xphone" namespace, lazy-loaded, written at the moment of change.
#include "RadioPolicy.h"

#include <Preferences.h>

namespace RadioPolicy {
namespace {
constexpr const char* kNs = "xphone";
constexpr const char* kKey = "rdRadio";
constexpr int kAutoOffAtPercent = 20;

ReaderRadio gValue = ReaderRadio::Auto;
bool gLoaded = false;

ReaderRadio clamp(uint8_t raw) {
  return raw <= 2 ? static_cast<ReaderRadio>(raw) : ReaderRadio::Auto;
}

void ensureLoaded() {
  if (gLoaded) return;
  gLoaded = true;
  Preferences p;
  if (p.begin(kNs, /*readOnly=*/true)) {
    gValue = clamp(p.getUChar(kKey, 0));
    p.end();
  }
}
}  // namespace

ReaderRadio get() {
  ensureLoaded();
  return gValue;
}

void set(ReaderRadio v) {
  ensureLoaded();
  gValue = v;
  Preferences p;
  if (p.begin(kNs, /*readOnly=*/false)) {
    p.putUChar(kKey, static_cast<uint8_t>(v));
    p.end();
  }
}

ReaderRadio cycled() {
  const uint8_t next = (static_cast<uint8_t>(get()) + 1) % 3;
  return static_cast<ReaderRadio>(next);
}

bool radioAllowed(int batteryPercent) {
  switch (get()) {
    case ReaderRadio::Always: return true;
    case ReaderRadio::Never: return false;
    case ReaderRadio::Auto:
    default:
      // Unknown percentage fails open — see the header.
      return batteryPercent < 0 || batteryPercent > kAutoOffAtPercent;
  }
}

const char* label(ReaderRadio v) {
  switch (v) {
    case ReaderRadio::Always: return "Always on";
    case ReaderRadio::Never: return "Off in books";
    case ReaderRadio::Auto:
    default: return "Auto";
  }
}

}  // namespace RadioPolicy
