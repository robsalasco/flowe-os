#include "BleFileReceiver.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstdio>
#include <cstring>

namespace {
constexpr size_t kFrameMax = 520;          // MTU 517 - 3 = 514 payload, rounded
constexpr size_t kQueueFrames = 6;         // ~3 KB: a few intervals of slack
constexpr uint32_t kProgressEvery = 128UL * 1024UL;

struct Frame {
  uint16_t len;
  uint8_t data[kFrameMax];
};

Frame gQueue[kQueueFrames];
volatile uint8_t gHead = 0, gCount = 0;
portMUX_TYPE gLock = portMUX_INITIALIZER_UNLOCKED;

bool gActive = false;
char gName[96] = {0};
char gPartPath[128] = {0};
char gFinalPath[128] = {0};
uint32_t gExpected = 0, gCrcExpected = 0, gReceived = 0, gCrc = 0xFFFFFFFFu, gNextMark = 0;
bool gGap = false;
FsFile gFile;

uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t n) {
  // Table-less CRC-32 (IEEE); ~50 KB/s of data is nothing for the C3.
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

bool safeName(const char* name) {
  if (!name || !name[0] || strlen(name) > 90) return false;
  if (strchr(name, '/') || strstr(name, "..") || name[0] == '.') return false;
  return true;
}
}  // namespace

namespace BleFileReceiver {

bool begin(const char* name, const uint32_t size, const uint32_t crc32) {
  abort();
  if (!safeName(name) || size == 0) return false;
  snprintf(gName, sizeof(gName), "%s", name);
  snprintf(gPartPath, sizeof(gPartPath), "/books/%s.part", name);
  snprintf(gFinalPath, sizeof(gFinalPath), "/books/%s", name);
  SdMan.remove(gPartPath);
  gFile = SdMan.open(gPartPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!gFile) {
    Serial.printf("[xphone-os] ble-file: cannot create %s\n", gPartPath);
    return false;
  }
  gExpected = size;
  gCrcExpected = crc32;
  gReceived = 0;
  gCrc = 0xFFFFFFFFu;
  gNextMark = kProgressEvery;
  gGap = false;
  gActive = true;
  Serial.printf("[xphone-os] ble-file: begin %s (%lu bytes)\n", name, static_cast<unsigned long>(size));
  return true;
}

bool enqueue(const uint8_t* frame, const size_t len) {
  if (!gActive || len < 4 || len > kFrameMax) return false;
  portENTER_CRITICAL(&gLock);
  const bool room = gCount < kQueueFrames;
  if (room) {
    Frame& f = gQueue[(gHead + gCount) % kQueueFrames];
    f.len = static_cast<uint16_t>(len);
    memcpy(f.data, frame, len);
    gCount++;
  }
  portEXIT_CRITICAL(&gLock);
  // A full queue is not a gap by itself: the writer waits and retries once
  // (Meditations, 2026-09-05 01:45: every byte and the CRC arrived, the
  // verdict still said "gap"). The writer calls markGap() when the retry
  // fails too; a real hole also fails the CRC.
  return room;
}

void markGap() { gGap = true; }

size_t drain() {
  if (!gActive) return 0;
  size_t wrote = 0;
  while (true) {
    Frame f;
    portENTER_CRITICAL(&gLock);
    const bool ready = gCount > 0;
    if (ready) {
      f = gQueue[gHead];
      gHead = (gHead + 1) % kQueueFrames;
      gCount--;
    }
    portEXIT_CRITICAL(&gLock);
    if (!ready) break;
    const uint32_t offset = static_cast<uint32_t>(f.data[0]) | (static_cast<uint32_t>(f.data[1]) << 8) |
                            (static_cast<uint32_t>(f.data[2]) << 16) | (static_cast<uint32_t>(f.data[3]) << 24);
    const uint8_t* bytes = f.data + 4;
    const size_t n = f.len - 4;
    if (offset != gReceived) {
      // Out of order or a repeat: seek and let the CRC catch anything wrong.
      gGap = gGap || offset > gReceived;
      gFile.seekSet(offset);
    }
    if (gFile.write(bytes, n) != n) {
      Serial.println("[xphone-os] ble-file: write failed");
      gGap = true;
    }
    if (offset == gReceived) gCrc = crcUpdate(gCrc, bytes, n);
    gReceived = offset + n;
    wrote += n;
  }
  return wrote;
}

bool end(char* reason, const size_t reasonSize) {
  if (!gActive) {
    snprintf(reason, reasonSize, "nothing in progress");
    return false;
  }
  drain();
  gFile.flush();
  gFile.close();
  const uint32_t crc = gCrc ^ 0xFFFFFFFFu;
  bool ok = !gGap && gReceived == gExpected && crc == gCrcExpected;
  if (!ok) {
    snprintf(reason, reasonSize, "%s (got %lu of %lu)", gGap ? "gap" : gReceived != gExpected ? "size" : "checksum",
             static_cast<unsigned long>(gReceived), static_cast<unsigned long>(gExpected));
    SdMan.remove(gPartPath);
  } else {
    SdMan.remove(gFinalPath);
    ok = SdMan.rename(gPartPath, gFinalPath);
    if (!ok) snprintf(reason, reasonSize, "rename failed");
    else snprintf(reason, reasonSize, "ok");
  }
  Serial.printf("[xphone-os] ble-file: end %s -> %s\n", gName, reason);
  gActive = false;
  return ok;
}

void abort() {
  if (gFile) gFile.close();
  if (gActive) SdMan.remove(gPartPath);
  gActive = false;
  portENTER_CRITICAL(&gLock);
  gHead = 0;
  gCount = 0;
  portEXIT_CRITICAL(&gLock);
}

bool active() { return gActive; }
uint32_t received() { return gReceived; }
uint32_t expected() { return gExpected; }
const char* name() { return gName; }

bool takeProgressMark() {
  if (!gActive || gReceived < gNextMark) return false;
  gNextMark += kProgressEvery;
  return true;
}

}  // namespace BleFileReceiver
