// dreamsy-tool: the headless command-line engine.
//
//   dreamsy-tool version
//   dreamsy-tool validate <package> [--json]   check a package; exit 1 on errors
//   dreamsy-tool info <package>                summary + content hash
//   dreamsy-tool pack <package> <out.dpk>      validate, then write a Dreamsy pack
//   dreamsy-tool unpack <pack.dpk> <dir>       extract a pack into a directory
//   dreamsy-tool run <package> [--ticks N] [--seed S] [--budget MS] [--load-budget MS] [--input PLAN] [--dump SPEC] [--hash]
//                    [--view side|topdown|oblique|iso] [--height ID=H,...] [--zjump TICK:ID=VZ;...]
//                                              run headless (see cmd_run)
//   dreamsy-tool check <package> [--strict] [--json]   type-check scripts; exit 1 on errors
//   dreamsy-tool check --print-defs                    print the Luau API definitions
//   dreamsy-tool playtest <package> --timeline JSON ...   scripted playtest -> JSON report (playtest.cpp)
//   dreamsy-tool eval --script FILE --kits a,b ...         run a Luau snippet (kit sims) -> JSON (playtest.cpp)
//   dreamsy-tool smoke [--seed N] [--bodies N] [--steps N]
//
// <package> is a directory, a .dpk pack, or a legacy .zip (converted on the fly).
// Later slices add: replay, screenshot (docs/engine.md §14.3).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/log.h"
#include "base/version.h"
#include "dev/smoke.h"
#include "pkg/json.h"
#include "pkg/text.h"
#include "pkg/validate.h"
#include "pkg/zip_import.h"
#include "check/check.h"
#include "script/embedded_kits.h"
#include "script/host.h"
#include "tool.h"

using namespace dreamsy;
namespace fs = std::filesystem;

namespace dreamsy::tool {

Result<std::shared_ptr<pkg::Files::Bytes>> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return Error{"cannot open " + path};
  auto size = static_cast<size_t>(in.tellg());
  auto bytes = std::make_shared<pkg::Files::Bytes>(size);
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size))) {
    return Error{"cannot read " + path};
  }
  return bytes;
}

Result<pkg::Files> load_files(const std::string& path) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) return pkg::Files::from_dir(path);
  auto bytes = read_file(path);
  if (!bytes) return Error{bytes.error()};
  if (pkg::ends_with(path, ".zip")) return pkg::import_zip(*bytes.value());
  return pkg::Files::from_pack(std::move(bytes).value());
}

}  // namespace dreamsy::tool

using dreamsy::tool::load_files;
using dreamsy::tool::read_file;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  dreamsy-tool version\n"
               "  dreamsy-tool validate <package> [--json]\n"
               "  dreamsy-tool info <package>\n"
               "  dreamsy-tool pack <package> <out.dpk>\n"
               "  dreamsy-tool unpack <pack.dpk> <dir>\n"
               "  dreamsy-tool run <package> [--ticks N] [--seed S] [--budget MS] [--load-budget MS] [--input PLAN] [--dump SPEC] [--hash]\n"
               "                   [--view MODE] [--height ID=H,...] [--zjump TICK:ID=VZ;...]\n"
               "  dreamsy-tool check <package> [--strict] [--json] | --print-defs\n"
               "  dreamsy-tool playtest <package> [--timeline JSON|@file] [--seconds N] [--scene PATH] [--params JSON]\n"
               "                   [--save JSON] [--clock UNIX] [--auto-dialog first|last|N] [--budget MS] [--seed S]\n"
               "  dreamsy-tool eval --script FILE|- --kits a,b [--base <package>] [--ticks N] [--until KEY] [--max-ticks N]\n"
               "                   [--budget MS] [--seed S] [--clock UNIX] [--save JSON]\n"
               "  dreamsy-tool smoke [--seed N] [--bodies N] [--steps N]\n"
               "<package> is a directory, a .dpk pack or a .zip\n");
  return 2;
}

Status write_file(const std::string& path, std::span<const uint8_t> data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out || !out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
    return Error{"cannot write " + path};
  }
  return Status::success();
}

