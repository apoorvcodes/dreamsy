// The script host: runs a package's Luau scripts with the platform API (format/LUAU_API.md).
//
// A port of engine-web/core/game.ts (docs/engine.md §14.13). The prelude (engine-web/core/prelude.ts,
// embedded at build time) builds the public API in Luau and calls numbered host ops, which are
// implemented here, natively. Same op numbers, value tags, script order and tick order as
// engine-web, so the two engines can be compared op by op.
//
// The host owns the simulation of one game: the entity world (world/), physics (physics/), tweens,
// sprite animation, particles, the camera, dialogs and the scripts that drive them. host.cpp has
// scripts, modules, timers, scenes and the non-entity ops; host_world.cpp has everything that
// touches entities (props, methods, spawn/clone/destroy, tile maps, tweens, anims, particles,
// camera, physics events).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/result.h"
#include "base/rng.h"
#include "physics/physics2d.h"
#include "pkg/package.h"
#include "input/input_state.h"
#include "script/json_value.h"
#include "script/kits.h"
#include "script/vm.h"
#include "ui/dialog.h"
#include "ui/toasts.h"
#include "world/tween.h"
#include "world/world.h"

namespace dreamsy::script {

struct HostOptions {
  uint32_t seed = 1;
  double budget_ms = 250;
  size_t memory_limit = 64u * 1024 * 1024;
  double tick_rate = 60;
  std::function<double()> clock;             // unix seconds for clock.now(); default: wall clock
  std::optional<std::string> scene;          // start here instead of entry_scene (tools/tests)
  std::optional<Json> scene_params;
  Json save;                                 // initial save data (an object)
  std::function<void(const struct LogLine&)> on_log;
  KitLookup kits;  // where kit sources come from; empty = script::default_kits() (script/kits.h)
  // Forces one 2.5D view mode for every scene (tools/tests). Unset = each scene's own view
  // (scene `view`, else manifest `view`, else side; docs/engine.md §14.5).
  std::optional<world::ViewMode> view;
  // A run ended (engine-web Game.onRunEnd, the app's {type:"runEnd"}): leaderboard finish/submit,
  // platform.runEnded, or the (offline) room finished. Called during step(); not rate-limited here.
  std::function<void(const struct RunEnd&)> on_run_end;
};

struct RunEnd {
  std::string reason;  // "finish" | "submit" | "gameover" | "quit"
  std::optional<std::string> board;
  std::optional<double> value;
};

struct LogLine {
  double time = 0;
  std::string level;  // "info" | "warn" | "error"
  std::string text;
  std::string script;
};

struct ScriptError {
  std::string script;
  int line = 0;  // 0 = unknown
  std::string message;
  std::string traceback;
  int count = 1;
  double time = 0;
};

struct AudioEvent {
  std::string kind;  // "sfx" | "music" | "music_stop"
  std::string id;
  double volume = 1, pitch = 1, fade = 0, time = 0;
};

struct Settings {
  double music = 0.7, sfx = 0.9;
  bool mute = false, vibration = true;
};

class Host {
 public:
  // The package must be valid (pkg::load_package). Runs the prelude and the session scripts.
  static Result<std::unique_ptr<Host>> create(std::shared_ptr<const pkg::Package> package, HostOptions options = {});
  ~Host();

  // One fixed simulation tick (engine-web Game.step order).
  void step();

  double time() const { return time_; }
  int64_t tick() const { return tick_; }
  const std::string& scene() const { return scene_path_; }
  const std::vector<LogLine>& logs() const { return logs_; }
  const std::vector<ScriptError>& errors() const { return errors_; }
  const std::vector<AudioEvent>& audio_events() const { return audio_; }
  const Json& save_data() const { return save_; }
  double time_scale() const { return time_scale_; }
  size_t memory_used() const { return vm_->memory_used(); }

  // The simulation (tools and tests may read and poke it between ticks).
  const pkg::Package& package() const { return *pkg_; }
  world::World& world() { return world_; }
  const world::World& world() const { return world_; }
  physics::Physics2D* physics() { return physics_.get(); }
  input::InputState& input() { return input_; }
  size_t tween_count() const { return tweens_.size(); }
  struct CameraView {
    double x = 0, y = 0, zoom = 1;  // view center (smoothing, limits and shake applied)
  };
  CameraView camera_view();
  // Hash of the simulation state: tick, time, entities, physics bodies, camera (§14.4).
  uint64_t state_hash();

  Settings settings;

