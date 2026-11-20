// Data reader, clock, timeScale, game.session, ui.theme, UI input_transparent / scale, TileMap2D taps,
// budgets, spawned tile maps. Port of engine-web/test/platform-api.test.ts (same numbers and thresholds).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <regex>

#include "kits/kit_test.h"
#include "ui/layout.h"

using namespace dreamsy;
using namespace dreamsy::test;

namespace {

using Strs = std::vector<std::string>;

// TS pkg(entities, files, manifest): every entity is a direct child of the `world` root.
// `top` = their ids (JSON strings), `entities` = their JSON members.
FileMap make_pkg(const std::string& top, const std::string& entities, FileMap files = {}, const std::string& manifest_extra = "") {
  files["dreamsy.toml"] = "format = \"0\"\nname = \"P\"\ndimension = \"2d\"\nentry_scene = \"scenes/main.scene.json\"\n" +
                          manifest_extra +
                          "\n[style]\npreset = \"pixel\"\npalette = [\"#101020\", \"#3355ff\", \"#ffcc00\", \"#f0f0f0\"]\n";
  files["scenes/main.scene.json"] = R"({"format": "0", "root": "world", "entities": {"world": {"type": "Node2D", "children": [)" +
                                    top + "]}" + (entities.empty() ? "" : ", " + entities) + "}}";
  return files;
}

bool has_line(const Strs& lines, const std::string& line) { return std::find(lines.begin(), lines.end(), line) != lines.end(); }

}  // namespace

TEST_SUITE("api platform_api") {

TEST_CASE("platform_api: data reader (data.list / data.read, alias files.*): lists package dirs and reads JSON/text, read-only copies, clear errors") {
  FileMap files;
  files["data/creatures/fox.json"] = R"({"name": "Fox", "hp": 12, "moves": ["bite", "dash"]})";
  files["data/creatures/owl.json"] = R"({"name": "Owl", "hp": 9, "moves": []})";
  files["data/creatures/notes.txt"] = "hello";
  files["data/bad.json"] = "{nope";
  files["scripts/client/m.luau"] = R"(
        local list = data.list("data/creatures")
        print(table.concat(list, ","))
        local fox = data.read("data/creatures/fox.json")
        print(fox.name, fox.hp, #fox.moves, fox.moves[2])
        fox.hp = 999
        print("fresh", data.read("data/creatures/fox.json").hp, files.read("data/creatures/notes.txt"), data.exists("data/creatures/owl.json"))
        print(pcall(function() return data.read("data/missing.json") end))
        print(pcall(function() return data.read("data/bad.json") end))
        print(pcall(function() return data.read("../dreamsy.toml") end))
        print(#data.list("data/nothing"))
      )";
  Game g = start(make_pkg("", "", files));
  CHECK_NO_ERRORS(g);
  const Strs logs = g.logs();
  const std::string l = join(logs);
  CHECK(contains(l, "data/creatures/fox.json,data/creatures/notes.txt,data/creatures/owl.json"));
  CHECK(contains(l, "Fox\t12\t2\tdash"));
  CHECK(contains(l, "fresh\t12\thello\ttrue"));
  CHECK(matches(l, R"(no file "data\/missing.json")"));
  CHECK(matches(l, R"(data\/bad.json is not valid JSON)"));
  CHECK(matches(l, R"(bad path "..\/dreamsy.toml")"));
  // TS /\n0$/m: a later line that is exactly "0"
  CHECK(std::find(logs.begin() + 1, logs.end(), "0") != logs.end());
}

TEST_CASE("platform_api: clock, timeScale, session, theme: clock.now() is the injected clock (fixed in playtests), wall clock by default") {
  FileMap files;
  files["scripts/client/c.luau"] = R"(after(1, function() print("now " .. clock.now()) end))";
  PlaytestReport r = simulate(make_pkg("", "", files), {}, 1.5);
  CHECK(contains(join(r.logs), "now " + std::to_string(1767225600 + 1)));  // PLAYTEST_EPOCH + 1
  FileMap wall_files;
  wall_files["scripts/client/c.luau"] = R"(print("wall " .. math.floor(clock.now())))";
  Game g = start(make_pkg("", "", wall_files));
  const Strs wall = g.logs_with("wall ");
  REQUIRE(wall.size() == 1);
  const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  CHECK(std::fabs(std::stod(wall[0]) - now) < 5);
}

TEST_CASE("platform_api: clock, timeScale, session, theme: game.timeScale scales dt, timers, tweens and physics; 0 freezes them (hit-stop)") {
  // TS sets g.timeScale from the test after ticks 60 and 90. The C++ host has no public setter, so
  // an extra onTick sets game.timeScale at the end of those ticks (the scaled dt of a tick is taken
  // at its start, so it applies from the next tick: the same ticks as in TS).
  FileMap files;
  files["scripts/client/t.luau"] = R"(
        local total = 0
        onTick(function(dt) total += dt end)
        game.timeScale = 0.5
        print("read back " .. game.timeScale)
        tween(entity("s"), { position = vec2(100, 0) }, 1, "linear")
        after(0.5, function() print("timer at tick", total) end)
        every(2, function() print("ts " .. game.timeScale .. " total " .. string.format("%.3f", total)) end)
        local ok, err = pcall(function() game.nope = 1 end)
        print(err)
        local ticks = 0
        onTick(function()
          ticks += 1
          if ticks == 60 then game.timeScale = 0 elseif ticks == 90 then game.timeScale = 1 end
        end)
      )";
  Game g = start(make_pkg(R"("box", "s")",
                          R"("box": {"type": "Body2D", "props": {"position": [0, 0], "body": "dynamic"}},
                             "s": {"type": "Sprite2D", "props": {"position": [0, 0]}})",
                          files));
  g.step(60);  // 1 real second at 0.5x = 0.5 game seconds
  CHECK_CLOSE(g.pos("s").x, 50, 5);
  CHECK_CLOSE(g->time(), 0.5, 6);
  const double y1 = g.pos("box").y;
  g.step(30);  // timeScale 0
  CHECK(g.pos("box").y == y1);
  CHECK_CLOSE(g.pos("s").x, 50, 5);
  g.step(30);  // timeScale 1
  CHECK_CLOSE(g.pos("s").x, 100, 5);
  const Strs logs = g.logs();
  CHECK(std::any_of(logs.begin(), logs.end(), [](const std::string& l) { return matches(l, "game.nope is read-only"); }));
  CHECK(has_line(logs, "read back 0.5"));  // live read (not a load-time import constant)
  CHECK(!g.logs_with("timer at tick").empty());
}

