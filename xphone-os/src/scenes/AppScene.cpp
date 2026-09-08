#include "AppScene.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstring>
#include <memory>

#include "../Fonts.h"
#include "../Gfx.h"
#include "AppScenes.h"

namespace {

// Op-name → handler dispatch happens in drawOp; these are shared helpers.

const XpFont& fontByName(const char* name) {
  if (name && !strcmp(name, "bold")) return kFontBold;
  if (name && !strcmp(name, "small")) return kFontSmall;
  return kFontRegular;
}

// A bare "{{path}}" template (nothing but one slot, optional spaces).
// Used by emitCard to keep raw JSON values in payloads.
const char* barePathSlot(const char* s, char* pathOut, size_t n) {
  if (!s) return nullptr;
  while (*s == ' ') ++s;
  if (s[0] != '{' || s[1] != '{') return nullptr;
  s += 2;
  while (*s == ' ') ++s;
  size_t i = 0;
  while (*s && *s != ' ' && *s != '}' && i < n - 1) pathOut[i++] = *s++;
  pathOut[i] = 0;
  while (*s == ' ') ++s;
  if (s[0] != '}' || s[1] != '}') return nullptr;
  s += 2;
  while (*s == ' ') ++s;
  return (*s == 0 && i > 0) ? pathOut : nullptr;
}

void appendVariant(JsonVariantConst v, char* out, size_t n, size_t& len) {
  char buf[48];
  const char* s = buf;
  if (v.isNull()) {
    s = "";
  } else if (v.is<bool>()) {
    s = v.as<bool>() ? "x" : " ";  // checkbox convention, same as applab.py
  } else if (v.is<const char*>()) {
    s = v.as<const char*>();
  } else if (v.is<long>()) {
    snprintf(buf, sizeof(buf), "%ld", v.as<long>());
  } else if (v.is<double>()) {
    snprintf(buf, sizeof(buf), "%g", v.as<double>());
  } else {
    s = "";
  }
  while (*s && len < n - 1) out[len++] = *s++;
}

}  // namespace

// ---------------------------------------------------------------- load

bool AppScene::open(const char* name) {
  onExit();  // clear any previous app
  if (!name || !name[0] || strlen(name) >= sizeof(_name)) return false;
  if (!SdMan.ready() && !SdMan.begin()) {
    Serial.println("[xphone-os] app: no SD card");
    return false;
  }
  strncpy(_name, name, sizeof(_name) - 1);

  auto readJson = [&](const char* file, JsonDocument& doc, bool required) {
    char path[80];
    snprintf(path, sizeof(path), "/apps/%s/%s", _name, file);
    FsFile f = SdMan.open(path, O_RDONLY);
    if (!f) {
      if (required) Serial.printf("[xphone-os] app: missing %s\n", path);
      return !required;
    }
    const size_t size = f.size();
    if (size == 0 || size > kMaxFileBytes) {
      Serial.printf("[xphone-os] app: %s is %u bytes (cap %d)\n", path,
                    static_cast<unsigned>(size), kMaxFileBytes);
      f.close();
      return false;
    }
    std::unique_ptr<char[]> buf(new (std::nothrow) char[size]);
    if (!buf || f.read(buf.get(), size) != static_cast<int>(size)) {
      f.close();
      return false;
    }
    f.close();
    const DeserializationError err = deserializeJson(doc, buf.get(), size);
    if (err) {
      Serial.printf("[xphone-os] app: %s parse: %s\n", path, err.c_str());
      return false;
    }
    return true;
  };

  if (!readJson("app.json", _app, /*required=*/true)) return false;
  if (_app["app"].as<int>() != 1) {
    Serial.printf("[xphone-os] app: %s: not a Flowe app (need \"app\": 1)\n", _name);
    _app.clear();
    return false;
  }
  readJson("data.json", _data, /*required=*/false);  // Phase 1 data stand-in

  // Soft-key labels into stable storage (softKeys() returns raw pointers).
  JsonArrayConst keys = _app["keys"].as<JsonArrayConst>();
  for (int i = 0; i < 4; ++i) {
    _keys[i] = nullptr;
    _keyLabels[i][0] = 0;
    const char* label = keys[i]["label"].as<const char*>();
    if (!label && i == 0) label = "BACK";
    if (!label) continue;
    if (!strcmp(label, "<")) {
      _keys[i] = SoftKey::Left;
    } else if (!strcmp(label, ">")) {
      _keys[i] = SoftKey::Right;
    } else {
      strncpy(_keyLabels[i], label, sizeof(_keyLabels[i]) - 1);
      _keys[i] = _keyLabels[i];
    }
  }

  _sel = 0;
  _page = 0;
  _loaded = true;
  markDirty();
  Serial.printf("[xphone-os] app: loaded %s\n", _name);
  return true;
}

