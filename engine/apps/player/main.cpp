// dreamsy-player: the windowed engine (web, macOS, later iOS/Android).
//
// Runs a game: the package's scripts at a fixed 60 Hz (script::Host: world, physics, tweens,
// animation, camera) and draws the world every frame (play::draw_world -> render::Renderer).
// --demo shows the E4 renderer test scene (demo_scene.h) instead.
//
//   dreamsy-player [package-dir] [--view side|topdown|oblique|iso] [--smooth] [--frames N] [--demo] [--touch]
//
// Input (src/input, play/platform_ui.h): arrows/WASD move, Space/Z jump (Up/W too in games that never
// read move_y), E/J/Enter/X action, Q/K/C action2; mouse and multi-touch drive input.pointer*, taps
// on UI and world, dialogs and (on touch devices, or with --touch / #touch) the on-screen controls.
// Esc/P pause. In game mode, keys 1-4 preview the world in another view mode (2.5D projection).
//
// --smooth forces smooth mode (linear filtering) on a pixel-art package. On the web the URL hash sets
// the options as dash-separated words (dreamsy-player.html#iso, #iso-smooth, #demo-iso, #touch).
// Keys: 1-4 pick the view mode, Tab cycles.
//
// Web: the package comes from the query (?public=<id> | ?api=&game=&version=&token= | ?pkg=<url>, see
// embed.js), else the embedded demo at /pkg. Embedded in the app (?embed=1, a React Native WebView, or
// ?public=) the player posts {type:"ready" | "error" | "quit" | "runEnd"} messages to the host (embed.js).
//
// Tools (web, like engine-web dev/main.ts): ?manual=1 stops the real-time loop; the page then steps
// the game itself with window.__dreamsy.step(ticks) (game time, so screenshots are reproducible
// however slow the browser is). ?seed=<n> seeds the RNG, ?clock=<unix seconds> fixes clock.now()
// (plus game time). window.__dreamsy.ready / .error tell the page when the game is running.
// Harness playtests start mid-game with ?scene=<path>&params=<json>&save=<json> (scene params and
// preloaded save data, like engine-web's playtest boot options).
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "base/log.h"
#include "base/version.h"
#include "demo_scene.h"
#include "pkg/catalog.h"
#include "pkg/json.h"
#include "pkg/text.h"
#include "pkg/zip_import.h"
#include "input/usage.h"
#include "play/platform_ui.h"
#include "play/scene_draw.h"
#include "script/host.h"
#include "pkg/validate.h"
#include "render/atlas.h"
#include "render/font.h"
#include "render/renderer.h"
#include "render/style.h"
#include "render/textures.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/em_js.h>
#define EMSCRIPTEN_KEEPALIVE_IF_WEB EMSCRIPTEN_KEEPALIVE  // exported to JavaScript
#else
#define EMSCRIPTEN_KEEPALIVE_IF_WEB
#endif

namespace {

using namespace dreamsy;

#ifndef DREAMSY_DEMO_PACKAGE
#define DREAMSY_DEMO_PACKAGE "/pkg"
#endif
#ifndef DREAMSY_KIT_PACKS_DIR
#define DREAMSY_KIT_PACKS_DIR "/kits"  // web: fetched with the package once games load by URL (E6)
#endif

using Bytes = pkg::Files::Bytes;

// Reads a whole file (heap buffer: the web build's stack is small).
std::shared_ptr<Bytes> read_file(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return nullptr;
  auto bytes = std::make_shared<Bytes>();
  std::vector<char> buf(64 * 1024);
  for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;) bytes->insert(bytes->end(), buf.begin(), buf.begin() + static_cast<long>(n));
  std::fclose(f);
  return bytes;
}

// Kit pack bytes by kit name: from DREAMSY_KIT_PACKS_DIR natively, or fetched over HTTP on the web.
using KitBytes = std::function<std::shared_ptr<Bytes>(const std::string& kit)>;

std::shared_ptr<Bytes> kit_from_disk(const std::string& kit) {
  return read_file(std::string(DREAMSY_KIT_PACKS_DIR) + "/" + kit + ".dpk");
}

std::vector<std::string> kits_of(const pkg::Manifest& m) {
  return pkg::Catalog::get().expand_kits(m.kits.value_or(std::vector<std::string>{}));
}

// The kits a game's scripts require (require("@kit/<name>...)), with their dependencies. Only these
// count for the input-usage scan, like engine-web's detectInputUsage.
std::vector<std::string> required_kits(std::string_view scripts) {
  std::vector<std::string> names;
  for (size_t at = scripts.find("require"); at != std::string_view::npos; at = scripts.find("require", at + 1)) {
    size_t i = at + 7;
    while (i < scripts.size() && (scripts[i] == ' ' || scripts[i] == '(')) i++;
    if (i >= scripts.size() || (scripts[i] != '"' && scripts[i] != '\'')) continue;
    if (scripts.substr(i + 1, 5) != "@kit/") continue;
    size_t end = i + 6;
    while (end < scripts.size() && ((scripts[end] >= 'a' && scripts[end] <= 'z') || (scripts[end] >= '0' && scripts[end] <= '9') ||
                                    scripts[end] == '-')) {
      end++;
    }
    const std::string name(scripts.substr(i + 6, end - i - 6));
    if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  }
  return names.empty() ? names : pkg::Catalog::get().expand_kits(names);
}