TEST_CASE("platform_api: clock, timeScale, session, theme: game.session keeps connections alive when the creating entity dies; ui.theme() returns the [ui] theme") {
  FileMap files;
  files["scripts/client/e.luau"] = R"(
        session_ticks, own_ticks = 0, 0
        onTick(function() own_ticks += 1 end)
        game.session(function() onTick(function() session_ticks += 1 if session_ticks == 30 then print("session alive", own_ticks) end end) end)
        after(0.1, function() script.entity:destroy() end)
        local t = ui.theme()
        print(t.primary, t.accent, t.background, t.text, t.panel, t.font)
      )";
  Game g = start(make_pkg(R"("e")", R"("e": {"type": "Node2D", "scripts": ["scripts/client/e.luau"]})", files,
                          "[ui]\nprimary = \"#112233\"\npanel = \"rounded\""));
  g.step(40);
  const std::string l = g.log_text();
  CHECK(matches(l, R"(session alive\t\d)"));
  std::smatch m;
  REQUIRE(std::regex_search(l, m, std::regex(R"(session alive\t(\d+))")));
  CHECK(std::stoi(m[1].str()) < 10);  // the entity's own onTick died with it
  CHECK(matches(l, R"(#112233\t#[0-9a-f]{6}\t#[0-9a-f]{6}\t#[0-9a-f]{6}\trounded\tpixel)"));
}

TEST_CASE("platform_api: UI props and TileMap2D taps: input_transparent panels let taps through; UI scale is tweenable without changing the hit rect") {
  FileMap files;
  files["scripts/client/b.luau"] = R"(
        script.entity.pressed:connect(function() print("pressed") end)
        entity("label").tapped:connect(function() print("label tapped") end)
        tween(script.entity, { scale = vec2(1.2, 1.2) }, 0.2, { yoyo = true })
      )";
  Game g = start(make_pkg(R"("btn", "frame", "label")", R"(
      "btn": {"type": "UIButton", "props": {"anchor": "center", "size": [200, 100]}, "scripts": ["scripts/client/b.luau"]},
      "frame": {"type": "UIPanel", "props": {"anchor": "center", "size": [600, 600], "z_index": 5, "input_transparent": true}},
      "label": {"type": "UIText", "props": {"anchor": "top", "size": [300, 60], "input_transparent": false, "text": "tap me"}})",
                          files));
  CHECK(g.tap(640, 360) == "btn");
  CHECK(g.tap(640, 30) == "label");
  g.step(12);
  std::optional<world::Value> scale = g->world().get_prop(g.entity("btn"), "scale");
  REQUIRE(scale);
  CHECK(scale->v[0] > 1.1);
  const ui::Rect r = ui::ui_rect_headless(g->world(), g.id("btn"), 1280, 720);
  CHECK(r.x == 540);
  CHECK(r.y == 310);
  CHECK(r.w == 200);
  CHECK(r.h == 100);
  CHECK(has_line(g.logs(), "pressed"));
  CHECK(has_line(g.logs(), "label tapped"));
}