  // ---- pointer, taps and platform UI (host_input.cpp). Positions are reference (UI) px.
  //
  // The platform layer (play/platform_ui.h) calls these as events arrive, between ticks. They fire
  // input.pointerDown / pointerUp / swipe right away, like engine-web's Game.pointerDown/Up.
  void pointer_down(double x, double y, std::optional<double> time = std::nullopt);
  void pointer_move(double x, double y);
  void pointer_up(double x, double y, std::optional<double> time = std::nullopt);
  // Headless tap (engine-web Game.tapScreen, playtests): pointer down + up, then the open dialog,
  // else the topmost UI element (UIButton.pressed + tapped), else the world. Returns "dialog",
  // the entity id that was hit, or "".
  std::string tap_screen(double x, double y);
  // World-space tap: `tapped` (+ tileTapped, tappedAt) on the topmost visible listening entity.
  world::EntityId tap_world(double wx, double wy);
  // `tapped` on one entity (the UI overlay); false if nothing listens.
  bool tap_entity(world::EntityId e);
  // UIButton press: fires `pressed` if the button is visible.
  void press(world::EntityId e);
  // Reference screen -> world (ground plane, side/topdown camera; engine-web refToWorld).
  world::Point ref_to_world(double x, double y);
  double ref_width() const;
  double ref_height() const;

  // Platform pause menu (engine-web Game.pause / resume / requestRestart). While paused the
  // player stops calling step(): no ticks, timers or physics.
  void pause();
  void resume();
  bool paused() const { return paused_; }
  // "Restart": fires game.restartRequested, or (no listeners) reloads the entry scene.
  void request_restart();

  // This session's finished runs (seconds, lower is better) and submitted values, oldest first:
  // the pause menu's local Leaderboard view.
  struct LocalScore {
    std::string board;
    double value = 0;
    bool ascending = false;
  };
  const std::vector<LocalScore>& local_scores() const { return local_scores_; }

  ui::DialogSystem& dialogs() { return dialogs_; }
  // The avatar: the first entity tagged `player` (engine-web Game.avatar), or null.
  world::EntityId avatar() const;
  const ui::ToastQueue& toasts() const { return toasts_; }

 private:
  struct ScriptInst {
    std::string path;
    world::EntityId owner;  // null = session (unattached scripts and module top levels)
  };
  struct TickCb {
    int64_t id;
    int ref;
    ScriptInst* inst;
  };
  struct Timer {
    int64_t id;
    double due;
    double interval;
    int ref;
    ScriptInst* inst;
    int64_t seq;
  };
  struct Conn {
    int64_t id;
    world::EntityId entity;  // null = game-level signal
    std::string signal;
    int ref;
    ScriptInst* inst;
  };
  struct Refs {
    int make_script = 0, proxy = 0, flush = 0, start = 0, invoke = 0, player = 0, scene_set = 0, done_fire = 0;
  };

  Host(std::shared_ptr<const pkg::Package> package, HostOptions options);
  Status init();

  static int host_call(lua_State* L);
  int dispatch(lua_State* L);
  int net_op(lua_State* L);

  void run_script(const std::string& path, world::EntityId owner = {});
  void setup_script_globals(lua_State* T, world::EntityId owner, const std::string& path);
  void call_ref(int ref, ScriptInst* inst, int nargs, const std::function<void(lua_State*)>& push_args = {});
  void fire(world::EntityId entity, const std::string& signal, const std::function<int(lua_State*)>& push_args);
  bool has_conn(world::EntityId entity, const std::string& signal) const;
  bool dead(const ScriptInst* inst) const { return !inst->owner.is_null() && !world_.alive(inst->owner); }
  void record_error(std::string message, const std::string& traceback, const std::string& fallback_script);
  void log(const std::string& level, const std::string& text, const std::string& script = {});

  std::string resolve_module(lua_State* L, const std::string& from, const std::string& name);
  void require_module(lua_State* L, const std::string& from, const std::string& name);
  std::optional<std::string_view> kit_source(const std::string& path) const;

  void process_scene_change();
  void switch_scene(const std::string& path);
  void step_dialogs();

  // ---- host_world.cpp: entities, physics, tweens, anims, particles, camera
  void new_physics();
  double bottom_anchor_shift(const world::Entity& e) const;  // host_input.cpp
  // The view a scene runs in (options_.view overrides it).
  world::ViewMode view_for(const std::string& scene_path) const;
  void build_scene();
  world::EntityId entity_arg(lua_State* L, int index);  // raises "entity was destroyed"
  void push_entity(lua_State* L, uint32_t handle);       // the prelude's proxy for a handle
  std::optional<world::Value> read_tagged(lua_State* L, int index);
  int push_prop(lua_State* L, world::EntityId e, const std::string& key);
  Status set_prop(world::EntityId e, const std::string& key, const std::optional<world::Value>& value);
  void after_transform(world::EntityId e);
  Status destroy_entity(world::EntityId e);
  world::EntityId clone_entity(world::EntityId e, world::EntityId parent);
  Status load_tiles(world::EntityId e, bool strict);
  Result<world::TileGrid*> tiles_of(world::EntityId e);
  world::Point world_to_cell(world::EntityId e, double x, double y);
  void detect_tile_touches();
  void step_physics(double sdt);
  Result<int64_t> start_tween(world::EntityId e, std::vector<world::TweenTrack> tracks, double duration,
                              const std::string& easing, double delay, bool loop, bool yoyo);
  void step_tweens(double sdt);
  const pkg::AnimSet* anim_set(std::string_view path);
  Status play_anim(world::EntityId e, const std::string& name, bool restart);
  void advance_anim(world::EntityId e, double dt);
  void step_sprites_and_particles(double sdt);
  world::EmitterParams emitter_params(const world::Entity& e) const;
  world::Emitter& emitter_of(world::Entity& e);
  Status burst(world::EntityId e, double n);
  bool effective_visible(world::EntityId e) const;
  world::EntityId current_camera() const;
  void update_camera(bool snap);
  void shake(double intensity, double seconds);
  int entity_call(lua_State* L);
  int spawn_op(lua_State* L);
  int tween_op(lua_State* L);
  int camera_op(lua_State* L);
  int dialog_op(lua_State* L);
  int leaderboard_op(lua_State* L);
  void remove_timer(int64_t id, bool unref);
  int ref_function(lua_State* L, int index, const std::string& what);
  ScriptInst* inst();
  void write_save(lua_State* L, Json next);