void print_issues(const std::vector<pkg::Issue>& issues) {
  for (const pkg::Issue& i : issues) std::fprintf(stderr, "%s\n", pkg::format_issue(i).c_str());
}

std::string issues_json(const pkg::LoadResult& r) {
  std::string out = "{\"ok\":" + std::string(r.ok ? "true" : "false") + ",\"issues\":[";
  for (size_t k = 0; k < r.issues.size(); ++k) {
    const pkg::Issue& i = r.issues[k];
    if (k) out += ",";
    out += "{\"severity\":" + pkg::quote(i.severity == pkg::Severity::Error ? "error" : "warning") +
           ",\"code\":" + pkg::quote(i.code) + ",\"message\":" + pkg::quote(i.message);
    if (!i.file.empty()) out += ",\"file\":" + pkg::quote(i.file);
    if (!i.id.empty()) out += ",\"id\":" + pkg::quote(i.id);
    out += "}";
  }
  return out + "]}";
}

int cmd_validate(int argc, char** argv) {
  if (argc < 3) return usage();
  bool json = argc > 3 && std::string_view(argv[3]) == "--json";
  auto files = load_files(argv[2]);
  if (!files) {
    if (json) std::printf("{\"ok\":false,\"error\":%s,\"issues\":[]}\n", pkg::quote(files.error()).c_str());
    else std::fprintf(stderr, "✗ %s\n", files.error().c_str());
    return 1;
  }
  pkg::LoadResult r = pkg::load_package(std::move(files).value());
  if (json) {
    std::printf("%s\n", issues_json(r).c_str());
  } else {
    print_issues(r.issues);
    size_t errors = 0;
    for (const pkg::Issue& i : r.issues) errors += i.severity == pkg::Severity::Error;
    if (r.ok) std::fprintf(stderr, "✓ valid (%zu warnings)\n", r.issues.size());
    else std::fprintf(stderr, "✗ %zu errors\n", errors);
  }
  return r.ok ? 0 : 1;
}

int cmd_info(int argc, char** argv) {
  if (argc < 3) return usage();
  auto files = load_files(argv[2]);
  if (!files) {
    std::fprintf(stderr, "✗ %s\n", files.error().c_str());
    return 1;
  }
  std::string hash = to_hex(files.value().content_hash());
  size_t file_count = files.value().count();
  uint64_t bytes = files.value().total_bytes();
  pkg::LoadResult r = pkg::load_package(std::move(files).value());
  std::printf("files      %zu (%llu bytes)\ncontent    sha256:%s\n", file_count, static_cast<unsigned long long>(bytes),
              hash.c_str());
  if (!r.ok) {
    print_issues(r.issues);
    return 1;
  }
  const pkg::Package& p = *r.package;
  const pkg::Manifest& m = p.manifest;
  size_t entities = 0;
  for (const auto& [_, s] : p.scenes) entities += s.entities.size();
  std::printf("name       %s\norientation %s (%dx%d)\nentry      %s\nplayers    %d-%d (server: %s)\n", m.name.c_str(),
              m.portrait() ? "portrait" : "landscape", m.reference_width(), m.reference_height(), m.entry_scene.c_str(),
              m.players_min, m.players_max, m.server_mode.c_str());
  std::string kits = m.kits ? pkg::join(*m.kits, ", ") : "-";
  std::printf("kits       %s\nstyle      %s\nscenes     %zu (%zu entities)\nassets     %zu\ndata       %zu tilesets, %zu anim sets, %zu sfx, %zu songs\n",
              kits.c_str(), m.style.preset.c_str(), p.scenes.size(), entities, p.assets.size(), p.tilesets.size(),
              p.anims.size(), p.sfx.size(), p.music.size());
  return 0;
}