TEST_CASE("platform_api: UI props and TileMap2D taps: TileMap2D fires tileTapped(col, row, char) and tapped") {
  FileMap files;
  files["data/tilesets/t.json"] = R"({"tiles": {"#": {"color": "#888888", "solid": true}}})";
  files["scripts/client/m.luau"] = R"(
        script.entity.tileTapped:connect(function(c, r, ch) print("tile", c, r, ch) end)
        script.entity.tapped:connect(function() print("map tapped") end)
      )";
  Game g = start(make_pkg(R"("map")",
                          R"("map": {"type": "TileMap2D", "props": {"position": [100, 100], "tile_size": 16, "tileset": "data/tilesets/t.json", "data": "#.#\n###"}, "scripts": ["scripts/client/m.luau"]})",
                          files));
  const world::EntityId hit = g->tap_world(100 + 16 * 2 + 3, 100 + 16 + 3);
  REQUIRE(g->world().alive(hit));
  CHECK(g->world().get(hit)->id == "map");
  g->tap_world(100 + 16 + 3, 100 + 3);  // empty cell -> char nil
  CHECK(g->tap_world(0, 0).is_null());   // outside
  const Strs l = g.logs();
  CHECK(has_line(l, "tile\t2\t1\t#"));
  CHECK(has_line(l, "tile\t1\t0\tnil"));
  CHECK(std::count(l.begin(), l.end(), "map tapped") == 2);
}

TEST_CASE("platform_api: long simulations: budgetMs raises the per-call Luau budget for headless sims (default stays 250ms)") {
  // Tests the budget itself: exact values (no budget() scaling).
  FileMap heavy;
  heavy["scripts/client/h.luau"] = R"(
    onTick(function()
      local t0 = os.clock()
      while os.clock() - t0 < 0.4 do end
      print("survived")
    end)
  )";
  Game short_run = start(make_pkg("", "", heavy));
  short_run->step();
  Strs messages;
  for (const auto& e : short_run->errors()) messages.push_back(e.message);
  CHECK(matches(join(messages, ","), R"(execution budget \(250 ms\))"));
  script::HostOptions o;
  o.budget_ms = 5000;
  Game long_run = start(make_pkg("", "", heavy), o);
  long_run->step();
  CHECK_NO_ERRORS(long_run);
  CHECK(has_line(long_run.logs(), "survived"));
}

TEST_CASE("platform_api: kit-agent fixes: spawn TileMap2D with tileset+data; map:isSolid / map:tileDef; e.destroyed and isEntity never throw") {
  FileMap files;
  files["data/tilesets/t.json"] =
      R"({"tiles": {"#": {"color": "#888888", "solid": true, "tags": ["wall"]}, "^": {"sprite": "asset:sp", "tags": ["spike"]}}})";
  files["assets/sp.svg"] = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"><rect width="8" height="8"/></svg>)";
  files["assets/sp.meta.json"] = R"({"kind": "image", "file": "sp.svg"})";
  files["data/rooms/a.txt"] = "#^#\n#.#\n";
  files["scripts/client/m.luau"] = R"(
        local map = spawn("TileMap2D", { tileset = "data/tilesets/t.json", data = data.read("data/rooms/a.txt"), tile_size = 16 })
        print("dims", map.cols, map.rows, map:isSolid(0, 0), map:isSolid(1, 0), map:isSolid(1, 1), map:isSolid(9, 9))
        local d = map:tileDef("#")
        print("def", d.char, d.solid, d.tags[1], d.color, map:tileDef("^").sprite, map:tileDef("z"))
        local x = spawn("Node2D")
        print("alive", x.destroyed, isEntity(x), isEntity({}), isEntity(nil), isEntity(vec2(1, 2)))
        x:destroy()
        print("dead", x.destroyed, isEntity(x))
      )";
  Game g = start(make_pkg(R"("e")", R"("e": {"type": "Node2D", "scripts": ["scripts/client/m.luau"]})", files));
  CHECK_NO_ERRORS(g);
  const Strs l = g.logs();
  CHECK(has_line(l, "dims\t3\t2\ttrue\tfalse\tfalse\tfalse"));
  CHECK(has_line(l, "def\t#\ttrue\twall\t#888888\tasset:sp\tnil"));
  CHECK(has_line(l, "alive\tfalse\ttrue\tfalse\tfalse\tfalse"));
  CHECK(has_line(l, "dead\ttrue\ttrue"));
}

}  // TEST_SUITE