// The kits a game enables (and their dependencies), each a <kit>.dpk. Kit code isn't in the
// player binary (script/kits.h); only what a game uses is loaded.
// `kit_sources` receives the scripts of the kits in `scanned`, for the input-usage scan (input/usage.h).
Result<script::KitLookup> load_kits(const pkg::Manifest& m, const KitBytes& kit_bytes,
                                    const std::vector<std::string>& scanned, std::string& kit_sources) {
  std::vector<std::shared_ptr<const pkg::Files>> packs;
  for (const std::string& kit : kits_of(m)) {
    std::shared_ptr<Bytes> bytes = kit_bytes(kit);
    if (!bytes) return Error{"kit \"" + kit + "\" is not available"};
    auto files = pkg::Files::from_pack(bytes);
    if (!files) return Error{"kit \"" + kit + "\": " + files.error()};
    if (std::find(scanned.begin(), scanned.end(), kit) != scanned.end()) {
      for (const std::string& file : files.value().paths("kits/")) {
        if (auto text = files.value().text(file)) kit_sources.append(*text);
      }
    }
    packs.push_back(std::make_shared<const pkg::Files>(std::move(files).value()));
  }
  return script::kits_from_packs(std::move(packs));
}

struct Options {
  std::string package_dir = DREAMSY_DEMO_PACKAGE;  // a directory, a .dpk or a .zip
  std::string package_source;                       // web: the package downloaded from the query (embed.js)
  std::string kits_url = "kits/";                   // web: where <kit>.dpk files are (?kits=<url>)
  // Forces a view mode (dev: --view / #iso / keys 1-4). Unset = the scene's own view (manifest
  // or scene `view`, docs/engine.md §14.5).
  std::optional<render::ViewMode> view;
  int frames = 0;  // quit after this many frames (0 = run until closed)
  bool smooth = false;  // force smooth mode (linear filtering, no pixel snap) to check it on any package
  bool demo = false;    // renderer test scene instead of running the game
  bool touch = false;   // force the on-screen controls (they show by default on touch devices only)
  bool manual = false;  // tools: no real-time ticks; window.__dreamsy.step(n) advances the game
  bool embed = false;   // web: inside the app (or a share link); messages go to the host (embed.js)
  ui::SafeArea insets;  // web: the host's safe-area insets, CSS px (?insets=)
  uint32_t seed = 1;
  std::optional<double> clock;  // fixed clock.now() base (unix seconds); default: the wall clock
  // Tools: start in this scene with these params / this save data (JSON text; empty = none).
  std::string start_scene, start_params, start_save;
};

std::optional<render::ViewMode> parse_view(const char* s) {
  if (std::strcmp(s, "side") == 0) return render::ViewMode::Side;
  if (std::strcmp(s, "topdown") == 0) return render::ViewMode::TopDown;
  if (std::strcmp(s, "oblique") == 0) return render::ViewMode::Oblique;
  if (std::strcmp(s, "iso") == 0) return render::ViewMode::Iso;
  return std::nullopt;
}

// Everything the player owns. One instance, created in sokol_main, freed in cleanup.
struct App {
  Options options;
  std::shared_ptr<const pkg::Package> package;
  std::unique_ptr<script::Host> host;  // the running game (null in --demo)
  std::unique_ptr<play::PlatformUi> ui;  // input routing, UI entities, dialogs, controls (with host)
  double accumulator = 0;              // unsimulated time, seconds
  uint64_t last_ticks = 0;
  render::RenderStyle style;
  std::unique_ptr<render::Atlas> atlas;
  std::unique_ptr<render::TextureStore> textures;
  std::unique_ptr<render::FontStore> fonts;
  std::unique_ptr<player::DemoScene> scene;
  render::Renderer renderer;
  render::DrawList draw_list;
  render::ViewMode view = render::ViewMode::Side;
  uint64_t start_ticks = 0;
  int frame_count = 0;
  bool ok = false;
  bool loading = false;  // web: waiting for ?pkg= / kit downloads
#ifdef __EMSCRIPTEN__
  std::shared_ptr<Bytes> package_bytes;
  std::unique_ptr<pkg::Package> pending_package;
  std::map<std::string, std::shared_ptr<Bytes>> fetched_kits;
  size_t kits_pending = 0;
#endif
  double manual_elapsed = 0;  // manual mode: game time stepped since the last frame
  bool error_published = false;
  std::string load_error;  // the first reason the game didn't load (shown to the host app)
  // Host messages (embed): script errors reported so far, error and run-end rate limits (wall clock, s).
  size_t errors_reported = 0;
  std::string last_error;
  double last_error_at = -1e9;
  std::optional<std::string> queued_error;
  double last_run_end_at = -1e9;
  // Web audio (audio.js): events already sent, and the last volumes sent.
  const script::Host* audio_host = nullptr;
  size_t audio_sent = 0;
  double audio_music = -1, audio_sfx = -1;
  bool audio_paused = false;
};

std::unique_ptr<App> g_app;

// A reason the game can't start: logged, and the first one is what the host app is told.
void load_failed(App& app, const std::string& message) {
  log_error(message);
  if (app.load_error.empty()) app.load_error = message;
}

