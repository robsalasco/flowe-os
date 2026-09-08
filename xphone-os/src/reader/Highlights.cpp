// See Highlights.h. File I/O mirrors Bookmarks.cpp: whole-file read and
// write through SdMan, sidecar path built from the book path.
#include "Highlights.h"

#include <SDCardManager.h>

#include <cstdio>
#include <cstring>

namespace reader {
namespace {

void sidecarPath(const char* bookPath, char* out, size_t n) {
  snprintf(out, n, "%s.hl", bookPath);
}

bool writeAll(const char* path, const Highlights::Rec* recs, int count) {
  FsFile f = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  const bool ok =
      f.write(recs, (size_t)count * sizeof(Highlights::Rec)) == (int)(count * sizeof(Highlights::Rec));
  f.close();
  return ok;
}

}  // namespace

int Highlights::load(const char* bookPath, Rec* out, int cap) {
  char side[192];
  sidecarPath(bookPath, side, sizeof(side));
  FsFile f = SdMan.open(side, O_RDONLY);
  if (!f) return 0;
  const int n = f.read(out, (size_t)cap * sizeof(Rec));
  f.close();
  return n > 0 ? n / (int)sizeof(Rec) : 0;
}

bool Highlights::has(const char* bookPath, uint32_t cid) {
  Rec recs[kMax];
  const int n = load(bookPath, recs, kMax);
  for (int i = 0; i < n; i++)
    if (recs[i].cid == cid && !(recs[i].flags & kFlagTombstone)) return true;
  return false;
}

int Highlights::toggle(const char* bookPath, uint32_t cid, uint32_t day) {
  char side[192];
  sidecarPath(bookPath, side, sizeof(side));
  Rec recs[kMax];
  int n = load(bookPath, recs, kMax);

  for (int i = 0; i < n; i++) {
    if (recs[i].cid == cid && !(recs[i].flags & kFlagTombstone)) {
      recs[i].flags |= kFlagTombstone;
      return writeAll(side, recs, n) ? -1 : 0;
    }
  }

  if (n >= kMax) {
    // Compact tombstones out; if every slot is live, the oldest live
    // record makes room (the cap rule, stated in the app).
    int w = 0;
    for (int i = 0; i < n; i++)
      if (!(recs[i].flags & kFlagTombstone)) recs[w++] = recs[i];
    if (w >= kMax) {
      memmove(recs, recs + 1, (size_t)(w - 1) * sizeof(Rec));
      w--;
    }
    n = w;
  }
  recs[n].cid = cid;
  recs[n].day = day;
  recs[n].flags = 0;
  memset(recs[n].pad, 0, sizeof(recs[n].pad));
  n++;
  return writeAll(side, recs, n) ? +1 : 0;
}

}  // namespace reader