int cmd_pack(int argc, char** argv) {
  if (argc < 4) return usage();
  auto files = load_files(argv[2]);
  if (!files) {
    std::fprintf(stderr, "✗ %s\n", files.error().c_str());
    return 1;
  }
  pkg::Files::Bytes packed = files.value().to_pack();
  std::string hash = to_hex(files.value().content_hash());
  pkg::LoadResult r = pkg::load_package(std::move(files).value());
  print_issues(r.issues);
  if (!r.ok) {
    std::fprintf(stderr, "✗ not packed: the package has errors\n");
    return 1;
  }
  if (Status s = write_file(argv[3], packed); !s) {
    std::fprintf(stderr, "✗ %s\n", s.error().c_str());
    return 1;
  }
  std::printf("%s  %zu bytes  sha256:%s\n", argv[3], packed.size(), hash.c_str());
  return 0;
}

int cmd_unpack(int argc, char** argv) {
  if (argc < 4) return usage();
  auto files = load_files(argv[2]);
  if (!files) {
    std::fprintf(stderr, "✗ %s\n", files.error().c_str());
    return 1;
  }
  const fs::path root = argv[3];
  for (const std::string& p : files.value().paths()) {
    fs::path out = root / p;  // paths are normalized: no "..", no absolute paths
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (ec) {
      std::fprintf(stderr, "✗ %s: %s\n", out.string().c_str(), ec.message().c_str());
      return 1;
    }
    if (Status s = write_file(out.string(), *files.value().bytes(p)); !s) {
      std::fprintf(stderr, "✗ %s\n", s.error().c_str());
      return 1;
    }
  }
  std::printf("%zu files -> %s\n", files.value().count(), argv[3]);
  return 0;
}

bool parse_int(const char* text, int64_t min, int64_t max, int64_t* out);

int cmd_check(int argc, char** argv) {
  if (argc >= 3 && std::string_view(argv[2]) == "--print-defs") {
    std::fwrite(check::definitions().data(), 1, check::definitions().size(), stdout);
    return 0;
  }
  if (argc < 3) return usage();
  check::Options opts;
  bool json = false;
  for (int i = 3; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "--strict") opts.strict = true;
    else if (a == "--json") json = true;
    else return usage();
  }
  auto files = load_files(argv[2]);
  if (!files) {
    std::fprintf(stderr, "✗ %s\n", files.error().c_str());
    return 1;
  }
  pkg::LoadResult loaded = pkg::load_package(std::move(files).value());
  if (!loaded.ok) {
    print_issues(loaded.issues);
    return 1;
  }
  auto report = check::check_package(*loaded.package, opts);
  if (!report) {
    std::fprintf(stderr, "✗ %s\n", report.error().c_str());
    return 1;
  }
  const check::Report& r = report.value();
  if (json) {
    std::string out = "{\"errors\":" + std::to_string(r.errors) + ",\"warnings\":" + std::to_string(r.warnings) + ",\"issues\":[";
    for (size_t k = 0; k < r.issues.size(); ++k) {
      const check::Issue& i = r.issues[k];
      out += std::string(k ? "," : "") + "{\"file\":" + pkg::quote(i.file) + ",\"line\":" + std::to_string(i.line) +
             ",\"column\":" + std::to_string(i.column) + ",\"severity\":" + pkg::quote(i.severity) +
             ",\"kind\":" + pkg::quote(i.kind) + ",\"message\":" + pkg::quote(i.message) + "}";
    }
    std::printf("%s]}\n", out.c_str());
  } else {
    for (const check::Issue& i : r.issues) {
      std::printf("%s:%d:%d: %s [%s] %s\n", i.file.c_str(), i.line, i.column, i.severity.c_str(), i.kind.c_str(), i.message.c_str());
    }
    std::fprintf(stderr, "%s %d errors, %d warnings\n", r.errors ? "✗" : "✓", r.errors, r.warnings);
  }
  return r.errors ? 1 : 0;
}

// A scripted input plan: what a player presses, tick by tick (ticks count from 1).
//   "30-200:move_x=1"   axis move_x = 1 on ticks 30..200 (back to 0 after)
//   "40-45:jump"        button jump held on ticks 40..45
//   "60:jump"           a tap of jump on tick 60
// Items are separated by ';'. engine/tests/conformance/ts_run.mts reads the same format.
struct InputItem {
  int64_t from = 0, to = 0;
  std::string name;
  bool axis = false, tap = false;
  double value = 0;
};

