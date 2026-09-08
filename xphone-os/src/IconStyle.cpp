#include "IconStyle.h"

#include <Preferences.h>
#include <SDCardManager.h>

#include <cstdlib>
#include <cstring>

#include "art/LauncherIcons.h"

// Icon packs come from two places:
//   1. the packs compiled into the firmware (art/LauncherIcons.h), and
//   2. pack files on the card: /icons/<Name>.xpi — see tools/xphone-icons/
//      gen/export_pack.py for the writer. The phone syncs them like books,
//      so a new pack never needs a firmware release.
//
// The active pack is persisted by NAME, not index: card packs come and go
// and the built-in list grows, so an index would silently pick a different
// pack after a sync or an update.
//
// XPI file: 16-byte header then XPhoneIconAppCount bitmaps, each
// XPhoneLauncherIconSize rows of ceil(size/8) bytes, MSB first, bit 0 = ink —
// the exact bytes drawIcon() reads, so loading is one read into RAM.
//   0  "XPI1"
//   4  u16 icon size (104)       6  u8 icon count (6)     7  u8 reserved
//   8  u32 reserved              12 u32 reserved

namespace {

constexpr const char* kPrefsNs = "xphone";
constexpr const char* kPrefsKey = "iconPack";      // legacy: index (kept for migration)
constexpr const char* kPrefsNameKey = "iconPackN";  // pack name
constexpr const char* kIconDir = "/icons";
constexpr const char* kExt = ".xpi";
constexpr int kMaxCardPacks = 24;
constexpr int kNameMax = 32;
constexpr int kRowBytes = (XPhoneLauncherIconSize + 7) / 8;
constexpr int kIconBytes = kRowBytes * XPhoneLauncherIconSize;

struct CardPack {
  char name[kNameMax];
};

// Both card-pack buffers live on the heap and only while they are needed.
// As statics they cost 8,880 bytes on every device from boot (the pack
// bits 8,112 + the name table 768), which was the whole 9 KB the boot heap
// lost when this feature merged (2026-09-05). Most devices never carry an
// /icons folder; they now pay nothing.
CardPack* gCard = nullptr;  // kMaxCardPacks entries, allocated on the first pack file found
uint8_t gCardCount = 0;
bool gScanned = false;

uint8_t gPack = 0;   // index into builtins (0..builtin-1) then card packs
bool gLoaded = false;

// The one card pack held in RAM: 6 * 1352 = 8112 bytes, allocated when a
// card pack is active and freed when a built-in pack takes over. Cycling
// in Settings reloads on each step (a 8 KB read).
uint8_t* gCardBits = nullptr;
int gCardLoaded = -1;  // card pack index currently in gCardBits, -1 = none

void releaseCardBits() {
  free(gCardBits);
  gCardBits = nullptr;
  gCardLoaded = -1;
}

uint8_t builtinCount() { return static_cast<uint8_t>(XPhoneIconPackCount); }

void scanCard() {
  if (gScanned) return;
  gScanned = true;
  gCardCount = 0;
  if (!SdMan.ready() && !SdMan.begin()) return;
  FsFile d = SdMan.open(kIconDir, O_RDONLY);
  if (!d || !d.isDir()) return;
  FsFile f;
  while (f.openNext(&d, O_RDONLY) && gCardCount < kMaxCardPacks) {
    char name[64];
    const int len = f.getName(name, sizeof(name));
    const bool dir = f.isDir();
    f.close();
    if (len <= 0 || dir || name[0] == '.') continue;
    const size_t ext = strlen(kExt);
    if (static_cast<size_t>(len) <= ext || strcasecmp(name + len - ext, kExt) != 0) continue;
    if (static_cast<size_t>(len - ext) >= kNameMax) continue;  // too long for the settings line
    if (!gCard) {
      gCard = static_cast<CardPack*>(calloc(kMaxCardPacks, sizeof(CardPack)));
      if (!gCard) break;
    }
    memcpy(gCard[gCardCount].name, name, len - ext);
    gCard[gCardCount].name[len - ext] = 0;
    gCardCount++;
  }
  d.close();
  if (gCardCount) Serial.printf("[xphone-os] icons: %u pack file(s) on the card\n", static_cast<unsigned>(gCardCount));
}

bool loadCard(int cardIndex) {
  if (gCardLoaded == cardIndex) return true;
  if (cardIndex < 0 || cardIndex >= gCardCount || !gCard) return false;
  if (!gCardBits) {
    gCardBits = static_cast<uint8_t*>(malloc(static_cast<size_t>(XPhoneIconAppCount) * kIconBytes));
    if (!gCardBits) return false;  // no room: the built-in pack stands in
  }
  char path[80];
  snprintf(path, sizeof(path), "%s/%s%s", kIconDir, gCard[cardIndex].name, kExt);
  if (!SdMan.ready() && !SdMan.begin()) return false;
  FsFile f = SdMan.open(path, O_RDONLY);
  if (!f) return false;
  uint8_t hdr[16];
  bool ok = f.read(hdr, sizeof(hdr)) == static_cast<int>(sizeof(hdr)) && memcmp(hdr, "XPI1", 4) == 0 &&
            (hdr[4] | (hdr[5] << 8)) == XPhoneLauncherIconSize && hdr[6] == XPhoneIconAppCount;
  if (ok) {
    for (int i = 0; i < XPhoneIconAppCount && ok; i++) {
      ok = f.read(gCardBits + static_cast<size_t>(i) * kIconBytes, kIconBytes) == kIconBytes;
    }
  }
  f.close();
  if (!ok) {
    Serial.printf("[xphone-os] icons: %s is not a valid pack file\n", path);
    releaseCardBits();
    return false;
  }
  gCardLoaded = cardIndex;
  return true;
}

uint8_t totalCount() { return static_cast<uint8_t>(builtinCount() + gCardCount); }

const char* nameAt(uint8_t i) {
  if (i < builtinCount()) return XPhoneIconPackNames[i];
  const int c = i - builtinCount();
  return (gCard && c < gCardCount) ? gCard[c].name : nullptr;
}

int indexOfName(const char* name) {
  if (!name || !name[0]) return -1;
  for (uint8_t i = 0; i < totalCount(); i++) {
    const char* n = nameAt(i);
    if (n && strcmp(n, name) == 0) return i;
  }
  return -1;
}

void ensureLoaded() {
  if (gLoaded) return;
  gLoaded = true;
  scanCard();
  Preferences prefs;
  char saved[kNameMax] = {0};
  uint8_t legacy = 0;
  if (prefs.begin(kPrefsNs, /*readOnly=*/true)) {
    prefs.getString(kPrefsNameKey, saved, sizeof(saved));
    legacy = prefs.getUChar(kPrefsKey, 0);
    prefs.end();
  }
  int idx = indexOfName(saved);
  if (idx < 0) idx = legacy < builtinCount() ? legacy : 0;  // pre-name firmware, or the file is gone
  gPack = static_cast<uint8_t>(idx);
  if (gPack >= builtinCount() && !loadCard(gPack - builtinCount())) gPack = 0;
}

}  // namespace

