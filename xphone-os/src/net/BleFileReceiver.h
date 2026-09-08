#pragma once
// xphone-os — the Bluetooth slow lane (roadmap D1, 2026-09-04).
//
// When no Wi-Fi rung is possible, the phone sends ONE book over the BLE
// link: a JSON "book.begin" card (name, size, crc32) on the card
// characteristic, then raw frames on the file characteristic
// ([4-byte little-endian offset][bytes]), then "book.end". Frames arrive in
// the NimBLE host task and are queued; the main loop writes them to
// /books/<name>.part (SD stays a main-loop affair). At "book.end" the size
// and CRC are checked and the file is renamed into place. About 30-60 KB/s
// on a phone at its fast connection interval: a 4 MB book in 1-2 minutes.
#include <cstddef>
#include <cstdint>

namespace BleFileReceiver {
// Start receiving <name> (a bare file name under /books). Returns false when
// the name is bad or the .part cannot be created.
bool begin(const char* name, uint32_t size, uint32_t crc32);
// Called from the BLE host task: copy the frame into the queue. Returns false
// when the queue is full (the phone's write is acked anyway; it will notice
// the gap at "book.end" when the size does not match, and resend).
/** The writer dropped a frame for good (queue full twice): the book is bad. */
void markGap();
bool enqueue(const uint8_t* frame, size_t len);
// Called from the main loop: write queued frames to the card. Returns bytes
// written this call.
size_t drain();
// "book.end": verify and rename. Returns true when the book landed.
bool end(char* reason, size_t reasonSize);
void abort();
bool active();
uint32_t received();
uint32_t expected();
const char* name();
// Progress mark: true once per 128 KB crossed (the caller notifies the phone).
bool takeProgressMark();
}  // namespace BleFileReceiver