#ifdef __EMSCRIPTEN__
EM_JS(int, dreamsy_js_has_source, (), { return dreamsyHost.download ? 1 : 0; });
EM_JS(void, dreamsy_js_deliver, (), { dreamsyHost.deliver(); });
EM_JS(void, dreamsy_js_post, (const char* json), { dreamsyHost.post(UTF8ToString(json)); });
EM_JS(void, dreamsy_js_quit, (), { dreamsyHost.quit(); });
EM_JS(void, dreamsy_js_audio, (const char* kind, const char* id, const char* json, double volume, double pitch, double fade), {
  dreamsyAudio.event(UTF8ToString(kind), UTF8ToString(id), json ? UTF8ToString(json) : "", volume, pitch, fade);
});
EM_JS(void, dreamsy_js_audio_state, (double music, double sfx, int paused), { dreamsyAudio.state(music, sfx, paused); });
#endif

// A message to the host app (embed mode only; embed.js lists them).
void post_to_host(const App& app, const script::Json& msg) {
#ifdef __EMSCRIPTEN__
  if (app.options.embed) dreamsy_js_post(msg.stringify().c_str());
#else
  (void)app;
  (void)msg;
#endif
}

double wall_seconds() { return stm_sec(stm_now()); }

// {type:"error"} to the host: at most one every 2 s (the latest waits, see flush_error), the same
// message at most every 30 s (engine-web dev/main.ts reportError).
void report_error(App& app, std::string message) {
  if (!app.options.embed) return;
  if (message.size() > 500) {
    message.resize(500);
    while (!message.empty() && (static_cast<unsigned char>(message.back()) & 0xC0) == 0x80) message.pop_back();
    if (!message.empty() && (static_cast<unsigned char>(message.back()) & 0x80) != 0) message.pop_back();
  }
  const double now = wall_seconds();
  if (message == app.last_error && now - app.last_error_at < 30) return;
  if (now - app.last_error_at < 2) {
    app.queued_error = message;
    return;
  }
  app.last_error_at = now;
  app.last_error = message;
  app.queued_error.reset();
  script::Json m = script::Json::object_value();
  m.set("type", script::Json::string_value("error"));
  m.set("message", script::Json::string_value(message));
  post_to_host(app, m);
}

// Called every frame: sends a held-back error once the gap has passed, and new script errors.
void flush_errors(App& app) {
  if (!app.options.embed) return;
  if (app.host && app.host->errors().size() > app.errors_reported) {
    app.errors_reported = app.host->errors().size();
    const script::ScriptError& e = app.host->errors().back();
    report_error(app, e.script + (e.line ? ":" + std::to_string(e.line) : "") + ": " + e.message);
  }
  if (app.queued_error && wall_seconds() - app.last_error_at >= 2) {
    std::string m = *app.queued_error;
    app.queued_error.reset();
    report_error(app, m);
  }
}

// Sends new audio events and volume changes to audio.js. Tool runs (?manual=1) stay silent.
void flush_audio(App& app) {
#ifdef __EMSCRIPTEN__
  if (!app.host || app.options.manual) return;
  const script::Host& host = *app.host;
  if (app.audio_host != &host) {
    app.audio_host = &host;
    app.audio_sent = 0;
  }
  const double music = host.settings.mute ? 0 : host.settings.music;
  const double sfx = host.settings.mute ? 0 : host.settings.sfx;
  if (music != app.audio_music || sfx != app.audio_sfx || host.paused() != app.audio_paused) {
    app.audio_music = music;
    app.audio_sfx = sfx;
    app.audio_paused = host.paused();
    dreamsy_js_audio_state(music, sfx, host.paused() ? 1 : 0);
  }
  const std::vector<script::AudioEvent>& events = host.audio_events();
  for (; app.audio_sent < events.size(); app.audio_sent++) {
    const script::AudioEvent& e = events[app.audio_sent];
    std::string json;
    if (e.kind == "sfx" || e.kind == "music") {
      const std::string path = (e.kind == "sfx" ? "data/sfx/" : "data/music/") + e.id + ".json";
      if (const std::optional<std::string_view> text = app.package->files.text(path)) json = std::string(*text);
    }
    dreamsy_js_audio(e.kind.c_str(), e.id.c_str(), json.empty() ? nullptr : json.c_str(), e.volume, e.pitch, e.fade);
  }
#else
  (void)app;
#endif
}

// {type:"runEnd", reason, board?, value?}: at most one every 3 s (engine-web RUN_END_GAP_MS).
void report_run_end(App& app, const script::RunEnd& r) {
  if (!app.options.embed) return;
  const double now = wall_seconds();
  if (now - app.last_run_end_at < 3) return;
  app.last_run_end_at = now;
  script::Json m = script::Json::object_value();
  m.set("type", script::Json::string_value("runEnd"));
  m.set("reason", script::Json::string_value(r.reason));
  if (r.board) m.set("board", script::Json::string_value(*r.board));
  if (r.value && std::isfinite(*r.value)) m.set("value", script::Json::number_value(*r.value));
  post_to_host(app, m);
}

}  // namespace

// Manual mode (tools): runs `ticks` fixed ticks now (none while paused) and returns how many ran.
// Called from JavaScript as window.__dreamsy.step(n); the next frame draws the result.
extern "C" EMSCRIPTEN_KEEPALIVE_IF_WEB int dreamsy_step(int ticks) {
  if (!g_app || !g_app->host) return 0;
  int ran = 0;
  for (; ran < ticks && !g_app->host->paused(); ran++) g_app->host->step();
  g_app->manual_elapsed += ran / 60.0;
  return ran;
}