Result<std::vector<InputItem>> parse_input_plan(std::string_view plan) {
  std::vector<InputItem> out;
  for (std::string_view item : pkg::split(plan, ';')) {
    item = pkg::trim(item);
    if (item.empty()) continue;
    size_t colon = item.find(':');
    if (colon == std::string_view::npos) return Error{"input item \"" + std::string(item) + "\" needs ticks:name"};
    std::string ticks(item.substr(0, colon)), what(item.substr(colon + 1));
    InputItem it;
    size_t dash = ticks.find('-');
    int64_t a = 0, b = 0;
    if (!parse_int(ticks.substr(0, dash).c_str(), 1, 10000000, &a)) return Error{"bad tick in \"" + std::string(item) + "\""};
    b = a;
    if (dash != std::string::npos && !parse_int(ticks.substr(dash + 1).c_str(), a, 10000000, &b)) return Error{"bad tick range in \"" + std::string(item) + "\""};
    it.from = a, it.to = b, it.tap = dash == std::string::npos;
    size_t eq = what.find('=');
    if (eq != std::string::npos) {
      it.axis = true, it.tap = false;
      it.value = std::strtod(what.c_str() + eq + 1, nullptr);
      what = what.substr(0, eq);
    }
    it.name = what;
    out.push_back(it);
  }
  return out;
}

void apply_input(input::InputState& input, const std::vector<InputItem>& plan, int64_t tick) {
  for (const InputItem& it : plan) {
    if (it.tap) {
      if (tick == it.from) input.tap(it.name);
    } else if (it.axis) {
      if (tick == it.from) input.set_axis(it.name, it.value);
      if (tick == it.to + 1) input.set_axis(it.name, 0);
    } else {
      if (tick == it.from) input.set_button(it.name, true);
      if (tick == it.to + 1) input.set_button(it.name, false);
    }
  }
}

// "--dump N:a,b,#tag": every N ticks print "dump <tick> <id> <x> <y> [<vx> <vy>]" for the
// entities with id a and b and every entity tagged `tag` (global position; velocity for Body2D).
void dump_entities(script::Host& host, int64_t tick, const std::vector<std::string>& selectors, bool with_z) {
  world::World& w = host.world();
  for (const std::string& sel : selectors) {
    std::vector<world::EntityId> ids;
    if (pkg::starts_with(sel, "#")) ids = w.tagged(sel.substr(1));
    else if (w.alive(w.by_id(sel))) ids.push_back(w.by_id(sel));
    for (world::EntityId id : ids) {
      const world::Entity* e = w.get(id);
      const world::Mat2D m = w.global(id);
      std::string line = "dump " + std::to_string(tick) + " " + e->id + " " + pkg::number_text(m.tx) + " " + pkg::number_text(m.ty);
      if (e->type->name == "Body2D" && host.physics()) {
        const world::Point v = host.physics()->velocity(id);
        line += " " + pkg::number_text(v.x) + " " + pkg::number_text(v.y);
      }
      if (with_z) line += " z=" + pkg::number_text(e->z);
      std::printf("%s\n", line.c_str());
    }
  }
}

