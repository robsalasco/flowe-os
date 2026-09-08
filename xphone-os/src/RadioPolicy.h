// R4 "Radio while reading" — the user's say over the reading link.
// Tri-state, stored in NVS, consulted ONCE per book open (never mid-read,
// so the radio can never drop under a reader):
//   Auto   — radio on, unless the battery is at or under 20%
//   Always — radio on
//   Never  — the reader takes the EPUB suspend path for every book
// Global (IconStyle pattern): the reader menu sets it, ReaderScene's book
// open consults it, the apps mirror it read-only.
#pragma once

#include <cstdint>

namespace RadioPolicy {

enum class ReaderRadio : uint8_t { Auto = 0, Always = 1, Never = 2 };

ReaderRadio get();
void set(ReaderRadio v);
ReaderRadio cycled();  // the value after one SELECT press (Auto->Always->Never->Auto)

// The decision, made at book open. batteryPercent < 0 means unknown —
// unknown fails OPEN (radio stays up): a broken gauge must not silently
// kill notifications.
bool radioAllowed(int batteryPercent);

// Value-column label for the menu row.
const char* label(ReaderRadio v);

}  // namespace RadioPolicy