// Script errors so far (distinct errors; tools fail a run that has any).
extern "C" EMSCRIPTEN_KEEPALIVE_IF_WEB int dreamsy_error_count() {
  return g_app && g_app->host ? static_cast<int>(g_app->host->errors().size()) : 0;
}

namespace {

// A package from a directory, or from the bytes of a .zip or a Dreamsy pack (told apart by the zip
// signature, so downloads need no extension).
Result<pkg::Files> open_package(const std::string& name, std::shared_ptr<Bytes> bytes) {
  if (!bytes) return pkg::Files::from_dir(name);
  if (bytes->size() >= 4 && (*bytes)[0] == 'P' && (*bytes)[1] == 'K' && ((*bytes)[2] == 3 || (*bytes)[2] == 5)) {
    return pkg::import_zip(*bytes);
  }
  return pkg::Files::from_pack(std::move(bytes));
}

// The pause menu's Quit and settings persistence, per platform. Web: settings live in localStorage
// under engine-web's key (both engines share them); Quit tells the host app (React Native WebView or
// a parent frame) like engine-web's embed mode, else goes back. Native: Quit closes the window.
void hook_platform(play::PlatformUi& ui, bool tools) {
#ifdef __EMSCRIPTEN__
  if (!tools) {  // tool runs (?manual=1) start from the defaults, like engine-web's ?save=0
    const std::string saved = emscripten_run_script_string(
        "(() => { try { const s = JSON.parse(localStorage.getItem('dreamsy.settings.v1') || 'null');"
        " return s ? [+s.music, +s.sfx, s.mute ? 1 : 0, s.vibration ? 1 : 0, +(s.touchSize || 1)].join(' ') : ''; }"
        " catch (e) { return ''; } })()");
    ui::ShellSettings s;
    int mute = 0, vibration = 1;
    if (std::sscanf(saved.c_str(), "%lf %lf %d %d %f", &s.music, &s.sfx, &mute, &vibration, &s.touch_size) == 5) {
      s.mute = mute != 0;
      s.vibration = vibration != 0;
      ui.set_settings(s);
    }
  }
  ui.on_settings = [](const ui::ShellSettings& s) {
    char script[256];
    std::snprintf(script, sizeof script,
                  "try { localStorage.setItem('dreamsy.settings.v1', JSON.stringify({music: %g, sfx: %g, mute: %s,"
                  " vibration: %s, touchSize: %g})); } catch (e) {}",
                  s.music, s.sfx, s.mute ? "true" : "false", s.vibration ? "true" : "false", static_cast<double>(s.touch_size));
    emscripten_run_script(script);
  };
  ui.on_quit = [] { dreamsy_js_quit(); };  // embed: {type:"quit"} to the host app, else back
#else
  (void)tools;
  ui.on_quit = [] { sapp_request_quit(); };
#endif
}

std::unique_ptr<pkg::Package> validate_package(App& app, pkg::Files files) {
  pkg::LoadResult loaded = pkg::load_package(std::move(files));
  for (const pkg::Issue& issue : loaded.issues) {
    if (issue.severity == pkg::Severity::Error) load_failed(app, "invalid package: " + pkg::format_issue(issue));
  }
  if (!loaded.ok) load_failed(app, "invalid package");
  return std::move(loaded.package);
}

// Starts the validated package as a game (or the --demo scene).
bool start(App& app, std::unique_ptr<pkg::Package> package, const KitBytes& kit_bytes) {
  app.package = std::move(package);
  app.style = render::style_from_manifest(app.package->manifest);
  if (app.options.smooth) {
    app.style.nearest = false;
    app.style.pixel_snap = false;
  }
  app.atlas = std::make_unique<render::Atlas>();
  app.textures = std::make_unique<render::TextureStore>(app.package.get(), *app.atlas, app.style.nearest);
  app.fonts = std::make_unique<render::FontStore>(*app.atlas, app.style.theme_font);
  if (Status s = app.fonts->init(); !s) {
    load_failed(app, s.error());
    return false;
  }
  if (app.options.demo) {
    app.scene = std::make_unique<player::DemoScene>(*app.package, *app.textures, app.style);
    return true;
  }
  script::HostOptions host_options;
  std::string game_scripts;
  for (const std::string& path : app.package->files.paths("scripts/")) {
    if (auto text = app.package->files.text(path)) game_scripts.append(*text);
  }
  std::string kit_sources;
  auto kits = load_kits(app.package->manifest, kit_bytes, required_kits(game_scripts), kit_sources);
  if (!kits) {
    load_failed(app, kits.error());
    return false;
  }
  host_options.kits = std::move(kits).value();
  host_options.seed = app.options.seed;
  if (!app.options.start_scene.empty()) host_options.scene = app.options.start_scene;
  for (auto [text, what] : {std::pair{&app.options.start_params, "params"}, std::pair{&app.options.start_save, "save"}}) {
    if (text->empty()) continue;
    auto doc = pkg::JsonDoc::parse(*text);
    if (!doc || !yyjson_is_obj(doc.value().root())) {
      log_error(std::string("?") + what + "= must be a JSON object");
      return false;
    }
    (what == std::string_view("save") ? host_options.save : host_options.scene_params.emplace()) = script::Json::from_yyjson(doc.value().root());
  }
  if (app.options.clock) {
    // engine-web: fixed base + game time, so timers that read clock.now() still advance.
    const double base = *app.options.clock;
    host_options.clock = [base] { return base + (g_app && g_app->host ? g_app->host->time() : 0.0); };
  }
  host_options.on_log = [](const script::LogLine& l) {
    (l.level == "error" ? log_error : l.level == "warn" ? log_warn : log_info)("[game] " + l.text);
  };
  host_options.on_run_end = [](const script::RunEnd& r) {
    if (g_app) report_run_end(*g_app, r);
  };
  auto host = script::Host::create(app.package, host_options);
  if (!host) {
    load_failed(app, "game failed to start: " + host.error());
    return false;
  }
  app.host = std::move(host).value();

  // Which controls the game reads (engine-web detectInputUsage), and whether Up/W also jump.
  const input::InputUsage usage = input::detect_input_usage(game_scripts + kit_sources);
  bool touch = app.options.touch;
#ifdef __EMSCRIPTEN__
  touch = touch || emscripten_run_script_int("('ontouchstart' in window || navigator.maxTouchPoints > 0) ? 1 : 0") != 0;
#elif defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  touch = true;
#endif
  app.ui = std::make_unique<play::PlatformUi>(*app.host, *app.textures, *app.fonts, usage,
                                              !input::scripts_mention_move_y(game_scripts), touch);
  if (Status s = app.ui->init(); !s) {
    load_failed(app, s.error());
    return false;
  }
  hook_platform(*app.ui, app.options.manual);
  return true;
}

// Native/embedded: the package is a local path.
bool load(App& app) {
  const std::string& name = app.options.package_dir;
  std::shared_ptr<Bytes> bytes;
  if (pkg::ends_with(name, ".dpk") || pkg::ends_with(name, ".zip")) {
    bytes = read_file(name);
    if (!bytes) {
      load_failed(app, "can't read " + name);
      return false;
    }
  }
  auto files = open_package(name, std::move(bytes));
  if (!files) {
    load_failed(app, "can't open package " + name + ": " + files.error());
    return false;
  }
  auto package = validate_package(app, std::move(files).value());
  return package && start(app, std::move(package), kit_from_disk);
}

void set_view(App& app, render::ViewMode v);
void finish_init(App& app);

#ifdef __EMSCRIPTEN__
// Web: embed.js downloads the package named in the query (it starts before the WASM is ready) and
// hands it over with dreamsy_package_buffer + dreamsy_package_ready. Fetch the enabled kits in
// parallel, then load them in manifest dependency order in load_kits.
struct Download {
  std::string kit;
};

void finish_download(App& app) {
  app.loading = false;
  const auto fetched_kits = std::move(app.fetched_kits);
  auto kit_bytes = [&fetched_kits](const std::string& kit) -> std::shared_ptr<Bytes> {
    auto it = fetched_kits.find(kit);
    return it == fetched_kits.end() ? nullptr : it->second;
  };
  if (start(app, std::move(app.pending_package), kit_bytes)) finish_init(app);
}

void on_kit_error(void* arg) {
  std::unique_ptr<Download> d(static_cast<Download*>(arg));
  if (!g_app || !g_app->loading) return;
  App& app = *g_app;
  load_failed(app, "can't download kit \"" + d->kit + "\"");
  app.loading = false;
  app.pending_package.reset();
  app.fetched_kits.clear();
}

void on_kit(void* arg, void* data, int size) {
  std::unique_ptr<Download> d(static_cast<Download*>(arg));
  if (!g_app || !g_app->loading) return;
  App& app = *g_app;
  const auto* bytes = static_cast<const uint8_t*>(data);
  app.fetched_kits.emplace(std::move(d->kit), std::make_shared<Bytes>(bytes, bytes + size));
  if (--app.kits_pending == 0) finish_download(app);
}
#endif

}  // namespace

