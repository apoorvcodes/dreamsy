// The entity world: a tree of typed entities built from a scene (docs/format.md §3.3).
//
// A port of engine-web/core/world.ts. Entities live in a Pool and are referenced by EntityId
// (generational handle, base/pool.h). Each entity also has a numeric `handle`: a sequence number
// that is never reused, which is what scripts hold (the prelude's entity proxies) and what
// engine-web uses, so both engines hand out the same numbers in the same order.
//
// 2.5D (§14.5) is part of the model from the start: every entity has a height above the ground
// (`z`) and a vertical extent (`height`), and the world has a view mode (manifest / scene `view`).
// The catalog props `z`, `height`, `z_velocity` and `z_gravity_scale` (§3.3, slice E7) are stored in
// the Entity fields below, not in `props`: physics moves `z` every tick, and get_prop / set_raw /
// unset route those keys to the fields, so scripts, tweens and the scene loader all see one value.
// With the defaults (z = 0, height = 0, view = side) everything behaves exactly like 2D.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/hash.h"
#include "base/pool.h"
#include "base/result.h"
#include "pkg/catalog.h"
#include "pkg/package.h"
#include "world/particles.h"
#include "world/tilemap.h"
#include "world/value.h"

namespace dreamsy::world {

struct Entity;
using EntityId = Id<Entity>;

// Props read every frame for many entities; their indices are cached per type (TypeInfo::hot).
enum class Hot : uint8_t {
  Position, Rotation, Scale, Visible, ZIndex, Texture, Size, Anchor,
  FlipH, FlipV, Modulate, Blend, Emissive, Anim, AutoFlip, LockRotation,
  Count
};
std::string_view hot_name(Hot key);

// Entity types checked every frame, so the checks compare integers instead of type names.
enum class Kind : uint8_t { Other, Sprite2D, Text2D, TileMap2D, Particles2D, Body2D, Light2D, Lighting2D, Camera2D };

// Per-type data computed once (type_info()) so prop reads, the hottest lookup, don't rebuild it.
struct TypeInfo {
  Kind kind = Kind::Other;
  bool ui = false;                             // UIPanel, UIText, UIButton, ... (name starts with "UI")
  std::vector<std::optional<Value>> defaults;  // by prop index; nullopt = no default
  int16_t hot[static_cast<int>(Hot::Count)];   // prop index per Hot key, -1 = not a prop of this type
  // Prop index of z, height, z_velocity, z_gravity_scale (-1 = not a prop of this type).
  int elevation[4] = {-1, -1, -1, -1};
};
const TypeInfo& type_info(const pkg::EntityType& type);

// How a scene is projected (§14.5): manifest `view`, overridden per scene.
enum class ViewMode : uint8_t { Side, TopDown, Oblique, Iso };
const char* view_mode_name(ViewMode v);
// "side" | "topdown" | "oblique" | "iso" -> mode; anything else (or nothing) -> Side.
ViewMode parse_view_mode(std::string_view name);
// The view a scene runs in: its own `view`, else the manifest's, else side.
ViewMode scene_view(const pkg::Manifest& manifest, const pkg::SceneDef& scene);

struct Entity {
  uint32_t handle = 0;  // script handle: 1, 2, 3, ... never reused
  std::string id;
  const pkg::EntityType* type = nullptr;
  const TypeInfo* info = nullptr;  // type_info(*type), set with `type`
  std::string name;
  std::vector<std::string> scripts;
  std::vector<std::string> tags;  // insertion order, no duplicates
  EntityId parent;                // null for the scene root (and detached entities)
  std::vector<EntityId> children; // ordered

  // Prop values by catalog prop index (type->props[i]); unset = catalog default.
  std::vector<std::optional<Value>> props;

  // 2.5D: height of the entity's base above the ground, and its vertical extent (px). Bodies
  // with height 0 are "flat" and collide with everything, as in plain 2D (see physics2d.h).
  // These are the catalog props of the same names (see is_elevation_key()). `z` is relative to
  // the parent, like `position`.
  double z = 0;
  double height = 0;
  double z_velocity = 0;       // px/s, + = up; integrated by physics for moving bodies
  double z_gravity_scale = 1;  // multiplier on physics::kZGravity

  // Runtime state owned by subsystems.
  std::optional<std::pair<double, double>> pending_velocity;  // velocity set before the body exists
  std::unique_ptr<TileGrid> tiles;  // TileMap2D: the live grid
  std::string tiles_key;            // tileset + "\0" + data the grid was loaded from
  std::unique_ptr<Emitter> emitter; // Particles2D: live particles
  // Sprite2D animation playback (engine-web Entity.animName/animT/animDone/animOwner).
  std::string anim_name;
  double anim_t = 0;
  bool anim_done = false;
  EntityId anim_owner;  // entity e:play() was called on, when it forwarded to this sprite
  double last_x = std::numeric_limits<double>::quiet_NaN();  // auto_flip without a body

