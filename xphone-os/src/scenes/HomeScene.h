#pragma once

// xphone-os — HomeScene: the Widget home layout (Phase 2 of the
// home/apps plan, docs/plans/2026-08-28-home-apps-execution-plan.md).
//
// Home is a layout CHOICE (decided 2026-08-29). Layout 1, "Tiles", is
// the existing LauncherScene, unchanged. Layout 2, "Widget" — this
// scene — shows one hero, carousel dots, and two pinned icon tiles.
// The chosen layout lives in NVS ("xphone"/"homeLay", 0=tiles,
// 1=widget; Widget is the default since 2026-09-04). The phone sets it
// with a home.layout card, Settings has a row, and the bench flips it
// with the dev-console command `home tiles|widget`.
//
// Hero pages (Phase 2): Read (the chosen V2 stat-blocks hero — real
// cover left, big numbers right) and Priorities. A running Block takes
// the hero over and hides the dots until it ends (the one override).
// Left/right cycle pages, OPEN enters the hero's app, APPS opens the
// launcher grid.
//
// Known deviations from the approved mocks, to resolve in Phase 3:
//   - the cover draws at the existing 200x260 thumb size (the phone
//     does not pre-render hero-size sidecars yet);
//   - the "2h 10m LEFT" stat is "24m TODAY" (time-left needs a pace
//     model the device does not have; today-minutes is real data);
//   - the Block countdown shows minutes, not MM:SS (no RTC seconds).

#include <cstdint>

#include "../Scene.h"

class HomeScene : public Scene {
 public:
  void onEnter() override;
  void onExit() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

  // Bench ("homedump"): reload the read-hero snapshot and print it, so the
  // host verification mirror renders from the device's own values.
  void debugDump();

 private:
  static constexpr int kMaxPages = 4;

  void renderStatusStrip(Gfx& gfx) const;
  void renderReadHero(Gfx& gfx) const;
  void renderPrioritiesHero(Gfx& gfx) const;
  void renderTodayHero(Gfx& gfx) const;
  void renderBlockHero(Gfx& gfx) const;
  void renderDots(Gfx& gfx) const;
  void renderTiles(Gfx& gfx) const;
  bool blockOverride() const;
  void loadReadHero();  // Preferences + book.bin metadata + .pos + stats

  // Read-hero snapshot, refreshed in onEnter/loadReadHero. Fixed buffers
  // only; the transient Epub used to fill them is dropped immediately.
  struct {
    bool valid = false;
    char title[64] = {0};
    char author[48] = {0};
    char thumbPath[96] = {0};  // "" = no cover; streamed at render time
    uint8_t pct = 0;           // 0 = unknown
    uint16_t streakDays = 0;
    uint16_t todayMinutes = 0;
  } _read;

  int _page = 0;
  uint32_t _seenPrioRev = 0;
  uint32_t _seenBlockRev = 0;

  // Parsed home config (NVS csv strings; see loadConfig()).
  enum class PageId : uint8_t { Read, Priorities, Today };
  PageId _pages[kMaxPages] = {PageId::Read, PageId::Priorities};
  int _pageCount = 2;
  uint8_t _tileIcon[2] = {3, 0};    // launcher icon indices (Block, Today)
  char _tileId[2][16] = {"block", "today"};

 public:
  void loadConfig();  // re-reads the NVS csv strings (onEnter + live apply)
  // Sleep face: draw the current hero into a cleared framebuffer, no
  // chrome. Returns false when this page has no dormant form of its own
  // (Priorities: the caller uses the priorities poster; Today likewise).
  bool renderDormant(Gfx& gfx);
};

// The stored layout choice (NVS). main.cpp boot + devcon read this.
// 0.7 ships the tile grid (Andrew, 2026-09-06 14:27). While this is false the
// device boots to the grid whatever NVS or a phone card says, and Settings
// hides the Home layout and Sleep screen rows. The Customization thread
// flips it when the Widget home is ready.
constexpr bool kExploreHomeApps = false;
enum class HomeLayout : uint8_t { Tiles = 0, Widget = 1 };
HomeLayout homeLayout();
void setHomeLayout(HomeLayout l);
// Phase 3 (home.layout card): pinned tiles and widget order, csv of
// builtin ids ("block,today" / "read,priorities"). Stored in NVS.
void setHomeTiles(const char* csv);
void setHomeWidgets(const char* csv);
// The six launcher slots, csv of builtin ids or installed app names.
// Default: the six builtins in launcher order.
void setHomeSlots(const char* csv);
/// The documented out-of-the-box slot order.
const char* kDefaultSlotsCsv();
void homeSlotsCsv(char* out, size_t n);
// Re-read the config into the live scene and repaint or re-root as needed.
void applyHomeConfigLive();
