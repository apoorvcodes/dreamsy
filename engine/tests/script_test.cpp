// Script host (E2). Behavior parity with engine-web is checked by tests/conformance/run_compare.py;
// these tests cover the host natively (no Node needed) and the safety limits.
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "pkg/validate.h"
#include "script/host.h"

using namespace dreamsy;
using namespace dreamsy::script;

namespace {

const std::string kRoot = DREAMSY_REPO_ROOT;

// A minimal valid package whose only content is `scripts`.
std::shared_ptr<const pkg::Package> package_with(std::map<std::string, std::string> files) {
  files["dreamsy.toml"] = "format = \"0\"\nname = \"t\"\ndimension = \"2d\"\nentry_scene = \"scenes/main.scene.json\"\n";
  files["scenes/main.scene.json"] = R"({"format": "0", "root": "r", "entities": {"r": {"type": "Folder"}}})";
  auto f = pkg::Files::from_map(files);
  REQUIRE(f.ok());
  pkg::LoadResult r = pkg::load_package(std::move(f).value());
  REQUIRE(r.ok);
  return std::shared_ptr<const pkg::Package>(std::move(r.package));
}

std::shared_ptr<const pkg::Package> package_dir(const std::string& rel) {
  auto f = pkg::Files::from_dir(kRoot + "/" + rel);
  REQUIRE(f.ok());
  pkg::LoadResult r = pkg::load_package(std::move(f).value());
  REQUIRE(r.ok);
  return std::shared_ptr<const pkg::Package>(std::move(r.package));
}

std::string all_logs(const Host& h) {
  std::string out;
  for (const LogLine& l : h.logs()) out += "[" + l.level + "] " + l.text + "\n";
  return out;
}

}  // namespace

TEST_CASE("host runs session scripts, timers and onTick in engine-web order") {
  auto pkg = package_with({{"scripts/client/main.luau", R"(
    print("client")
    local n = 0
    onTick(function() n += 1 end)
    after(0.1, function() print("after", n) end)
  )"},
                           {"scripts/server/main.luau", R"(print("server"))"}});
  auto host = Host::create(pkg);
  REQUIRE_MESSAGE(host.ok(), host.error());
  for (int i = 0; i < 10; ++i) host.value()->step();
  std::string logs = all_logs(*host.value());
  // server dir first; the timer fires at tick 6 (0.1 s), before that tick's onTick: n = 5.
  CHECK(logs == "[info] server\n[info] client\n[info] after\t5\n");
  CHECK(host.value()->errors().empty());
}

