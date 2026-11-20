// Luau runtime: sandbox, CPU budget, and the core script API (vectors, colors, require, entities,
// physics signals, timers) on a tiny in-memory package. Port of engine-web/test/luau.test.ts.
//
// Already covered elsewhere (not repeated here): the memory cap (script_test.cpp "the memory cap
// stops a script..."), errors with file + line + repeat count (script_test.cpp "errors carry the
// script path and line..."), and seed determinism (script_test.cpp "the same seed gives the same
// random numbers...").
#include "kits/kit_test.h"

using namespace dreamsy;
using namespace dreamsy::test;

namespace {

const char* kManifest =
    "format = \"0\"\nname = \"T\"\ndimension = \"2d\"\nentry_scene = \"scenes/main.scene.json\"\n[style]\npreset = \"clean\"\n";

// A floor, a falling box (tagged player, runs the script), an area under it and a label.
const char* kScene = R"({"format": "0", "root": "world", "entities": {
  "world": {"type": "Node2D", "children": ["floor", "box", "zone", "label"]},
  "floor": {"type": "Body2D", "props": {"position": [0, 100], "size": [400, 20], "body": "static"}},
  "box": {"type": "Body2D", "props": {"position": [0, 0], "size": [20, 20], "body": "dynamic"}, "tags": ["player"],
          "scripts": ["scripts/client/main.luau"]},
  "zone": {"type": "Area2D", "props": {"position": [0, 60], "size": [40, 40]}},
  "label": {"type": "UIText", "props": {"text": "hi"}}
}})";

// TS run(script, ticks, extra, opts): load the package, step `ticks` times.
Game run(const std::string& script, int ticks, const FileMap& extra = {}, script::HostOptions o = {}) {
  FileMap files{{"dreamsy.toml", kManifest}, {"scenes/main.scene.json", kScene}, {"scripts/client/main.luau", script}};
  for (const auto& [path, data] : extra) files[path] = data;
  Game g = start(files, std::move(o));
  g.step(ticks);
  return g;
}

script::HostOptions budget_opts(double ms) {
  script::HostOptions o;
  o.budget_ms = ms;
  return o;
}

std::string text(const Game& g) { return g->world().str(g.entity("label"), "text"); }

}  // namespace

TEST_SUITE("api luau") {

TEST_CASE("luau: sandbox: no io/debug/loadstring/getfenv, os.clock only") {
  Game g = run(R"(entity("label").text = table.concat({tostring(io), tostring(debug), tostring(loadstring), tostring(getfenv), type(os.clock), tostring(os.time)}, ","))",
               1, {}, budget_opts(budget(100)));
  CHECK(text(g) == "nil,nil,nil,nil,function,nil");
  CHECK_NO_ERRORS(g);
}

TEST_CASE("luau: kills runaway scripts and pcall can't swallow the budget error") {
  // Keep this top-level timeout test fast with an exact 100 ms load budget.
  auto opts = budget_opts(100);
  opts.load_budget_ms = 100;
  Game g = run(R"(
      local n = 0
      onTick(function() n += 1 entity("label").text = tostring(n) end)
      pcall(function() while true do end end)
      entity("label").text = "survived")",
               3, {}, opts);
  REQUIRE_FALSE(g->errors().empty());
  CHECK(g->errors()[0].message == "script exceeded its execution budget (100 ms)");
  CHECK(text(g) == "3");  // the script died, its earlier onTick still runs
}

TEST_CASE("luau: vectors, colors, require, entities, physics, signals, timers") {
  Game g = run(R"(
      local cfg = require("../shared/cfg")
      local box = script.entity
      local v = vec2(3, 4)
      assert(v:length() == 5 and (v * 2).x == 6 and (v + vec2(1, 1)) == vec2(4, 5))
      assert(tostring(color("#ff0000")) == "#ff0000")
      assert(box.id == "box" and box.type == "Body2D" and box.parent == entity("world"))
      assert(#entitiesTagged("player") == 1 and entitiesTagged("player")[1] == box)
      local entered = 0
      entity("zone").bodyEntered:connect(function(other) if other == box then entered += 1 end end)
      local fired = 0
      after(0.5, function() fired += 1 end)
      local t = every(0.25, function() fired += 10 end)
      local s = spawn("Sprite2D", { position = vec2(5, 5), modulate = "#00ff00" }, box)
      assert(s.parent == box and s.modulate.g == 1)
      onTick(function()
        entity("label").text = string.format("%d|%d|%d|%s", cfg.answer, entered, fired, tostring(box.onFloor))
      end)
      after(0.6, function() t:cancel() end))",
               90, {{"scripts/shared/cfg.luau", "return { answer = 42 }"}}, budget_opts(budget(100)));
  CHECK_NO_ERRORS(g);
  CHECK(text(g) == "42|1|21|true");
  CHECK(g.pos("box").y > 70);
}

}  // TEST_SUITE