namespace IconStyle {

uint8_t get() {
  ensureLoaded();
  return gPack;
}

void set(uint8_t packIndex) {
  ensureLoaded();
  uint8_t next = packIndex < totalCount() ? packIndex : 0;
  if (next >= builtinCount() && !loadCard(next - builtinCount())) next = 0;
  if (next < builtinCount()) releaseCardBits();  // a built-in pack: give the 8 KB back
  gPack = next;
  Preferences prefs;
  if (prefs.begin(kPrefsNs, /*readOnly=*/false)) {
    prefs.putString(kPrefsNameKey, nameAt(gPack));
    prefs.putUChar(kPrefsKey, gPack < builtinCount() ? gPack : 0);
    prefs.end();
  }
}

const uint8_t* iconForApp(int appIndex) {
  ensureLoaded();
  if (appIndex < 0 || appIndex >= XPhoneIconAppCount) return nullptr;
  if (gPack < builtinCount()) return XPhoneIconPacks[gPack][appIndex];
  if (!loadCard(gPack - builtinCount())) return XPhoneIconPacks[0][appIndex];
  return gCardBits + static_cast<size_t>(appIndex) * kIconBytes;
}

const char* activeName() {
  ensureLoaded();
  const char* n = nameAt(gPack);
  return n ? n : XPhoneIconPackNames[0];
}

const char* packName(uint8_t i) {
  ensureLoaded();
  return nameAt(i);
}

uint8_t packCount() {
  ensureLoaded();
  return totalCount();
}

void rescan() {
  gScanned = false;
  releaseCardBits();
  free(gCard);
  gCard = nullptr;
  gCardCount = 0;
  scanCard();
  if (gPack >= totalCount()) gPack = 0;
  if (gPack >= builtinCount() && !loadCard(gPack - builtinCount())) gPack = 0;
}

}  // namespace IconStyle