TEST_CASE("run ends reach the host app: platform.runEnded, leaderboard finish/submit, room.finish") {
  auto pkg = package_with({{"scripts/client/main.luau", R"(
    platform.runEnded()
    platform.runEnded("dead")
    platform.runEnded("quit")
    leaderboard.start("time")
    after(0.5, function() leaderboard.finish("time") end)
    after(0.6, function() leaderboard.submit("score", 1200) end)
    after(0.7, function() room.finish() end)
  )"}});
  HostOptions o;
  std::vector<RunEnd> ends;
  o.on_run_end = [&](const RunEnd& r) { ends.push_back(r); };
  auto host = Host::create(pkg, o);
  REQUIRE_MESSAGE(host.ok(), host.error());
  for (int i = 0; i < 60; ++i) host.value()->step();
  CHECK(host.value()->errors().empty());
  REQUIRE(ends.size() == 6);
  CHECK(ends[0].reason == "gameover");
  CHECK(ends[1].reason == "gameover");  // unknown reasons end the run as a game over (engine-web)
  CHECK(ends[2].reason == "quit");
  CHECK(ends[3].reason == "finish");
  CHECK(ends[3].board == "time");
  CHECK(ends[3].value.value_or(0) == doctest::Approx(0.5).epsilon(0.05));
  CHECK(ends[4].reason == "submit");
  CHECK(ends[4].board == "score");
  CHECK(ends[4].value == 1200);
  CHECK(ends[5].reason == "finish");
  CHECK(!ends[5].board);
}

TEST_CASE("the same seed gives the same random numbers; another seed doesn't") {
  auto pkg = package_with({{"scripts/client/main.luau", R"(print(random(), math.random(100)))"}});
  auto run = [&](uint32_t seed) {
    HostOptions o;
    o.seed = seed;
    auto h = Host::create(pkg, o);
    REQUIRE(h.ok());
    return all_logs(*h.value());
  };
  CHECK(run(1) == run(1));
  CHECK(run(1) != run(2));
}

TEST_CASE("slow top-level script and module setup can finish and register onTick") {
  auto pkg = package_with({{"scripts/client/main.luau", R"(
    local start = os.clock()
    while os.clock() - start < 0.5 do end
    local message = require("../shared/setup")
    onTick(function() print(message) end)
  )"},
                           {"scripts/shared/setup.luau", R"(
    local start = os.clock()
    while os.clock() - start < 0.5 do end
    return "tick after setup"
  )"}});
  auto host = Host::create(pkg);
  REQUIRE_MESSAGE(host.ok(), host.error());
  host.value()->step();
  CHECK(host.value()->errors().empty());
  CHECK(all_logs(*host.value()) == "[info] tick after setup\n");
}

TEST_CASE("a runaway top-level loop is killed by the load budget and pcall can't swallow it") {
  auto pkg = package_with({{"scripts/client/main.luau", R"(
    local ok = pcall(function() while true do end end)
    print("unreachable", ok)
  )"},
                           {"scripts/client/next.luau", R"(print("next script runs"))"}});
  HostOptions o;
  int expected_ms = 2000;
  SUBCASE("default load budget") {}
  SUBCASE("a larger callback budget also raises the load budget") {
    o.load_budget_ms = 20;
    o.budget_ms = 30;
    expected_ms = 30;
  }
  auto t0 = std::chrono::steady_clock::now();
  auto host = Host::create(pkg, o);
  auto elapsed = std::chrono::steady_clock::now() - t0;
  REQUIRE(host.ok());
  CHECK(elapsed >= std::chrono::milliseconds(expected_ms));
  REQUIRE(host.value()->errors().size() == 1);
  CHECK(host.value()->errors()[0].message ==
        "script exceeded its execution budget (" + std::to_string(expected_ms) + " ms)");
  CHECK(all_logs(*host.value()).find("next script runs") != std::string::npos);
  CHECK(all_logs(*host.value()).find("unreachable") == std::string::npos);
}

TEST_CASE("a module required from onTick shares the callback deadline") {
  auto pkg = package_with({{"scripts/client/main.luau", R"(
    onTick(function()
      require("../shared/setup")
      print("unreachable")
    end)
  )"},
                           {"scripts/shared/setup.luau", R"(
    local start = os.clock()
    while os.clock() - start < 0.5 do end
    return {}
  )"}});
  auto host = Host::create(pkg);
  REQUIRE_MESSAGE(host.ok(), host.error());
  REQUIRE(host.value()->errors().empty());
  host.value()->step();
  REQUIRE(host.value()->errors().size() == 1);
  CHECK(host.value()->errors()[0].message == "script exceeded its execution budget (250 ms)");
  CHECK(all_logs(*host.value()).find("unreachable") == std::string::npos);
}

TEST_CASE("the memory cap stops a script that allocates without bound") {
  auto pkg = package_with({{"scripts/client/main.luau", R"(
    local t = {}
    for i = 1, 1e9 do t[i] = string.rep("x", 1000) .. i end
  )"}});
  HostOptions o;
  o.memory_limit = 8u * 1024 * 1024;
  auto host = Host::create(pkg, o);
  REQUIRE(host.ok());
  REQUIRE(host.value()->errors().size() == 1);
  CHECK(host.value()->errors()[0].message.find("not enough memory") != std::string::npos);
  CHECK(host.value()->memory_used() < o.memory_limit);
}

TEST_CASE("the sandbox: no io, os.execute, loadstring, debug; globals frozen") {
  auto pkg = package_with({{"scripts/client/main.luau", R"(
    print(io == nil, os.execute == nil, loadstring == nil, debug == nil, getfenv == nil, type(os.clock))
    print(pcall(function() math.floor = nil end))
  )"}});
  auto host = Host::create(pkg);
  REQUIRE(host.ok());
  std::string logs = all_logs(*host.value());
  CHECK(logs.find("[info] true\ttrue\ttrue\ttrue\ttrue\tfunction") != std::string::npos);
  CHECK(logs.find("[info] false") != std::string::npos);  // writing to a frozen library fails
}

TEST_CASE("errors carry the script path and line; repeats are counted once") {
  auto pkg = package_with({{"scripts/client/main.luau", "onTick(function()\n  local t = nil\n  return t.x\nend)\n"}});
  auto host = Host::create(pkg);
  REQUIRE(host.ok());
  for (int i = 0; i < 5; ++i) host.value()->step();
  REQUIRE(host.value()->errors().size() == 1);
  const ScriptError& e = host.value()->errors()[0];
  CHECK(e.script == "scripts/client/main.luau");
  CHECK(e.line == 3);
  CHECK(e.count == 5);
  CHECK(e.message == "attempt to index nil with 'x'");
}

TEST_CASE("kits load from the embedded sources") {
  // The validator rejects literal requires of kits that aren't enabled; a computed name reaches the host,
  // which must refuse it with the dreamsy.toml hint.
  auto pkg = package_with({{"scripts/client/main.luau", R"(local name = "@kit/" .. "core"; print(typeof(require(name))))"}});
  auto host = Host::create(pkg);
  REQUIRE(host.ok());
  REQUIRE(host.value()->errors().size() == 1);
  CHECK(host.value()->errors()[0].message.find("add kits = [\"core\"] to dreamsy.toml") != std::string::npos);
}

TEST_CASE("the script-only conformance games run without errors except the expected ones") {
  for (const char* name : {"basics", "modules", "data_save", "scenes_room"}) {
    CAPTURE(name);
    auto host = Host::create(package_dir(std::string("engine/tests/scripts/") + name));
    REQUIRE(host.ok());
    for (int i = 0; i < 120; ++i) host.value()->step();
    CHECK_MESSAGE(host.value()->errors().empty(), all_logs(*host.value()));
  }
}
