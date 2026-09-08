#include "AppsManager.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstring>
#include <string>

#include "scenes/AppScenes.h"
#include "scenes/HomeScene.h"

namespace apps_mgr {
namespace {

bool gInventoryRequested = false;
bool gInfoRequested = false;

// Multi-part install assembly. One transfer at a time; part 0 resets.
// The link dying mid-assembly just leaves stale state that the next
// part-0 replaces.
std::string gAsmName;
std::string gAsmData;
int gAsmNextPart = 0;
int gAsmParts = 0;
constexpr size_t kMaxAppBytes = 8 * 1024;  // AppScene::kMaxFileBytes

bool validName(const char* n) {
  if (!n || !n[0] || strlen(n) > 31) return false;
  for (const char* p = n; *p; ++p) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

// Accept only ids the home screen knows. Unknown ids drop silently so a
// newer phone never wedges an older firmware.
bool builtinId(const char* s) {
  static constexpr const char* kIds[] = {"read",  "priorities", "today",
                                         "block", "workout",    "notifications"};
  for (const char* id : kIds)
    if (!strcmp(s, id)) return true;
  return false;
}

void csvFromArray(JsonArrayConst arr, char* out, size_t n, size_t maxItems) {
  size_t len = 0, count = 0;
  out[0] = 0;
  for (JsonVariantConst v : arr) {
    const char* s = v.as<const char*>();
    if (!s || !builtinId(s) || count >= maxItems) continue;
    const int wrote = snprintf(out + len, n - len, "%s%s", len ? "," : "", s);
    if (wrote < 0 || len + wrote >= n) break;
    len += wrote;
    ++count;
  }
}

bool handleLayout(JsonObjectConst doc) {
  const char* layout = doc["layout"].as<const char*>();
  if (layout) {
    setHomeLayout(!strcmp(layout, "widget") ? HomeLayout::Widget : HomeLayout::Tiles);
  }
  char csv[96];
  if (!doc["tiles"].isNull()) {
    csvFromArray(doc["tiles"].as<JsonArrayConst>(), csv, sizeof(csv), 2);
    if (csv[0]) setHomeTiles(csv);
  }
  if (!doc["widgets"].isNull()) {
    csvFromArray(doc["widgets"].as<JsonArrayConst>(), csv, sizeof(csv), 4);
    if (csv[0]) setHomeWidgets(csv);
  }
  if (!doc["slots"].isNull()) {
    // Launcher slots: builtin ids OR installed app names, up to six.
    char slots[224] = {0};
    size_t len = 0, count = 0;
    for (JsonVariantConst v : doc["slots"].as<JsonArrayConst>()) {
      const char* s = v.as<const char*>();
      if (!s || count >= 6 || !(builtinId(s) || validName(s))) continue;
      const int wrote = snprintf(slots + len, sizeof(slots) - len, "%s%s", len ? "," : "", s);
      if (wrote < 0 || len + wrote >= sizeof(slots)) break;
      len += wrote;
      ++count;
    }
    if (slots[0]) setHomeSlots(slots);
  }
  Serial.printf("[xphone-os] apps: home.layout applied (%s)\n", layout ? layout : "-");
  applyHomeConfigLive();
  return true;
}

bool handleInstall(JsonObjectConst doc) {
  const char* name = doc["name"].as<const char*>();
  if (!validName(name)) return true;  // consumed, refused
  const int part = doc["part"] | 0;
  const int parts = doc["parts"] | 1;
  const char* data = doc["data"].as<const char*>();
  if (!data || parts < 1 || part < 0 || part >= parts) return true;

  if (part == 0) {
    gAsmName = name;
    gAsmData.clear();
    gAsmNextPart = 0;
    gAsmParts = parts;
  }
  if (gAsmName != name || part != gAsmNextPart || parts != gAsmParts) {
    Serial.printf("[xphone-os] apps: install %s part %d out of order, dropped\n", name, part);
    gAsmName.clear();
    return true;
  }
  if (gAsmData.size() + strlen(data) > kMaxAppBytes) {
    Serial.printf("[xphone-os] apps: install %s exceeds %u bytes, dropped\n", name,
                  static_cast<unsigned>(kMaxAppBytes));
    gAsmName.clear();
    return true;
  }
  gAsmData += data;
  ++gAsmNextPart;
  if (gAsmNextPart < gAsmParts) return true;

  // Last part: validate before anything touches the SD.
  {
    JsonDocument probe;
    if (deserializeJson(probe, gAsmData) || probe["app"].as<int>() != 1) {
      Serial.printf("[xphone-os] apps: install %s: not a valid app file\n", name);
      gAsmName.clear();
      return true;
    }
  }
  if (!SdMan.ready() && !SdMan.begin()) {
    gAsmName.clear();
    return true;
  }
  char dir[48], path[64];
  snprintf(dir, sizeof(dir), "/apps/%s", name);
  snprintf(path, sizeof(path), "/apps/%s/app.json", name);
  if (!SdMan.exists("/apps")) SdMan.mkdir("/apps");
  if (!SdMan.exists(dir) && !SdMan.mkdir(dir)) {
    gAsmName.clear();
    return true;
  }
  FsFile f = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  bool ok = false;
  if (f) {
    ok = f.write(gAsmData.data(), gAsmData.size()) == static_cast<int>(gAsmData.size());
    f.close();
  }
  Serial.printf("[xphone-os] apps: install %s %s (%u bytes)\n", name, ok ? "ok" : "FAILED",
                static_cast<unsigned>(gAsmData.size()));
  gAsmName.clear();
  gAsmData.clear();
  gInventoryRequested = true;  // the inventory is the ack
  return true;
}

// app.data {name, data:{...}} — the app's current data card. Written to
// /apps/<name>/data.json (the file AppScene reads), so the app renders
// from the last data even after a reboot with no phone. Capped at the
// same 8 KB as app.json. If the app is on glass it reloads and repaints.
bool handleData(JsonObjectConst doc) {
  const char* name = doc["name"].as<const char*>();
  if (!validName(name)) return true;
  JsonVariantConst data = doc["data"];
  if (data.isNull()) return true;
  std::string json;
  serializeJson(data, json);
  if (json.size() > kMaxAppBytes) {
    Serial.printf("[xphone-os] apps: data for %s exceeds %u bytes, dropped\n", name,
                  static_cast<unsigned>(kMaxAppBytes));
    return true;
  }
  if (!SdMan.ready() && !SdMan.begin()) return true;
  char path[64];
  snprintf(path, sizeof(path), "/apps/%s", name);
  if (!SdMan.exists(path)) return true;  // data for an app that is not installed
  snprintf(path, sizeof(path), "/apps/%s/data.json", name);
  FsFile f = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return true;
  f.write(json.data(), json.size());
  f.close();
  appDataArrived(name);
  return true;
}

bool handleRemove(JsonObjectConst doc) {
  const char* name = doc["name"].as<const char*>();
  if (!validName(name)) return true;
  if (!SdMan.ready() && !SdMan.begin()) return true;
  char path[64];
  for (const char* file : {"app.json", "data.json"}) {
    snprintf(path, sizeof(path), "/apps/%s/%s", name, file);
    if (SdMan.exists(path)) SdMan.remove(path);
  }
  snprintf(path, sizeof(path), "/apps/%s", name);
  if (SdMan.exists(path)) SdMan.rmdir(path);
  Serial.printf("[xphone-os] apps: removed %s\n", name);
  gInventoryRequested = true;
  return true;
}

}  // namespace

bool handleCard(JsonObjectConst doc, const char* type) {
  if (!type) return false;
  if (!strcmp(type, "home.layout")) return handleLayout(doc);
  if (!strcmp(type, "app.install")) return handleInstall(doc);
  if (!strcmp(type, "app.remove")) return handleRemove(doc);
  if (!strcmp(type, "app.data")) return handleData(doc);
  if (!strcmp(type, "device.apps.request")) {
    gInventoryRequested = true;
    return true;
  }
  if (!strcmp(type, "device.info.request")) {
    gInfoRequested = true;
    return true;
  }
  return false;
}

bool infoRequestedAndClear() {
  const bool v = gInfoRequested;
  gInfoRequested = false;
  return v;
}

bool inventoryRequestedAndClear() {
  const bool v = gInventoryRequested;
  gInventoryRequested = false;
  return v;
}

void forEachInstalledApp(void (*emit)(const char* name, const char* title, void* ctx),
                         void* ctx) {
  if (!SdMan.ready() && !SdMan.begin()) return;
  FsFile dir = SdMan.open("/apps", O_RDONLY);
  if (!dir) return;
  FsFile entry;
  int count = 0;
  while (count < 16 && entry.openNext(&dir, O_RDONLY)) {
    char name[40] = {0};
    if (entry.isDir() && entry.getName(name, sizeof(name)) > 0 && validName(name)) {
      char path[64];
      snprintf(path, sizeof(path), "/apps/%s/app.json", name);
      FsFile f = SdMan.open(path, O_RDONLY);
      if (f) {
        // Filtered parse: only the display name leaves the file.
        JsonDocument filter;
        filter["name"] = true;
        JsonDocument doc;
        char buf[512];
        const int n = f.read(buf, sizeof(buf) - 1);
        f.close();
        const char* title = name;
        if (n > 0) {
          buf[n] = 0;
          // A truncated read still yields "name" when it sits early in the
          // file (our apps put it first). A parse failure falls back to the
          // directory name.
          if (!deserializeJson(doc, buf, static_cast<size_t>(n),
                               DeserializationOption::Filter(filter)) &&
              doc["name"].as<const char*>()) {
            title = doc["name"].as<const char*>();
          }
        }
        emit(name, title, ctx);
        ++count;
      }
    }
    entry.close();
  }
  dir.close();
}

}  // namespace apps_mgr