#ifdef __EMSCRIPTEN__
// embed.js: room for the downloaded package (`size` bytes), filled from JavaScript.
extern "C" EMSCRIPTEN_KEEPALIVE uint8_t* dreamsy_package_buffer(int size) {
  g_app->package_bytes = std::make_shared<Bytes>(static_cast<size_t>(std::max(size, 0)));
  return g_app->package_bytes->data();
}

// embed.js: the package is in the buffer. Keep the validated package while kits load.
extern "C" EMSCRIPTEN_KEEPALIVE void dreamsy_package_ready() {
  App& app = *g_app;
  auto files = open_package(app.options.package_source, std::move(app.package_bytes));
  if (!files) {
    load_failed(app, "can't open " + app.options.package_source + ": " + files.error());
    app.loading = false;
    return;
  }
  app.pending_package = validate_package(app, std::move(files).value());
  if (!app.pending_package) {
    app.loading = false;
    return;
  }
  const auto kits = kits_of(app.pending_package->manifest);
  if (kits.empty()) {
    finish_download(app);
    return;
  }
  app.kits_pending = kits.size();
  for (const auto& kit : kits) {
    const std::string url = app.options.kits_url + kit + ".dpk";
    auto download = std::make_unique<Download>(Download{kit});
    emscripten_async_wget_data(url.c_str(), download.release(), on_kit, on_kit_error);
  }
}

// embed.js: the download failed (dreamsyHost.error says why).
extern "C" EMSCRIPTEN_KEEPALIVE void dreamsy_package_failed() {
  load_failed(*g_app, emscripten_run_script_string("dreamsyHost.error"));
  g_app->loading = false;
}
#endif

