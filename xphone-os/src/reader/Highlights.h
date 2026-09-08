#pragma once

// R1 paragraph highlights: up to 64 per book in a "<book path>.hl"
// sidecar (the ".pos"/".bmk" convention — a deleted book takes its
// highlights with it). A record is {paragraph content id, day, flags}.
// Deleting sets a tombstone flag instead of compacting, so the phone's
// sync can treat the file as append-only history; the file only compacts
// when a 65th highlight needs the room, and then the oldest live record
// is the one that goes (said out loud in the app).

#include <cstdint>

namespace reader {

class Highlights {
 public:
  static constexpr int kMax = 64;
  static constexpr uint8_t kFlagTombstone = 1;

  struct Rec {
    uint32_t cid;   // paragraph content id
    uint32_t day;   // yyyymmdd, 0 when the clock was unknown
    uint8_t flags;
    uint8_t pad[3];
  };

  // Whole-file load (64 records is 768 bytes). Returns record count
  // including tombstones; the caller filters.
  static int load(const char* bookPath, Rec* out, int cap);

  // Toggle: no live record for cid -> append one; a live record exists ->
  // tombstone it. Returns +1 (added), -1 (removed), 0 on failure.
  // Appending into a full file compacts tombstones first and, if still
  // full, drops the oldest live record.
  static int toggle(const char* bookPath, uint32_t cid, uint32_t day);

  // Is this paragraph highlighted (live record exists)?
  static bool has(const char* bookPath, uint32_t cid);
};

}  // namespace reader
