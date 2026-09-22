// Host test for reader::FbpBook::ensureShelfSidecars' pkgDeclaresCover
// out-param: whether the package's shelf section declares a cover (ts > 0),
// independent of whether the sidecar file already exists. Follows
// min_page_buffer_test.cpp's pattern of synthesizing an FBPK package in memory
// so no bookc-built fixture is required, and relies on the FLOWE_TEST_FBP_IO
// SD stub (which writes real files) to observe the sidecar.
#include "../../src/reader/FbpBook.h"
#include "../../src/Gfx.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

// ensureShelfSidecars allocates nothing, so FbpBook.cpp is linked with its
// normal malloc/free. uzlib's crc helpers and Gfx::drawPixel are only needed
// to satisfy the linker for the parts of FbpBook.cpp this test does not
// exercise (renderPage is not called).
extern "C" uint32_t uzlib_crc32(const void*, unsigned, uint32_t) { std::abort(); }
extern "C" uint32_t uzlib_adler32(const void*, unsigned, uint32_t) { std::abort(); }
void Gfx::drawPixel(int, int, bool) {}

namespace {

void put_u16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = (uint8_t)(v & 0xFF);
  b[off + 1] = (uint8_t)(v >> 8);
}
void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  b[off] = (uint8_t)(v & 0xFF);
  b[off + 1] = (uint8_t)((v >> 8) & 0xFF);
  b[off + 2] = (uint8_t)((v >> 16) & 0xFF);
  b[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}
void put_u64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; i++) b[off + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

// Minimal valid FbpBook::Header (80 bytes): fmt_ver 6 / min_reader 6, one
// profile, meta_off pointing at a zero region (empty title/author), and
// shelf_off as given (0 = no shelf section).
std::vector<uint8_t> baseFixture(uint64_t shelfOff) {
  std::vector<uint8_t> b(512, 0);
  memcpy(b.data(), "FBPK", 4);
  put_u16(b, 4, 6);    // fmt_ver
  put_u16(b, 6, 6);    // min_reader
  put_u32(b, 24, 1);   // profile_count
  put_u64(b, 64, 248); // meta_off -> zero region (empty title/author)
  put_u64(b, 72, shelfOff);
  return b;
}

// Shelf section header is 16 bytes: tw(2) th(2) ts(4) sw(2) sh(2) ss(4),
// followed by ts bytes of 1-bpp cover bits then ss bytes of strip bits.
void putShelf(std::vector<uint8_t>& b, size_t off, uint16_t tw, uint16_t th, uint32_t ts,
              uint16_t sw, uint16_t sh, uint32_t ss) {
  put_u16(b, off, tw);
  put_u16(b, off + 2, th);
  put_u32(b, off + 4, ts);
  put_u16(b, off + 8, sw);
  put_u16(b, off + 10, sh);
  put_u32(b, off + 12, ss);
}

void writeFile(const char* path, const std::vector<uint8_t>& bytes) {
  FILE* f = std::fopen(path, "wb");
  assert(f);
  assert(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
  std::fclose(f);
}

// A package whose shelf section declares a cover: pkgDeclaresCover must be
// true and the .cov sidecar must be written (the host SD stub writes real
// files), carrying the XT header + ts payload bytes.
void testDeclaresCover() {
  const char* path = "/tmp/fbp_shelf_cover.fbp";
  std::vector<uint8_t> b = baseFixture(256);
  const uint32_t ts = 32;
  putShelf(b, 256, 100, 150, ts, 0, 0, 0);
  for (size_t i = 0; i < ts; i++) b[256 + 16 + i] = (uint8_t)(i * 7 + 1);  // cover payload
  writeFile(path, b);

  bool hasCover = false, hasStrip = false, declares = false;
  assert(reader::FbpBook::ensureShelfSidecars(path, &hasCover, &hasStrip, &declares));
  assert(declares);
  assert(hasCover);
  assert(!hasStrip);

  // Sidecar exists with the XT header {0x54,0x58,1,0, w, h} then the payload.
  FILE* f = std::fopen("/tmp/fbp_shelf_cover.fbp.cov", "rb");
  assert(f);
  uint8_t hdr[8] = {0};
  assert(std::fread(hdr, 1, 8, f) == 8);
  assert(hdr[0] == 0x54 && hdr[1] == 0x58 && hdr[2] == 1 && hdr[3] == 0);
  assert(hdr[4] == 100 && hdr[5] == 0 && hdr[6] == 150 && hdr[7] == 0);
  std::fclose(f);

  std::remove(path);
  std::remove("/tmp/fbp_shelf_cover.fbp.cov");
}

// A package with no shelf section: shelf_off == 0 means "nothing declared",
// so pkgDeclaresCover is false and no sidecar is written (no exceptions, no
// files). The out-param must be cleared even when the caller seeded it true.
void testNoShelf() {
  const char* path = "/tmp/fbp_shelf_none.fbp";
  std::vector<uint8_t> b = baseFixture(0);
  writeFile(path, b);

  bool hasCover = false, hasStrip = false, declares = true;
  assert(!reader::FbpBook::ensureShelfSidecars(path, &hasCover, &hasStrip, &declares));
  assert(!declares);
  assert(!hasCover);
  assert(!hasStrip);
  assert(std::fopen("/tmp/fbp_shelf_none.fbp.cov", "rb") == nullptr);
  assert(std::fopen("/tmp/fbp_shelf_none.fbp.str", "rb") == nullptr);

  std::remove(path);
}

// A strip-only package (ts == 0, ss > 0 — the shaped-title case for languages
// the UI font cannot draw): declares stays false because no cover is
// declared, while the .str sidecar is still extracted.
void testStripOnly() {
  const char* path = "/tmp/fbp_shelf_strip.fbp";
  std::vector<uint8_t> b = baseFixture(256);
  const uint32_t ss = 24;
  putShelf(b, 256, 0, 0, 0, 192, 56, ss);
  for (size_t i = 0; i < ss; i++) b[256 + 16 + i] = (uint8_t)(i * 3 + 5);  // strip payload
  writeFile(path, b);

  bool hasCover = false, hasStrip = false, declares = true;
  reader::FbpBook::ensureShelfSidecars(path, &hasCover, &hasStrip, &declares);
  assert(!declares);
  assert(!hasCover);
  assert(hasStrip);
  assert(std::fopen("/tmp/fbp_shelf_strip.fbp.cov", "rb") == nullptr);
  assert(std::fopen("/tmp/fbp_shelf_strip.fbp.str", "rb") != nullptr);

  std::remove(path);
  std::remove("/tmp/fbp_shelf_strip.fbp.str");
}

}  // namespace

int main() {
  testDeclaresCover();
  testNoShelf();
  testStripOnly();
  std::puts("PASS: ensureShelfSidecars pkgDeclaresCover (declares cover, no shelf, strip only)");
  return 0;
}