void AppScene::onExit() {
  // Working state is freed here — an inactive app costs no heap
  // (MEMORY_ARCHITECTURE.md: scene working state must not outlive the scene).
  _app.clear();
  _data.clear();
  _loaded = false;
  _name[0] = 0;
}

const char* const* AppScene::softKeys() const {
  if (!_loaded || _app["keys"].isNull()) return Scene::softKeys();
  return _keys;
}

// ---------------------------------------------------------------- bindings

JsonVariantConst AppScene::lookupPath(const char* path, JsonObjectConst row) const {
  char head[48];
  size_t h = 0;
  const char* rest = path;
  while (*rest && *rest != '.' && h < sizeof(head) - 1) head[h++] = *rest++;
  head[h] = 0;

  auto descend = [&](JsonVariantConst v) {
    const char* p = rest;
    while (!v.isNull() && *p == '.') {
      ++p;
      char part[48];
      size_t k = 0;
      while (*p && *p != '.' && k < sizeof(part) - 1) part[k++] = *p++;
      part[k] = 0;
      v = v[part];
    }
    return v;
  };

  // Scope 1: the current list row.
  if (!row.isNull()) {
    JsonVariantConst v = row[head];
    if (!v.isNull()) return descend(v);
  }
  // Scope 2: built-ins.
  if (!strcmp(head, "sel")) return JsonVariantConst();  // numeric built-ins
  if (!strcmp(head, "selected")) {
    JsonArrayConst items = cursorItems();
    if (_sel >= 0 && _sel < static_cast<int>(items.size()))
      return descend(items[_sel]);
    return JsonVariantConst();
  }
  const char* pagesBind = _app["pages"].as<const char*>();
  if (pagesBind && (!strcmp(head, "current") || !strcmp(head, "count"))) {
    JsonArrayConst arr = _data[pagesBind].as<JsonArrayConst>();
    if (!strcmp(head, "count")) return JsonVariantConst();  // handled in bindString
    if (arr.size() > 0) return descend(arr[_page % arr.size()]);
    return JsonVariantConst();
  }
  // Scope 3: the data document root.
  return descend(_data[head]);
}

void AppScene::bindString(const char* tmpl, JsonObjectConst row, char* out, size_t n) const {
  size_t len = 0;
  const char* s = tmpl ? tmpl : "";
  while (*s && len < n - 1) {
    if (s[0] == '{' && s[1] == '{') {
      s += 2;
      while (*s == ' ') ++s;
      char path[64];
      size_t k = 0;
      while (*s && *s != '}' && *s != ' ' && k < sizeof(path) - 1) path[k++] = *s++;
      path[k] = 0;
      while (*s == ' ') ++s;
      if (s[0] == '}' && s[1] == '}') s += 2;
      // Numeric built-ins render directly; everything else via lookup.
      if (!strcmp(path, "i")) {
        len += snprintf(out + len, n - len, "%d", _rowIndex);
      } else if (!strcmp(path, "sel")) {
        len += snprintf(out + len, n - len, "%d", _sel);
      } else if (!strcmp(path, "page")) {
        len += snprintf(out + len, n - len, "%d", _page);
      } else if (!strcmp(path, "count")) {
        const char* pagesBind = _app["pages"].as<const char*>();
        JsonArrayConst arr = _data[pagesBind ? pagesBind : ""].as<JsonArrayConst>();
        len += snprintf(out + len, n - len, "%u", static_cast<unsigned>(arr.size()));
      } else if (!strcmp(path, "current.n")) {
        const char* pagesBind = _app["pages"].as<const char*>();
        JsonArrayConst arr = _data[pagesBind ? pagesBind : ""].as<JsonArrayConst>();
        len += snprintf(out + len, n - len, "%u",
                        arr.size() ? static_cast<unsigned>(_page % arr.size()) + 1 : 0);
      } else {
        appendVariant(lookupPath(path, row), out, n, len);
      }
      if (len > n - 1) len = n - 1;
      continue;
    }
    out[len++] = *s++;
  }
  out[len] = 0;
}