namespace {

void init() {
  App& app = *g_app;
  sg_desc gfx{};
  gfx.environment = sglue_environment();
  gfx.logger.func = slog_func;
  sg_setup(&gfx);
  stm_setup();
  app.start_ticks = stm_now();
  log_info(std::string("dreamsy player ") + kEngineVersion + " (" +
           (sg_query_backend() == SG_BACKEND_GLES3 ? "webgl2/gles3" : "metal") + ")");

#ifdef __EMSCRIPTEN__
  // dreamsy-player.html#<words>: a view mode, "smooth", "demo", "touch", separated by dashes.
  const std::string hash = emscripten_run_script_string("window.location.hash.slice(1)");
  for (size_t start = 0; start <= hash.size();) {
    size_t end = hash.find('-', start);
    if (end == std::string::npos) end = hash.size();
    const std::string word = hash.substr(start, end - start);
    if (word == "smooth") app.options.smooth = true;
    else if (word == "demo") app.options.demo = true;
    else if (word == "touch") app.options.touch = true;
    else if (auto v = parse_view(word.c_str())) app.options.view = *v;
    start = end + 1;
  }
#endif
  app.view = app.options.view.value_or(render::ViewMode::Side);

#ifdef __EMSCRIPTEN__
  // Tool options in the query (engine-web names): ?manual=1&seed=1&clock=1767225600.
  app.options.manual = emscripten_run_script_int("new URLSearchParams(location.search).get('manual') === '1' ? 1 : 0") != 0;
  app.options.seed = static_cast<uint32_t>(emscripten_run_script_int("Number(new URLSearchParams(location.search).get('seed') || 1) | 0"));
  const std::string clock = emscripten_run_script_string("new URLSearchParams(location.search).get('clock') || ''");
  if (!clock.empty()) app.options.clock = std::strtod(clock.c_str(), nullptr);
  app.options.start_scene = emscripten_run_script_string("new URLSearchParams(location.search).get('scene') || ''");
  app.options.start_params = emscripten_run_script_string("new URLSearchParams(location.search).get('params') || ''");
  app.options.start_save = emscripten_run_script_string("new URLSearchParams(location.search).get('save') || ''");
#endif

#ifdef __EMSCRIPTEN__
  // The embedding contract (embed.js): package source, embed mode, the host's safe-area insets.
  app.options.embed = emscripten_run_script_int("dreamsyHost.embed ? 1 : 0") != 0;
  std::sscanf(emscripten_run_script_string("dreamsyHost.insets.join(' ')"), "%lf %lf %lf %lf", &app.options.insets.top,
              &app.options.insets.right, &app.options.insets.bottom, &app.options.insets.left);
  const std::string kits = emscripten_run_script_string("new URLSearchParams(location.search).get('kits') || ''");
  if (!kits.empty()) app.options.kits_url = kits.back() == '/' ? kits : kits + "/";
  if (dreamsy_js_has_source()) {
    app.options.package_source = emscripten_run_script_string("dreamsyHost.source");
    app.loading = true;
    log_info("downloading " + app.options.package_source);
    dreamsy_js_deliver();
    return;
  }
  if (app.options.embed) {  // the app always names a game; the demo is for development
    load_failed(app, "no game to load (missing ?public= or ?api=&game=)");
    return;
  }
#endif
  if (!load(app)) return;
  finish_init(app);
}

#ifdef __EMSCRIPTEN__
// window.__dreamsy for tools: { ready, error, step(ticks) } (engine-web dev/main.ts has the same).
void publish_tool_state(const std::string& error) {
  // `error` is fixed engine text (no quotes), so it can go straight into the script.
  const std::string fields = error.empty()
      ? "{ ready: true, step: (n) => _dreamsy_step(n), errors: () => _dreamsy_error_count() }"
      : "{ error: '" + error + "' }";
  emscripten_run_script(("window.__dreamsy = Object.assign(window.__dreamsy || {}, " + fields + ");").c_str());
}
#endif

// After the package is loaded: renderer, first frame timing, view mode.
void finish_init(App& app) {
  if (Status s = app.renderer.init(app.style); !s) {
    log_error(s.error());
    return;
  }
  app.ok = true;
  app.last_ticks = stm_now();
  if (app.host && app.options.view) set_view(app, *app.options.view);
  if (app.host && !app.options.view) app.view = static_cast<render::ViewMode>(static_cast<int>(app.host->world().view));
#ifdef __EMSCRIPTEN__
  publish_tool_state("");
#endif
  if (app.host) {
    script::Json ready = script::Json::object_value();
    ready.set("type", script::Json::string_value("ready"));
    post_to_host(app, ready);
  }
  log_info("package \"" + app.package->manifest.name + "\": reference " + std::to_string(app.style.ref_w) + "x" +
           std::to_string(app.style.ref_h) + (app.style.pixel_snap ? ", pixel-art mode" : ", smooth mode"));
}

void frame() {
  App& app = *g_app;
  if (!app.ok) {
#ifdef __EMSCRIPTEN__
    if (!app.loading && !app.error_published) {
      app.error_published = true;
      publish_tool_state("the game failed to load (see the console)");
      report_error(app, app.load_error.empty() ? "the game failed to load" : app.load_error);
    }
#endif
    // Still downloading: dark grey. Nothing loaded: dark red, so the failure is visible.
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = app.loading ? sg_color{0.08f, 0.08f, 0.1f, 1} : sg_color{0.3f, 0.05f, 0.08f, 1};
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    sg_end_pass();
    sg_commit();
    return;
  }
  if (app.host) {
    // Layout points per reference px: the game rect's scale over the display's pixel density
    // (on the web, CSS px per game px). Safe-area insets: the host's (?insets=) on the web;
    // native platforms: TODO(E9).
    const float W = static_cast<float>(sapp_width()), H = static_cast<float>(sapp_height());
    const render::Fit fit = render::fit_rect(app.style.ref_w, app.style.ref_h, W, H);
    const float dpi = std::max(0.5f, sapp_dpi_scale());
    const ui::SafeArea& in = app.options.insets;  // CSS px
    const double it = in.top * dpi, ir = in.right * dpi, ib = in.bottom * dpi, il = in.left * dpi;  // framebuffer px
    // The window inside the insets, in reference px: the pause button, menu and touch controls use it.
    const ui::Rect window{(il - fit.x) / fit.s, (it - fit.y) / fit.s, (W - il - ir) / fit.s, (H - it - ib) / fit.s};
    // The game's UI anchors: only the part of an inset that reaches into the game rect (past the letterbox bars).
    const ui::SafeArea safe{std::max(0.0, it - fit.y) / fit.s, std::max(0.0, ir - (W - fit.x - fit.w)) / fit.s,
                            std::max(0.0, ib - (H - fit.y - fit.h)) / fit.s, std::max(0.0, il - fit.x) / fit.s};
    app.ui->set_screen(fit.s / dpi, safe, window);
    // Fixed 60 Hz simulation, at most 5 ticks per frame (engine-web Game.advance). Paused: no ticks.
    constexpr double kDt = 1.0 / 60.0;
    double elapsed = stm_sec(stm_laptime(&app.last_ticks));
    if (app.options.manual) {
      // Tools step the game (dreamsy_step); the UI animates in game time too.
      elapsed = app.manual_elapsed;
      app.manual_elapsed = 0;
      app.accumulator = 0;
    } else if (app.host->paused()) {
      app.accumulator = 0;
    } else {
      app.accumulator = std::min(app.accumulator + elapsed, kDt * 5);
    }
    while (app.accumulator >= kDt - 1e-9) {
      app.accumulator -= kDt;
      app.host->step();
    }
    app.ui->update(static_cast<float>(elapsed));
    flush_errors(app);
    flush_audio(app);
    play::draw_world(*app.host, *app.textures, app.draw_list);
    app.ui->draw(app.draw_list);
  } else {
    const double t = stm_sec(stm_since(app.start_ticks));
    app.scene->build(app.draw_list, app.view, t, app.renderer.stats());
  }
  app.renderer.draw(app.draw_list, *app.textures, *app.fonts, sglue_swapchain(), sapp_width(), sapp_height());
  sg_commit();

  app.frame_count++;
  if (app.options.frames > 0 && app.frame_count >= app.options.frames) {
    const render::FrameStats& s = app.renderer.stats();
    if (app.host) log_info("game time " + std::to_string(app.host->time()) + "s, " + std::to_string(app.host->world().count()) +
                           " entities, " + std::to_string(app.host->errors().size()) + " script errors");
    log_info("frame " + std::to_string(app.frame_count) + " (" + player::view_mode_name(app.view) +
             "): " + std::to_string(s.draw_calls) + " draw calls, " + std::to_string(s.quads) + " quads, " +
             std::to_string(s.culled) + " culled, " + std::to_string(s.atlas_pages) + " atlas pages, " +
             std::to_string(app.fonts->cached_glyphs()) + " glyphs, framebuffer " + std::to_string(sapp_width()) +
             "x" + std::to_string(sapp_height()));
    sapp_request_quit();
  }
}

void cleanup() {
  g_app->renderer.shutdown();
  sg_shutdown();
  g_app.reset();
}

// sokol key codes -> the keys the engine maps (input/keyboard.h).
input::Key key_of(sapp_keycode k) {
  switch (k) {
    case SAPP_KEYCODE_LEFT: return input::Key::Left;
    case SAPP_KEYCODE_RIGHT: return input::Key::Right;
    case SAPP_KEYCODE_UP: return input::Key::Up;
    case SAPP_KEYCODE_DOWN: return input::Key::Down;
    case SAPP_KEYCODE_A: return input::Key::A;
    case SAPP_KEYCODE_C: return input::Key::C;
    case SAPP_KEYCODE_D: return input::Key::D;
    case SAPP_KEYCODE_E: return input::Key::E;
    case SAPP_KEYCODE_J: return input::Key::J;
    case SAPP_KEYCODE_K: return input::Key::K;
    case SAPP_KEYCODE_Q: return input::Key::Q;
    case SAPP_KEYCODE_S: return input::Key::S;
    case SAPP_KEYCODE_W: return input::Key::W;
    case SAPP_KEYCODE_X: return input::Key::X;
    case SAPP_KEYCODE_Z: return input::Key::Z;
    case SAPP_KEYCODE_SPACE: return input::Key::Space;
    case SAPP_KEYCODE_ENTER: case SAPP_KEYCODE_KP_ENTER: return input::Key::Enter;
    case SAPP_KEYCODE_ESCAPE: return input::Key::Escape;
    case SAPP_KEYCODE_P: return input::Key::P;
    default: return input::Key::Other;
  }
}

// Window events -> the platform UI (keys, mouse, touches, focus), positions in reference px.
void game_event(App& app, const sapp_event* e) {
  if (!app.ui) return;
  play::PlatformUi& ui = *app.ui;
  auto ref = [&](float x, float y) {
    const render::Vec2 p = render::framebuffer_to_ref(app.renderer.fit(), {x, y});
    return input::Vec2{p.x, p.y};
  };
  switch (e->type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
      if (!e->key_repeat) ui.key(key_of(e->key_code), true);
      break;
    case SAPP_EVENTTYPE_KEY_UP: ui.key(key_of(e->key_code), false); break;
    case SAPP_EVENTTYPE_UNFOCUSED: ui.blur(); break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
      if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) ui.pointer(play::PointerPhase::Down, play::kMousePointerId, ref(e->mouse_x, e->mouse_y));
      break;
    case SAPP_EVENTTYPE_MOUSE_MOVE: ui.pointer(play::PointerPhase::Move, play::kMousePointerId, ref(e->mouse_x, e->mouse_y)); break;
    case SAPP_EVENTTYPE_MOUSE_UP:
      if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) ui.pointer(play::PointerPhase::Up, play::kMousePointerId, ref(e->mouse_x, e->mouse_y));
      break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL: ui.wheel(ref(e->mouse_x, e->mouse_y), -e->scroll_y * 40); break;
    case SAPP_EVENTTYPE_TOUCHES_BEGAN:
    case SAPP_EVENTTYPE_TOUCHES_MOVED:
    case SAPP_EVENTTYPE_TOUCHES_ENDED:
    case SAPP_EVENTTYPE_TOUCHES_CANCELLED: {
      const play::PointerPhase phase = e->type == SAPP_EVENTTYPE_TOUCHES_BEGAN   ? play::PointerPhase::Down
                                       : e->type == SAPP_EVENTTYPE_TOUCHES_MOVED ? play::PointerPhase::Move
                                       : e->type == SAPP_EVENTTYPE_TOUCHES_ENDED ? play::PointerPhase::Up
                                                                                 : play::PointerPhase::Cancel;
      for (int i = 0; i < e->num_touches; i++) {
        const sapp_touchpoint& t = e->touches[i];
        if (!t.changed) continue;
        ui.pointer(phase, static_cast<int64_t>(t.identifier), ref(t.pos_x, t.pos_y));
      }
      break;
    }
    default: break;
  }
}

