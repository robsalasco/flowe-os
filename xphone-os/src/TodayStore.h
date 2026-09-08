#pragma once

// xphone-os M3 — dedicated Today store.
//
// PrioritiesStore's mold applied to the "today.snapshot" card: fixed char
// fields, no std::string, no heap growth, ~1.9KB static RAM total. The
// companion service copies every today snapshot into this store at the parse
// point (CompanionBleService.cpp applyCardPayload, predicate identical to
// x4-os TodayActivity.cpp:13-15 isTodayCard), because the service holds only
// ONE CompanionCardState — any later card (a Block status push, a priorities
// snapshot) would otherwise clobber the today data while the Today scene is
// off glass.
//
// applyCardPayload runs ONLY on the Arduino main loop (handleCardWrite on the
// NimBLE host task just stashes raw bytes; processPending() parses them from
// loop()), and scenes render on the main loop too, so every accessor here is
// main-loop-only by construction: no mutex needed.

#include <cstddef>
#include <cstdint>

struct CompanionCardState;

class TodayStore {
 public:
  // Capacity mirrors CompanionProtocol::MAX_TODAY_ITEMS (static_assert in
  // the .cpp). The fields are a deliberate DIET of the wire limits: the old
  // Item spent 65 bytes storing the word "reminder" (one bit) and 49 on
  // "9:00 AM". 16 dieted items cost ~2.8 KB where 6 fat ones cost 1.6 KB —
  // that is what makes the scrollable Today affordable on the X3's heap
  // headroom (flowe-os#39). updateFromCard clips into these sizes.
  static constexpr std::size_t CAPACITY = 16;

  struct Item {
    bool reminder = false;    // card kind == "reminder" -> the Reminders section
    char time[25] = {0};      // e.g. "9:00 AM" ("" for undated reminders)
    char title[97] = {0};
    char subtitle[49] = {0};  // events: day-bucket label ("TODAY"/"TOMORROW")
  };  // ~172 bytes

  // Copy the card's today items + weather/high-low/sync lines into the fixed
  // buffers and bump the revision. Call ONLY when the card actually is a
  // today snapshot — the service's predicate decides. Multi-part snapshots
  // (card.parts > 1, slices sharing one card id) stage across calls and
  // commit on the final part, the same discipline as PrioritiesStore.
  void updateFromCard(const CompanionCardState& card);

  bool hasSnapshot() const { return _hasSnapshot; }
  std::size_t count() const { return _count; }
  // Bounds-checked copy-out; returns false when index >= count().
  bool get(std::size_t index, Item& out) const;
  // Agenda (non-reminder) / reminder tallies for the section headers.
  void tally(int& agenda, int& reminders) const;
  const char* weather() const { return _weather; }    // "" when the card had none
  const char* highLow() const { return _highLow; }
  const char* syncLine() const { return _syncLine; }  // e.g. "Synced 9:41 AM"

  // Bumped on every updateFromCard(); the main loop polls this to mark the
  // Today scene dirty (e-ink discipline: no redraws from BLE paths).
  uint32_t revision() const { return _revision; }

 private:
  Item _items[CAPACITY];
  std::size_t _count = 0;
  // Multi-part staging cursor: slices fill _items in order and _count only
  // moves on the final part, so renders between parts see the OLD snapshot.
  std::size_t _stageCount = 0;
  char _stageId[24] = {0};
  bool _hasSnapshot = false;
  char _weather[97] = {0};
  char _highLow[97] = {0};
  char _syncLine[97] = {0};
  uint32_t _revision = 0;
};

extern TodayStore TODAY_STORE;