bool AppScene::truthy(const char* cond, JsonObjectConst row) const {
  if (!cond || !cond[0]) return true;
  const bool neg = cond[0] == '!';
  JsonVariantConst v = lookupPath(neg ? cond + 1 : cond, row);
  bool t;
  if (v.isNull()) {
    t = false;
  } else if (v.is<bool>()) {
    t = v.as<bool>();
  } else if (v.is<const char*>()) {
    t = v.as<const char*>()[0] != 0;
  } else if (v.is<JsonArrayConst>()) {
    t = v.as<JsonArrayConst>().size() > 0;
  } else {
    t = true;
  }
  return neg ? !t : t;
}

// ---------------------------------------------------------------- render

int AppScene::coordVal(JsonVariantConst v, const Gfx& gfx, int dy) const {
  if (v.is<int>()) return v.as<int>() + dy;
  const char* s = v.as<const char*>();
  if (!s) return dy;
  // Grammar (same as applab.py / render_spec): [wh] [+|- N] [/D]
  while (*s == ' ') ++s;
  int base = 0;
  bool isH = false;
  if (*s == 'w') {
    base = gfx.width();
  } else if (*s == 'h') {
    base = gfx.height() - Scene::SOFTKEY_BAR_H;  // "h" = content height
    isH = true;
  } else {
    return dy;
  }
  ++s;
  while (*s == ' ') ++s;
  int sign = 0;
  if (*s == '+' || *s == '-') {
    sign = (*s == '+') ? 1 : -1;
    ++s;
    while (*s == ' ') ++s;
  }
  long delta = 0;
  while (*s >= '0' && *s <= '9') delta = delta * 10 + (*s++ - '0');
  if (*s == '/') {
    ++s;
    long div = 0;
    while (*s >= '0' && *s <= '9') div = div * 10 + (*s++ - '0');
    if (div > 0) base /= static_cast<int>(div);
  }
  if (sign) base += sign * static_cast<int>(delta);
  return base + (isH ? dy : dy);
}

