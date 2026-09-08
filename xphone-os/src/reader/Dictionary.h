#pragma once
// The reader's dictionary: one file on the card, /books/en.fdic, built by
// tools/dict/build_fdic.py from WordNet 3.1 and copied over by the phone.
//
// Layout (docs in the builder): an 80-byte header, a sorted index of block
// heads (32 bytes each: u64 offset, u32 length, char head[20]), then blocks
// of entries {u16 len, u8 head_len, head, text}. A lookup is a binary
// search over the index (eleven small reads for a thousand blocks) and
// one walk through a block, entry by entry, so no block ever has to sit
// in RAM. Text "=base" means "see base": the lookup follows it once.
#include <cstddef>
#include <cstdint>

namespace reader {

class Dictionary {
 public:
  static constexpr const char* kPath = "/books/en.fdic";
  // Is a dictionary file on the card? Cached per call site; cheap.
  static bool present();
  // Look `word` up. Tries the word as given (lower-cased), then without a
  // possessive, then the common English endings. Writes the entry text into
  // `out` and the headword it matched into `head`. False when nothing fits.
  static bool lookup(const char* word, char* head, size_t headCap, char* out, size_t outCap);

 private:
  static bool find(const char* word, char* head, size_t headCap, char* out, size_t outCap);
};

}  // namespace reader