  bool has_tag(std::string_view tag) const;
  void add_tag(const std::string& tag);
  void remove_tag(std::string_view tag);
  bool is_2d() const { return type->dimension == pkg::Dimension::D2; }
};

// ---------------------------------------------------------------- 2D transforms

// Affine 2D: maps (x, y) to (a x + c y + tx, b x + d y + ty), like engine-web's Mat2D.
struct Mat2D {
  double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};
Mat2D mul(const Mat2D& m, const Mat2D& n);
Mat2D invert(const Mat2D& m);
struct Point {
  double x = 0, y = 0;
};
Point apply(const Mat2D& m, double x, double y);
double rotation_of(const Mat2D& m);  // radians

// A point on the ground (z = 0) -> where the view draws it, before the camera: identity in
// side/topdown/oblique, the 2:1 diamond (x - y, (x + y) / 2) in iso (render/view.h project()).
// unproject_ground() is the inverse (screen taps -> world, camera.screenToWorld).
Point project_ground(ViewMode v, double x, double y);
Point unproject_ground(ViewMode v, double px, double py);

// ---------------------------------------------------------------- the world

class World {
 public:
  using Warn = std::function<void(const std::string&)>;

  // Creates a detached entity (attach() puts it in the tree). `id` empty = auto id "rt_N".
  // Fails for unknown types and duplicate ids.
  Result<EntityId> create(std::string_view id, std::string_view type, std::optional<std::string> name = {},
                          std::vector<std::string> scripts = {});
  // Moves `child` under `parent` (null = detach) at `index` (-1 or out of range = last).
  void attach(EntityId child, EntityId parent, int index = -1);
  // Removes the subtree. Returns the removed entities, children first (they are dead afterwards).
  std::vector<EntityId> remove(EntityId e);
  // Removes every entity (scene change). Handles are never reused.
  void clear();
  // Builds a scene's entity tree; returns the created entities in tree order.
  std::vector<EntityId> load_scene(const pkg::SceneDef& scene, const Warn& warn);

  Entity* get(EntityId id) { return pool_.get(id); }
  const Entity* get(EntityId id) const { return pool_.get(id); }
  bool alive(EntityId id) const { return pool_.contains(id); }
  EntityId by_handle(double handle) const;
  EntityId by_id(std::string_view id) const;
  EntityId root() const { return root_; }
  size_t count() const { return pool_.size(); }

  // Depth-first, tree order (the entity itself first).
  std::vector<EntityId> walk(EntityId from) const;
  std::vector<EntityId> walk() const { return walk(root_); }
  // walk() without the copy, cached until the tree changes: only for loops that don't create, remove or move entities.
  const std::vector<EntityId>& order() const;
  std::vector<EntityId> tagged(std::string_view tag) const;

  // ---- props

  // The prop value: set value, else the catalog default (colors parsed to RGBA), else nullopt.
  std::optional<Value> get_prop(const Entity& e, std::string_view key) const;
  // The set value or cached default without copying (nullptr if neither); not for elevation keys.
  const Value* peek(const Entity& e, int index) const;
  // Typed shortcuts (0 / false / "" / (0,0) when the prop is missing or unset).
  double num(const Entity& e, std::string_view key) const;
  bool flag(const Entity& e, std::string_view key) const;
  std::string str(const Entity& e, std::string_view key) const;
  // str() without the copy; valid until the prop is next written.
  std::string_view str_view(const Entity& e, std::string_view key) const;
  Point vec(const Entity& e, std::string_view key) const;
  bool visible(const Entity& e) const;  // own `visible` prop (not the parents')
  // The same reads by Hot key, without matching prop names.
  static int index(const Entity& e, Hot key);
  const Value* peek(const Entity& e, Hot key) const;
  double num(const Entity& e, Hot key) const;
  bool flag(const Entity& e, Hot key) const;
  std::string_view str_view(const Entity& e, Hot key) const;
  Point vec(const Entity& e, Hot key) const;
  // Stores a value without checks (engine writes such as physics positions). The key must exist.
  void set_raw(Entity& e, std::string_view key, Value v);
  void unset(Entity& e, std::string_view key);

  // Checks and converts a value for a prop (engine-web coerceProp). Errors are worded for the
  // script author. Entity values are given as a script handle (number) or entity id (string).
  Result<Value> coerce(const Entity& e, std::string_view key, const Value& value) const;

  // ---- transforms

  Mat2D local(const Entity& e) const;
  Mat2D global(EntityId e) const;  // null = identity

  // ---- 2.5D
  ViewMode view = ViewMode::Side;

  // Adds every entity (tree order) to a state hash.
  void hash_into(Hasher& h) const;

 private:
  Pool<Entity> pool_;
  std::unordered_map<uint32_t, EntityId> by_handle_;
  std::unordered_map<std::string, EntityId> by_id_;
  EntityId root_;
  mutable std::vector<EntityId> order_;  // order(): the cached walk()
  mutable bool order_stale_ = true;     // set wherever the tree changes
  uint32_t next_handle_ = 1;
  uint32_t next_auto_ = 1;
};

// The 2.5D props stored in Entity fields instead of `props`: z, height, z_velocity, z_gravity_scale.
bool is_elevation_key(std::string_view key);
// The Entity field behind an elevation key (nullptr for other keys).
double* elevation_field(Entity& e, std::string_view key);
const double* elevation_field(const Entity& e, std::string_view key);

// Index of a prop in its type's catalog list, or -1.
int prop_index(const pkg::EntityType& type, std::string_view key);
// Catalog default as a runtime value (nullopt = no default).
std::optional<Value> default_value(const pkg::PropDef& def);
// Sprite2D `color` is stored as `modulate` (engine-web propAlias).
std::string_view prop_alias(const Entity& e, std::string_view key);
// JS Math.round (halves round up).
double js_round(double x);
// JS String(v) for a value (arrays join with ",").
std::string js_string(const Value& v);

}  // namespace dreamsy::world
