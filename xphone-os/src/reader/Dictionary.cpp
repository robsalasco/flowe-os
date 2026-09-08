#include "Dictionary.h"

#include <SDCardManager.h>

#include <cctype>
#include <cstring>

namespace reader {

namespace {

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint16_t version, flags;
  uint32_t entry_count, index_count, index_stride;
  uint64_t index_off, blocks_off, file_len;
  char lang[8];
  char name[24];
  uint32_t reserved;
};
struct IndexEnt {
  uint64_t off;
  uint32_t len;
  char head[20];
};
#pragma pack(pop)

bool readAt(FsFile& f, uint64_t off, void* dst, size_t n) {
  if (!f.seekSet(off)) return false;
  return f.read(dst, n) == (int)n;
}

void lowerAscii(char* s) {
  for (; *s; s++)
    if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

// The index head is the block's first headword cut to 19 bytes.
int cmpHead(const char* word, const char head[20]) {
  char h[20];
  memcpy(h, head, 20);
  h[19] = 0;
  return strncmp(word, h, 19);
}

}  // namespace

bool Dictionary::present() {
  if (!SdMan.ready() && !SdMan.begin()) return false;
  return SdMan.exists(kPath);
}

bool Dictionary::find(const char* word, char* head, size_t headCap, char* out, size_t outCap) {
  if (!SdMan.ready() && !SdMan.begin()) return false;
  FsFile f = SdMan.open(kPath, O_RDONLY);
  if (!f) return false;
  Header h;
  if (!readAt(f, 0, &h, sizeof(h)) || memcmp(h.magic, "FDIC", 4) != 0 || h.version != 1 ||
      h.index_count == 0) {
    f.close();
    return false;
  }
  // Last index entry whose head <= word.
  uint32_t lo = 0, hi = h.index_count - 1;
  IndexEnt e;
  while (lo < hi) {
    const uint32_t mid = (lo + hi + 1) / 2;
    if (!readAt(f, h.index_off + (uint64_t)mid * sizeof(e), &e, sizeof(e))) {
      f.close();
      return false;
    }
    if (cmpHead(word, e.head) >= 0) lo = mid;
    else hi = mid - 1;
  }
  if (!readAt(f, h.index_off + (uint64_t)lo * sizeof(e), &e, sizeof(e))) {
    f.close();
    return false;
  }
  // Walk the block one entry at a time: header, headword, then the text
  // only for the match. Nothing bigger than one entry is ever in RAM.
  uint64_t p = e.off;
  const uint64_t end = e.off + e.len;
  const size_t wl = strlen(word);
  bool found = false;
  while (p + 3 <= end) {
    uint8_t hdr[3];
    if (!readAt(f, p, hdr, 3)) break;
    const uint16_t elen = (uint16_t)(hdr[0] | (hdr[1] << 8));
    const uint8_t hl = hdr[2];
    if (elen < 3 + hl) break;
    if (hl == wl) {
      char hw[64];
      if (hl < sizeof(hw) && readAt(f, p + 3, hw, hl)) {
        hw[hl] = 0;
        if (strcmp(hw, word) == 0) {
          const size_t tl = elen - 3 - hl;
          const size_t take = tl < outCap - 1 ? tl : outCap - 1;
          if (readAt(f, p + 3 + hl, out, take)) {
            out[take] = 0;
            snprintf(head, headCap, "%s", hw);
            found = true;
          }
          break;
        }
      }
    }
    p += elen;
  }
  f.close();
  return found;
}

bool Dictionary::lookup(const char* word, char* head, size_t headCap, char* out, size_t outCap) {
  if (!word || !word[0] || outCap < 2) return false;
  char w[64];
  snprintf(w, sizeof(w), "%s", word);
  lowerAscii(w);
  // Candidates, in order: as is; without 's / '; then the endings a
  // reader meets most (cats, boxes, walked, walking, happier, happiest).
  char cand[8][64];
  int n = 0;
  auto add = [&](const char* s) {
    if (n >= 8 || !s[0]) return;
    for (int i = 0; i < n; i++)
      if (strcmp(cand[i], s) == 0) return;
    snprintf(cand[n++], 64, "%s", s);
  };
  add(w);
  size_t L = strlen(w);
  char t[64];
  if (L > 2 && (strcmp(w + L - 2, "'s") == 0)) { snprintf(t, sizeof(t), "%.*s", (int)(L - 2), w); add(t); }
  if (L > 1 && w[L - 1] == '\'') { snprintf(t, sizeof(t), "%.*s", (int)(L - 1), w); add(t); }
  if (L > 3 && w[L - 1] == 's') { snprintf(t, sizeof(t), "%.*s", (int)(L - 1), w); add(t); }
  if (L > 4 && strcmp(w + L - 2, "es") == 0) { snprintf(t, sizeof(t), "%.*s", (int)(L - 2), w); add(t); }
  if (L > 4 && strcmp(w + L - 3, "ies") == 0) { snprintf(t, sizeof(t), "%.*sy", (int)(L - 3), w); add(t); }
  if (L > 4 && strcmp(w + L - 2, "ed") == 0) {
    snprintf(t, sizeof(t), "%.*s", (int)(L - 2), w); add(t);
    snprintf(t, sizeof(t), "%.*s", (int)(L - 1), w); add(t);  // baked -> bake
  }
  if (L > 5 && strcmp(w + L - 3, "ing") == 0) {
    snprintf(t, sizeof(t), "%.*s", (int)(L - 3), w); add(t);
    snprintf(t, sizeof(t), "%.*se", (int)(L - 3), w); add(t);  // making -> make
  }
  if (L > 4 && strcmp(w + L - 2, "er") == 0) { snprintf(t, sizeof(t), "%.*s", (int)(L - 2), w); add(t); }
  if (L > 5 && strcmp(w + L - 3, "est") == 0) { snprintf(t, sizeof(t), "%.*s", (int)(L - 3), w); add(t); }
  for (int i = 0; i < n; i++) {
    if (!find(cand[i], head, headCap, out, outCap)) continue;
    if (out[0] == '=') {  // an inflected form: see its base, once
      char base[64];
      snprintf(base, sizeof(base), "%s", out + 1);
      if (find(base, head, headCap, out, outCap) && out[0] != '=') return true;
      continue;
    }
    return true;
  }
  return false;
}

}  // namespace reader
