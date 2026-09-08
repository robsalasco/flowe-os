#pragma once

// xphone-os — AppsManager: the firmware side of the Phase 3 cards
// (docs/plans/2026-08-28-home-apps-execution-plan.md).
//
// The phone arranges the device. Three inbound cards do all of it:
//
//   home.layout        {layout:"tiles"|"widget", tiles:[..], widgets:[..]}
//   app.install        {name, part, parts, data} — app.json text, split
//                      into parts the size of one GATT write
//   app.remove         {name}
//   device.apps.request                — please send the inventory
//
// One outbound message answers: device.apps, the installed inventory,
// chunked like notif.apps. It goes out after every install and remove,
// and on request. The inventory IS the install ack.
//
// All functions run on the main loop only (the card path already
// marshals there). App names are strict: lowercase letters, digits,
// '-' and '_', at most 31 bytes. Anything else never touches the SD.

#include <ArduinoJson.h>

namespace apps_mgr {

// Route one parsed card. Returns true when the card was one of ours.
// `type` is the card's "type" (or "kind") string.
bool handleCard(JsonObjectConst doc, const char* type);

// Read-and-clear: the BLE loop drains this into sendAppsInventory(),
// the same pattern wifi.known.request uses.
bool inventoryRequestedAndClear();
// Read-and-clear: a device.info.request arrived.
bool infoRequestedAndClear();

// Append the installed inventory entries ("{\"name\":..,\"title\":..}")
// one at a time. The BLE sender owns chunking; this owns the scan.
// Calls `emit` for each app dir under /apps with a valid app.json.
void forEachInstalledApp(void (*emit)(const char* name, const char* title, void* ctx),
                         void* ctx);

}  // namespace apps_mgr
