#pragma once

// xphone-os M1/M2 — scene registry / navigation helpers.
//
// The scene instances live as statics in AppScenes.cpp; these helpers are
// how scenes navigate without including each other's headers.

class Gfx;

#include <cstddef>
#include <cstdint>

// Shown in Settings and About. XPHONE_VERSION is the human-facing series;
// XPHONE_GIT_REV is the exact build (git describe), so a bug report names
// one commit instead of a marketing number nobody remembered to bump. Both
// come from include/generated/xphone_version.h, rewritten before every
// build by tools/gen_version.py.
#include "generated/xphone_version.h"
constexpr const char* XPHONE_VERSION = XPHONE_MARKETING;
constexpr const char* XPHONE_GIT_REV_STR = XPHONE_GIT_REV;

// M4.2 last-scene restore: a stable id for each restorable scene. Persisted in
// RTC memory at sleep (Sleep.cpp) and dispatched by boot() on wake so the
// device returns to whatever was on glass. Values are explicit so the
// RTC-stored integer is stable across firmware builds.
enum class SceneId : uint32_t {
  Launcher = 0,
  Notifications = 1,
  Settings = 2,
  Block = 3,
  Priorities = 4,
  Today = 5,
  About = 6,
  Reader = 7,
  Workout = 8,
  FileTransfer = 9,
  Wifi = 10,
  Home = 11,  // Widget home layout (Phase 2)
};

// Single source of truth for "what scene is on glass" — set by every show*()
// helper below. Read by Sleep::sleepNow() to persist the restore target.
extern SceneId gCurrentSceneId;

// boot() restore dispatch: switch to the scene named by `id` (calls the
// matching show*() so the scene's onEnter re-requests its data). Unknown ids
// fall back to the launcher.
void showSceneById(SceneId id);

// Short human-readable name for a scene id ("Launcher"/"Block"/…), for the
// About wake diagnostic.
const char* sceneName(SceneId id);

void showLauncher();
void showAbout();
void readerShelfDump();
void readerLineCids();
void readerAcceptGoto(const char* key, uint32_t cid);
void showNotifications();
void showSettings();    // M3: real Settings scene (SD update / restart / about)
void showWifi();        // the device's Wi-Fi screen (roadmap E1, 2026-09-04)
void showBlock();       // M3: real Block scene (Screen Time shields via BLE)
void showBlockDeepWork();  // Launcher top-right long-press: open Block + start Deep Work
void showPriorities();  // M3: real Priorities scene (to-do snapshot via BLE)
void showToday();       // M3: real Today scene (agenda/reminders/weather card)
void showReader();      // R1 EPUB reader (resumes the last book; book list on BACK)
void showWorkout();     // Workout: set-by-set exercise tracker synced from iPhone
// Declarative app (Phase 1: bench dev console "app <name>"). Loads
// /apps/<name>/app.json from SD into the generic AppScene and switches to
// it. Returns false (and stays on the current scene) when loading fails.
bool showApp(const char* name);
// Phase 3: an app.data card landed for `name`; reload if it is on glass.
void appDataArrived(const char* name);
void showHome();  // Widget home layout; falls back to launcher if layout=Tiles
void markHomeDirtyIfActive();
void homeDebugDump();  // bench: print the hero snapshot
// Sleep face for the Widget layout: the frozen hero. False = use the poster.
bool homeRenderDormant(Gfx& gfx);
void showFileTransfer();           // R2: Wi-Fi File Transfer scene (Idle menu)
void showFileTransferAutoStart();  // R2: same, but bring Wi-Fi up immediately (BLE transfer.start)
void showFileTransferAutoStartDirect();
// Sync in place: the phone's transfer.start / transfer.direct with the
// previous screen's picture kept on glass and a pill in the header.
void showFileTransferAutoStartInPlace(bool direct);
// showSceneById, but the reader comes back without the "Opening book..."
// frame: its page is still on glass from before the sync.
void showSceneByIdQuiet(SceneId id);
// Restart as a wake and land on `id` (main.cpp). For memory recovery only.
void quietRestartToScene(uint32_t id);
// A transfer card while the scene is already up: behave like a fresh entry.
void fileTransferRestartFromCard(bool direct);  // W2: raise the device's own hotspot (BLE transfer.direct)
// R2: BLE "transfer.stop" — ack + restart when the transfer scene is active
// (restart is the clean Wi-Fi teardown); no-op on any other scene.
void stopFileTransferIfActive();

// Bench dev console ("where"): current launcher grid index (0-based,
// row-major, COLS=2 — Today/Notif, Prio/Block, Read/Workout).
int launcherSelection();

// Bench dev console ("where" v2): Reader sub-state one-liner.
void readerWhere(char* out, size_t n);

// M2: main.cpp marshals BLE/ANCS events to redraws with these — a scene is
// only marked dirty when it is the one on glass (e-ink discipline: a
// notification burst never repaints the launcher, a connection change never
// repaints Notifications' list rows for nothing).
void markLauncherDirtyIfActive();
void markNotificationsDirtyIfActive();
// M3: companion card revision changed (Block status card updates land here).
void markBlockDirtyIfActive();
// M3: same revision pump for the Priorities snapshot card.
void markPrioritiesDirtyIfActive();
// M3: same revision pump for the Today snapshot card.
void markTodayDirtyIfActive();
// Workout: revision pump for the workout snapshot card + local +/- bumps.
void markWorkoutDirtyIfActive();

// Total boot-to-first-paint time, set once by main.cpp (shown in About).
extern unsigned long gBootTotalMs;

// M4.2 wake diagnostics — captured ONCE in boot() before anything else can
// change them, rendered on the About scene so we can tell (without a serial
// cable) whether the X3 truly deep-sleeps or cold-boots on power-button wake,
// and whether the last-scene id was captured. gWakeResetReason is a short
// esp_reset_reason() string ("POWERON"/"DEEPSLEEP"/"BROWNOUT"/"SW"/"PANIC"/…);
// gWakeRestoreScene names the restored scene, or "none" on a cold boot.
extern const char* gWakeResetReason;
extern const char* gWakeRestoreScene;
