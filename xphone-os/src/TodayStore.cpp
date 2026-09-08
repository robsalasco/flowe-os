#include "TodayStore.h"

#include <cstdio>
#include <cstring>

#include "ble/CompanionProtocol.h"

TodayStore TODAY_STORE;

// Capacity stays in lockstep with the wire protocol. The FIELDS are a
// deliberate diet of the wire limits (see the header comment): time and
// title clip via snprintf, kind collapses to one bool.
static_assert(TodayStore::CAPACITY == CompanionProtocol::MAX_TODAY_ITEMS, "store capacity != protocol");
static_assert(sizeof(TodayStore::Item::title) == CompanionProtocol::MAX_TITLE_CHARS + 1, "title field != protocol");
static_assert(sizeof(TodayStore::Item::subtitle) == CompanionProtocol::MAX_TODAY_FIELD_CHARS + 1,
              "subtitle field != protocol");

void TodayStore::updateFromCard(const CompanionCardState& card) {
  // Multi-part staging (PrioritiesStore's discipline): a new snapshot id or
  // an explicit first part restarts the cursor; a single-part card is the
  // degenerate one-slice case.
  if (card.part == 0 || strcmp(_stageId, card.id.c_str()) != 0) {
    _stageCount = 0;
    snprintf(_stageId, sizeof(_stageId), "%s", card.id.c_str());
  }
  for (std::size_t i = 0; i < card.todayItemCount && _stageCount < CAPACITY; i++, _stageCount++) {
    const CompanionTodayItem& src = card.todayItems[i];
    Item& dst = _items[_stageCount];
    dst.reminder = strcmp(src.kind.c_str(), "reminder") == 0;
    // snprintf clips + always null-terminates (strncpy does not).
    snprintf(dst.time, sizeof(dst.time), "%s", src.time.c_str());
    snprintf(dst.title, sizeof(dst.title), "%s", src.title.c_str());
    snprintf(dst.subtitle, sizeof(dst.subtitle), "%s", src.subtitle.c_str());
  }
  // Commit only on the final part, so a render between parts (~one
  // connection interval) sees the OLD snapshot, never a truncated new one.
  if (card.part + 1 < card.parts) return;
  _count = _stageCount;
  snprintf(_weather, sizeof(_weather), "%s", card.todayWeather.c_str());
  snprintf(_highLow, sizeof(_highLow), "%s", card.todayHighLow.c_str());
  snprintf(_syncLine, sizeof(_syncLine), "%s", card.todaySync.c_str());
  _hasSnapshot = true;
  _revision++;
}

bool TodayStore::get(std::size_t index, Item& out) const {
  if (index >= _count) return false;
  out = _items[index];
  return true;
}

void TodayStore::tally(int& agenda, int& reminders) const {
  agenda = 0;
  reminders = 0;
  for (std::size_t i = 0; i < _count; i++) {
    if (_items[i].reminder) {
      reminders++;
    } else {
      agenda++;
    }
  }
}