  std::shared_ptr<const pkg::Package> pkg_;
  HostOptions options_;
  // The world and physics are declared before the VM, so the VM (whose scripts may hold
  // callbacks into them) is destroyed first.
  world::World world_;
  std::unique_ptr<physics::Physics2D> physics_;
  std::unique_ptr<Vm> vm_;
  Rng rng_;
  Rng cam_rng_;
  input::InputState input_;
  ui::DialogSystem dialogs_;
  ui::ToastQueue toasts_;
  bool paused_ = false;
  Refs refs_;
  std::vector<std::string> enabled_kits_;

  double dt_;
  double time_ = 0;
  double time_scale_ = 1;
  int64_t tick_ = 0;
  int64_t next_id_ = 1;
  std::string scene_path_;
  std::optional<std::string> pending_scene_;

  std::vector<std::unique_ptr<ScriptInst>> scripts_;
  ScriptInst session_inst_{"(session)", {}};
  ScriptInst unknown_inst_{"?", {}};
  ScriptInst* current_ = nullptr;
  std::vector<TickCb> ticks_;          // sorted by id
  std::vector<TickCb> tick_snapshot_;  // step(): this tick's callbacks, reused
  std::vector<TickCb>::iterator find_tick(int64_t id);
  std::vector<Timer> timers_;
  std::map<int64_t, Conn> conns_;  // id order = creation order
  std::unordered_map<std::string, int> modules_;
  std::set<std::string> loading_modules_;
  std::unordered_map<std::string, Json> file_cache_;

  // entities
  std::vector<world::Tween> tweens_;
  std::set<uint32_t> spawning_;  // handles of TileMap2Ds being spawned (tiles load at the finish call)
  std::map<std::string, std::optional<pkg::AnimSet>, std::less<>> anims_cache_;  // less<>: find by string_view
  std::vector<world::EntityId> scratch_sprites_, scratch_emitters_;              // step_sprites_and_particles(), reused
  // Per TileMap2D handle: (body handle, cell) pairs touching last tick (tileTouched fires on new ones).
  std::map<uint32_t, std::set<std::pair<uint32_t, int>>> tile_touching_;
  struct Camera {
    double x = 0, y = 0;
    bool init = false;
    world::EntityId entity;
    double shake_t = 0, shake_dur = 0, shake_i = 0, ox = 0, oy = 0;
  } cam_;

  // leaderboards (offline: submissions never resolve during a headless run, as in engine-web's
  // synchronous runners; timing and running boards behave the same)
  struct LbRun {
    std::string board;
    double t0 = 0;
    int64_t seq = 0;
  };
  std::vector<LbRun> lb_runs_;  // Map insertion order
  int64_t lb_seq_ = 0;
  std::vector<LocalScore> local_scores_;

  // offline room: the avatar a script replicated (room.replicate)
  world::EntityId replicated_;

  std::vector<LogLine> logs_;
  std::vector<ScriptError> errors_;
  std::vector<AudioEvent> audio_;
  std::optional<std::string> music_id_;
  Json save_;

  // store (in-memory host, like MemoryStoreHost)
  std::set<std::string> store_owned_;   // delivered passes (what store.owns() sees)
  std::set<std::string> store_passes_;  // passes bought from the in-memory store
  struct Purchase {
    std::string item_id, purchase_id;
  };
  std::vector<Purchase> store_pending_, store_queue_;
  int store_next_ = 1;

  // offline room (engine-web NetSession without a link)
  int net_dispatch_ref_ = 0;
  Json room_state_ = Json::object_value();
  Json room_scores_ = room_state_;
  struct NetMsg {
    std::string kind;
    std::string key;
    Json value;
  };
  std::vector<NetMsg> net_inbox_;
  bool net_ended_ = false;
};

}  // namespace dreamsy::script