void set_view(App& app, render::ViewMode v) {
  app.view = v;
  if (app.host) app.host->world().view = static_cast<world::ViewMode>(static_cast<int>(v));
}

void event(const sapp_event* e) {
  App& app = *g_app;
  game_event(app, e);
  if (e->type != SAPP_EVENTTYPE_KEY_DOWN) return;
  switch (e->key_code) {
    case SAPP_KEYCODE_ESCAPE:
      if (!app.host) sapp_request_quit();  // the demo quits; games pause (platform UI)
      break;
    case SAPP_KEYCODE_1: set_view(app, render::ViewMode::Side); break;
    case SAPP_KEYCODE_2: set_view(app, render::ViewMode::TopDown); break;
    case SAPP_KEYCODE_3: set_view(app, render::ViewMode::Oblique); break;
    case SAPP_KEYCODE_4: set_view(app, render::ViewMode::Iso); break;
    case SAPP_KEYCODE_TAB: set_view(app, static_cast<render::ViewMode>((static_cast<int>(app.view) + 1) % 4)); break;
    default: break;
  }
}

}  // namespace

sapp_desc sokol_main(int argc, char** argv) {
  g_app = std::make_unique<App>();
  Options& o = g_app->options;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--view" && i + 1 < argc) {
      if (auto v = parse_view(argv[++i])) {
        o.view = *v;
      } else {
        log_warn(std::string("unknown view mode: ") + argv[i]);
      }
    } else if (arg == "--touch") {
      o.touch = true;
    } else if (arg == "--demo") {
      o.demo = true;
    } else if (arg == "--smooth") {
      o.smooth = true;
    } else if (arg == "--frames" && i + 1 < argc) {
      o.frames = std::atoi(argv[++i]);
    } else if (!arg.empty() && arg[0] != '-') {
      o.package_dir = arg;
    }
  }

  // Window shape from the package's orientation (portrait 9:16, landscape 16:9); the renderer
  // letterboxes anyway, this just avoids big bars.
  bool portrait = false;
  const bool archive = pkg::ends_with(o.package_dir, ".dpk") || pkg::ends_with(o.package_dir, ".zip");
  if (auto files = open_package(o.package_dir, archive ? read_file(o.package_dir) : nullptr)) {
    if (auto toml = files.value().text("dreamsy.toml")) portrait = toml->find("\"portrait\"") != std::string_view::npos;
  }

  sapp_desc desc{};
  desc.init_cb = init;
  desc.frame_cb = frame;
  desc.cleanup_cb = cleanup;
  desc.event_cb = event;
  desc.width = portrait ? 540 : 960;
  desc.height = portrait ? 960 : 540;
  desc.high_dpi = true;
  desc.window_title = "Dreamsy";
  desc.logger.func = slog_func;
  return desc;
}