void AppScene::drawOp(Gfx& gfx, JsonObjectConst op, JsonObjectConst row, int dy, bool invert) {
  const char* when = op["when"].as<const char*>();
  if (when && !truthy(when, row)) return;

  const char* kind = op["op"].as<const char*>();
  if (!kind) return;
  const bool black = op["black"].isNull() ? !invert : (op["black"].as<bool>() != invert);

  if (!strcmp(kind, "list")) {
    drawList(gfx, op);
    return;
  }

  const int y = op["y"].isNull() ? dy : coordVal(op["y"], gfx) + dy;

  if (!strcmp(kind, "text")) {
    const XpFont& f = fontByName(op["font"].as<const char*>());
    char s[192];
    bindString(op["s"].as<const char*>(), row, s, sizeof(s));
    if (!s[0]) return;
    int x = coordVal(op["x"], gfx);
    const char* align = op["align"].as<const char*>();
    const int scale = op["scale"].isNull() ? 1 : op["scale"].as<int>();
    const int w = scale > 1 ? gfx.textWidthScaled(f, s, scale) : gfx.textWidth(f, s);
    if (align && !strcmp(align, "center")) x -= w / 2;
    if (align && !strcmp(align, "right")) x -= w;
    if (scale > 1) {
      gfx.drawTextScaled(f, x, y, s, scale, black);
    } else {
      gfx.drawText(f, x, y, s, black);
    }
  } else if (!strcmp(kind, "wrap")) {
    const XpFont& f = fontByName(op["font"].as<const char*>());
    char s[256];
    bindString(op["s"].as<const char*>(), row, s, sizeof(s));
    gfx.drawTextWrapped(f, coordVal(op["x"], gfx), y, s, coordVal(op["maxWidth"], gfx),
                        op["maxLines"].isNull() ? 2 : op["maxLines"].as<int>(), black);
  } else if (!strcmp(kind, "fillRect")) {
    gfx.fillRect(coordVal(op["x"], gfx), y, coordVal(op["w"], gfx), coordVal(op["h"], gfx), black);
  } else if (!strcmp(kind, "drawRect")) {
    gfx.drawRect(coordVal(op["x"], gfx), y, coordVal(op["w"], gfx), coordVal(op["h"], gfx),
                 op["t"].isNull() ? 1 : op["t"].as<int>(), black);
  } else if (!strcmp(kind, "drawRoundedRect")) {
    gfx.drawRoundedRect(coordVal(op["x"], gfx), y, coordVal(op["w"], gfx), coordVal(op["h"], gfx),
                        op["r"].isNull() ? 8 : op["r"].as<int>(),
                        op["t"].isNull() ? 1 : op["t"].as<int>(), black);
  } else if (!strcmp(kind, "fillRoundedRect")) {
    gfx.fillRoundedRect(coordVal(op["x"], gfx), y, coordVal(op["w"], gfx), coordVal(op["h"], gfx),
                        op["r"].isNull() ? 8 : op["r"].as<int>(), black);
  } else if (!strcmp(kind, "line")) {
    gfx.drawLine(coordVal(op["x0"], gfx), coordVal(op["y0"], gfx) + dy, coordVal(op["x1"], gfx),
                 coordVal(op["y1"], gfx) + dy, op["t"].isNull() ? 1 : op["t"].as<int>(), black);
  }
  // Unknown ops draw nothing: an app from a newer format version degrades
  // to its known ops instead of failing the whole screen.
}

void AppScene::drawList(Gfx& gfx, JsonObjectConst op) {
  JsonArrayConst items = _data[op["bind"].as<const char*>()].as<JsonArrayConst>();
  const int y0 = coordVal(op["y"], gfx);
  const int rowH = op["rowH"].as<int>();
  if (rowH <= 0) return;
  const int contentH = gfx.height() - Scene::SOFTKEY_BAR_H;
  int maxRows = (contentH - y0) / rowH;
  if (!op["max"].isNull() && op["max"].as<int>() < maxRows) maxRows = op["max"].as<int>();
  const bool cursor = op["cursor"].as<bool>();
  int first = 0;
  if (cursor && _sel >= maxRows) first = _sel - maxRows + 1;  // keep cursor visible

  JsonArrayConst children = op["ops"].as<JsonArrayConst>();
  int drawn = 0;
  for (int idx = first; idx < static_cast<int>(items.size()) && drawn < maxRows; ++idx, ++drawn) {
    const int ry = y0 + drawn * rowH;
    const bool selected = cursor && idx == _sel;
    if (selected) gfx.fillRect(0, ry, gfx.width(), rowH, true);
    _rowIndex = idx + 1;  // "{{i}}" inside row ops (1-based)
    JsonObjectConst row = items[idx].as<JsonObjectConst>();
    for (JsonObjectConst child : children) drawOp(gfx, child, row, ry, selected);
  }
  _rowIndex = 0;
}

void AppScene::render(Gfx& gfx) {
  if (!_loaded) {
    gfx.drawTextCentered(kFontRegular, gfx.width() / 2, gfx.height() / 2 - 40, "App failed to load");
    gfx.drawTextCentered(kFontSmall, gfx.width() / 2, gfx.height() / 2, "Check the serial log.");
    return;
  }
  JsonArrayConst ops = _app["screen"]["ops"].as<JsonArrayConst>();
  for (JsonObjectConst op : ops) drawOp(gfx, op, JsonObjectConst(), 0, false);
}

// ---------------------------------------------------------------- input

JsonObjectConst AppScene::keyDef(int slot) const {
  return _app["keys"][slot].as<JsonObjectConst>();
}

JsonArrayConst AppScene::cursorItems(JsonObjectConst* listOpOut) const {
  for (JsonObjectConst op : _app["screen"]["ops"].as<JsonArrayConst>()) {
    if (op["op"] == "list" && op["cursor"].as<bool>()) {
      if (listOpOut) *listOpOut = op;
      return _data[op["bind"].as<const char*>()].as<JsonArrayConst>();
    }
  }
  return JsonArrayConst();
}