// Runs the package for N ticks with a fixed clock and prints the same report as
// engine/tests/conformance/ts_run.mts does for engine-web: logs as they happen, dumps, then
// errors, audio events, save data and game time. --hash adds "hash <tick> <state hash>" per tick.
//
// 2.5D test overrides (packages set these themselves since slice E7: manifest/scene `view`,
// entity `z`/`height`/`z_velocity`): --view forces every scene's view mode, --height ID=H gives
// entities a vertical extent, --zjump TICK:ID=VZ sets an entity's z velocity (px/s, up) before
// that tick. With any of them, dumps add the entity's z.
int cmd_run(int argc, char** argv) {
  if (argc < 3) return usage();
  int64_t ticks = 120, seed = 1, budget = 250, load_budget = 2000, dump_every = 0;
  bool hash = false;
  std::optional<world::ViewMode> view;
  std::vector<std::pair<std::string, double>> heights;
  struct ZJump {
    int64_t tick;
    std::string id;
    double vz;
  };
  std::vector<ZJump> zjumps;
  std::vector<InputItem> plan;
  std::vector<std::string> dump_selectors;
  for (int i = 3; i < argc; ++i) {
    std::string_view flag = argv[i];
    if (flag == "--hash") {
      hash = true;
      continue;
    }
    if (i + 1 >= argc) return usage();
    std::string_view value = argv[++i];
    if (flag == "--input") {
      auto p = parse_input_plan(value);
      if (!p) {
        std::fprintf(stderr, "✗ --input: %s\n", p.error().c_str());
        return 2;
      }
      plan = std::move(p).value();
      continue;
    }
    if (flag == "--dump") {
      size_t colon = value.find(':');
      if (colon == std::string_view::npos || !parse_int(std::string(value.substr(0, colon)).c_str(), 1, 1000000, &dump_every)) return usage();
      for (std::string_view sel : pkg::split(value.substr(colon + 1), ','))
        if (!pkg::trim(sel).empty()) dump_selectors.emplace_back(pkg::trim(sel));
      continue;
    }
    if (flag == "--view") {
      const world::ViewMode modes[] = {world::ViewMode::Side, world::ViewMode::TopDown, world::ViewMode::Oblique, world::ViewMode::Iso};
      for (world::ViewMode m : modes)
        if (value == world::view_mode_name(m)) view = m;
      if (!view) return usage();
      continue;
    }
    if (flag == "--height" || flag == "--zjump") {
      for (std::string_view item : pkg::split(value, flag == "--height" ? ',' : ';')) {
        item = pkg::trim(item);
        size_t eq = item.find('=');
        if (eq == std::string_view::npos) return usage();
        const double number = std::strtod(std::string(item.substr(eq + 1)).c_str(), nullptr);
        std::string target(item.substr(0, eq));
        if (flag == "--height") {
          heights.emplace_back(target, number);
          continue;
        }
        size_t colon = target.find(':');
        int64_t tick = 0;
        if (colon == std::string::npos || !parse_int(target.substr(0, colon).c_str(), 1, 10000000, &tick)) return usage();
        zjumps.push_back({tick, target.substr(colon + 1), number});
      }
      continue;
    }
    int64_t* target = flag == "--ticks" ? &ticks : flag == "--seed" ? &seed : flag == "--budget" ? &budget
                    : flag == "--load-budget" ? &load_budget : nullptr;
    if (!target || !parse_int(std::string(value).c_str(), 0, flag == "--seed" ? int64_t{0xffffffff} : 10000000, target)) return usage();
  }
  const bool with_z = view || !heights.empty() || !zjumps.empty();
  auto files = load_files(argv[2]);
  if (!files) {
    std::fprintf(stderr, "✗ %s\n", files.error().c_str());
    return 1;
  }
  pkg::LoadResult loaded = pkg::load_package(std::move(files).value());
  if (!loaded.ok) {
    print_issues(loaded.issues);
    return 1;
  }
  std::shared_ptr<const pkg::Package> package(std::move(loaded.package));
  script::HostOptions opts;
  opts.seed = static_cast<uint32_t>(seed);
  opts.budget_ms = static_cast<double>(budget);
  opts.load_budget_ms = static_cast<double>(load_budget);
  script::Host* host_ptr = nullptr;
  opts.clock = [&host_ptr] { return 1767225600.0 + (host_ptr ? host_ptr->time() : 0.0); };
  opts.on_log = [](const script::LogLine& l) { std::printf("[%s] %s\n", l.level.c_str(), l.text.c_str()); };
  if (view) opts.view = *view;
  auto host = script::Host::create(package, opts);
  if (!host) {
    std::fprintf(stderr, "✗ %s\n", host.error().c_str());
    return 1;
  }
  host_ptr = host.value().get();
  world::World& w = host_ptr->world();
  for (const auto& [id, h] : heights)
    if (world::Entity* e = w.get(w.by_id(id))) e->height = h;
  for (int64_t i = 1; i <= ticks; ++i) {
    apply_input(host_ptr->input(), plan, i);
    for (const ZJump& j : zjumps)
      if (world::Entity* e = j.tick == i ? w.get(w.by_id(j.id)) : nullptr) e->z_velocity = j.vz;
    host_ptr->step();
    if (dump_every > 0 && i % dump_every == 0) dump_entities(*host_ptr, i, dump_selectors, with_z);
    if (hash) std::printf("hash %lld %016llx\n", static_cast<long long>(i), static_cast<unsigned long long>(host_ptr->state_hash()));
  }
  auto num = [](double v) { return pkg::number_text(v); };
  for (const script::ScriptError& e : host_ptr->errors()) {
    std::printf("error %s:%d x%d: %s\n", e.script.c_str(), e.line, e.count, e.message.c_str());
  }
  for (const script::AudioEvent& a : host_ptr->audio_events()) {
    if (a.kind == "sfx") std::printf("audio sfx %s %s %s  %s\n", a.id.c_str(), num(a.volume).c_str(), num(a.pitch).c_str(), num(a.time).c_str());
    else if (a.kind == "music") std::printf("audio music %s   %s %s\n", a.id.c_str(), num(a.fade).c_str(), num(a.time).c_str());
    else std::printf("audio music_stop    %s %s\n", num(a.fade).c_str(), num(a.time).c_str());
  }
  std::printf("save %s\ntime %s\n", host_ptr->save_data().stringify().c_str(), num(host_ptr->time()).c_str());
  if (hash) std::printf("hash final %016llx\n", static_cast<unsigned long long>(host_ptr->state_hash()));
  return 0;
}

