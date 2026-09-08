#pragma once

// xphone-os — AppScene: one generic scene that runs a declarative "app".
//
// An app is one JSON file (/apps/<name>/app.json on SD, "app": 1). It
// declares a screen (drawing ops with {{path}} data bindings), a data
// card kind, and what the four buttons do. No user code runs on the
// device — this scene interprets the JSON. The format and its reference
// interpreter live in tools/plugin-lab/ (applab.py); this class must
// match applab.py's semantics op for op, because designs are approved
// against the lab's pixel-exact renders.
//
// Phase 1 scope (docs/plans/2026-08-28-home-apps-execution-plan.md):
//   - apps sideloaded on SD, opened from the bench dev console ("app
//     <name>"), data read from an optional data.json next to app.json
//     (a stand-in for the future BLE data card);
//   - key actions that would message the phone (card / toggle) log the
//     card JSON to serial instead of touching the BLE service — the
//     card wiring is Phase 3 and CompanionBleService changes need
//     coordination.
//
// Memory: both JSON files are capped at kMaxFileBytes and parsed into
// JsonDocuments owned by this scene; onExit() frees them. While active
// the budget is well under 10 KB for lab-sized apps; a file over the cap
// never loads. The scene object itself stays small and static like every
// other scene.

#include <ArduinoJson.h>

#include "../Scene.h"

class AppScene : public Scene {
 public:
  // Load /apps/<name>/app.json (+ optional data.json). Returns false and
  // leaves the scene unloaded on any failure (missing SD, missing file,
  // oversized file, bad JSON, wrong "app" version).
  bool open(const char* name);

  void onExit() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

  bool loaded() const { return _loaded; }
  const char* appName() const { return _name; }

  static constexpr int kMaxFileBytes = 8 * 1024;  // CrossPoint's manifest cap

 private:
  // ---- bindings ----
  // Resolve a dotted path against the scopes (innermost first): the
  // current list row (during list render), built-ins (sel/page/selected/
  // current/count), then the data document root.
  JsonVariantConst lookupPath(const char* path, JsonObjectConst row) const;
  // Fill {{path}} slots in `tmpl` into `out` (truncates at n-1).
  void bindString(const char* tmpl, JsonObjectConst row, char* out, size_t n) const;
  bool truthy(const char* cond, JsonObjectConst row) const;

  // ---- render ----
  int coordVal(JsonVariantConst v, const Gfx& gfx, int dy = 0) const;
  void drawOp(Gfx& gfx, JsonObjectConst op, JsonObjectConst row, int dy, bool invert);
  void drawList(Gfx& gfx, JsonObjectConst op);
  void drawSoftKeyBarLabelsOnly() const {}  // SceneManager draws the bar

  // ---- input ----
  JsonObjectConst keyDef(int slot) const;   // keys[slot], or null
  void runAction(JsonObjectConst action, const char* fallbackDo);
  JsonArrayConst cursorItems(JsonObjectConst* listOp = nullptr) const;
  JsonArrayConst pagesArray() const;
  void emitCard(JsonObjectConst action);    // Phase 1: serial log only

  JsonDocument _app;    // parsed app.json (owned; freed on exit)
  JsonDocument _data;   // parsed data.json (owned; mutable — toggle writes it)
  char _name[32] = {0};
  char _keyLabels[4][12] = {{0}};           // stable storage for softKeys()
  const char* _keys[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  bool _loaded = false;
  int _sel = 0;       // cursor index (first list op with "cursor": true)
  int _page = 0;      // page index (app-level "pages" binding)
  int _rowIndex = 0;  // 1-based row number while drawList runs ("{{i}}")
};