JsonArrayConst AppScene::pagesArray() const {
  return _data[_app["pages"].as<const char*>()].as<JsonArrayConst>();
}

void AppScene::handleInput(Input& in) {
  struct { Btn btn; int slot; } map[] = {
      {Btn::Back, 0}, {Btn::Confirm, 1}, {Btn::Left, 2}, {Btn::Right, 3}};
  for (const auto& m : map) {
    if (!in.wasPressed(m.btn)) continue;
    runAction(keyDef(m.slot)["action"].as<JsonObjectConst>(), m.slot == 0 ? "exit" : nullptr);
    return;
  }
}

void AppScene::runAction(JsonObjectConst action, const char* fallbackDo) {
  const char* do_ = action["do"].as<const char*>();
  if (!do_) do_ = fallbackDo;
  if (!do_) return;

  if (!strcmp(do_, "exit")) {
    showLauncher();
    return;
  }
  if (!strcmp(do_, "cursor")) {
    JsonArrayConst items = cursorItems();
    if (items.size() > 0) {
      const int delta = action["delta"].isNull() ? 1 : action["delta"].as<int>();
      _sel = (_sel + delta) % static_cast<int>(items.size());
      if (_sel < 0) _sel += items.size();
      markDirty();
    }
    return;
  }
  if (!strcmp(do_, "page")) {
    const char* bind = action["bind"].as<const char*>();
    JsonArrayConst pages =
        bind ? _data[bind].as<JsonArrayConst>() : pagesArray();
    const int n = pages.size() > 0 ? static_cast<int>(pages.size()) : 1;
    const int delta = action["delta"].isNull() ? 1 : action["delta"].as<int>();
    int p = _page + delta;
    if (p < 0) p = 0;
    if (p > n - 1) p = n - 1;
    if (p != _page) {
      _page = p;
      markDirty();
    }
    return;
  }
  if (!strcmp(do_, "toggle")) {
    JsonArrayConst items = cursorItems();
    if (_sel >= 0 && _sel < static_cast<int>(items.size())) {
      const char* field = action["field"].as<const char*>();
      if (!field) field = "done";
      // Mutate through the writable document (cursorItems is a const view).
      for (JsonObjectConst op : _app["screen"]["ops"].as<JsonArrayConst>()) {
        if (op["op"] == "list" && op["cursor"].as<bool>()) {
          JsonArray arr = _data[op["bind"].as<const char*>()].as<JsonArray>();
          JsonObject r = arr[_sel].as<JsonObject>();
          r[field] = !r[field].as<bool>();
          break;
        }
      }
      emitCard(action);
      markDirty();
    }
    return;
  }
  if (!strcmp(do_, "card")) {
    emitCard(action);
    return;
  }
}

void AppScene::emitCard(JsonObjectConst action) {
  // Phase 1: the card is built exactly as Phase 3 will send it, but goes to
  // the serial log — the bench proof is the payload, not the radio.
  JsonDocument card;
  card["schemaVersion"] = 1;
  const char* kind = action["kind"].as<const char*>();
  card["kind"] = kind ? kind : "plugin.action";
  // The display name from app.json, like the lab; directory name as backstop.
  const char* disp = _app["name"].as<const char*>();
  card["app"] = disp && disp[0] ? disp : _name;
  for (JsonPairConst kv : action["payload"].as<JsonObjectConst>()) {
    const char* tmpl = kv.value().as<const char*>();
    char path[64];
    if (tmpl && barePathSlot(tmpl, path, sizeof(path))) {
      card[kv.key()] = lookupPath(path, JsonObjectConst());  // raw JSON value
    } else if (tmpl) {
      char bound[128];
      bindString(tmpl, JsonObjectConst(), bound, sizeof(bound));
      card[kv.key()] = bound;
    } else {
      card[kv.key()] = kv.value();
    }
  }
  char json[512];
  serializeJson(card, json, sizeof(json));
  Serial.printf("[xphone-os] app: card -> phone: %s\n", json);
}