// int64_t, not long: long is 32-bit on wasm32 and would overflow for seeds above 2^31.
bool parse_int(const char* text, int64_t min, int64_t max, int64_t* out) {
  char* end = nullptr;
  long long v = std::strtoll(text, &end, 10);
  if (end == text || *end != '\0' || v < min || v > max) return false;
  *out = v;
  return true;
}

int cmd_smoke(int argc, char** argv) {
  int64_t seed = 1, bodies = 20, steps = 300;
  for (int i = 2; i < argc; i += 2) {
    std::string_view flag = argv[i];
    if (i + 1 >= argc) return usage();
    int64_t* target = flag == "--seed" ? &seed : flag == "--bodies" ? &bodies : flag == "--steps" ? &steps : nullptr;
    int64_t max = flag == "--seed" ? int64_t{0xffffffff} : 100000;
    if (!target || !parse_int(argv[i + 1], 0, max, target)) return usage();
  }

  auto lua = dev::run_luau(
      "local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end\n"
      "print(`luau ok: fib(20) = {fib(20)}`)\n");
  if (!lua) {
    log_error("luau: " + lua.error());
    return 1;
  }
  std::fputs(lua.value().c_str(), stdout);

  uint64_t hash = dev::physics_hash(static_cast<uint32_t>(seed), static_cast<int>(bodies), static_cast<int>(steps));
  std::printf("box2d ok: seed=%lld bodies=%lld steps=%lld hash=%016llx\n", static_cast<long long>(seed),
              static_cast<long long>(bodies), static_cast<long long>(steps), static_cast<unsigned long long>(hash));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  script::use_embedded_kits();  // the tool carries every kit
  if (argc < 2) return usage();
  std::string_view cmd = argv[1];
  if (cmd == "version") {
    std::printf("dreamsy engine %s\n", kEngineVersion);
    return 0;
  }
  if (cmd == "validate") return cmd_validate(argc, argv);
  if (cmd == "info") return cmd_info(argc, argv);
  if (cmd == "pack") return cmd_pack(argc, argv);
  if (cmd == "unpack") return cmd_unpack(argc, argv);
  if (cmd == "run") return cmd_run(argc, argv);
  if (cmd == "check") return cmd_check(argc, argv);
  if (cmd == "playtest") return tool::cmd_playtest(argc, argv);
  if (cmd == "eval") return tool::cmd_eval(argc, argv);
  if (cmd == "smoke") return cmd_smoke(argc, argv);
  return usage();
}
