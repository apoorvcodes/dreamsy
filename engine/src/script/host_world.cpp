// Host: everything that touches entities. A port of the entity half of engine-web/core/game.ts:
// scene building, entity props and methods, spawn/clone/destroy, tile maps, physics events,
// tweens, sprite animation, particles, the camera, dialogs and leaderboards.
//
// Functions that can fail return Status (errors are values, docs/engine.md §14.2); the op handlers
// turn a failed Status into a Lua error with exactly engine-web's message.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "lua.h"
#include "lualib.h"
#include "pkg/formats.h"
#include "pkg/text.h"
#include "script/host.h"
#include "script/lua_util.h"
#include "script/ops.h"

namespace dreamsy::script {

using world::Entity;
using world::EntityId;
using world::Point;
using world::Value;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr const char* kDefaultBoard = "fastest";

bool is_transform_key(std::string_view k) { return k == "position" || k == "rotation" || k == "scale"; }

bool is_physics_key(std::string_view k) {
  for (std::string_view p : {"body", "shape", "size", "mass", "friction", "bounce", "gravity_scale", "lock_rotation", "collision",
                             "tile_size", "layer", "mask"})
    if (k == p) return true;
  return false;
}

bool is_tweenable(pkg::PropKind k) {
  return k == pkg::PropKind::Number || k == pkg::PropKind::Int || k == pkg::PropKind::Vec2 || k == pkg::PropKind::Color;
}

// ^[a-z0-9_]{1,40}$ (leaderboard board names, docs/platform.md §13)
bool is_board_name(std::string_view s) {
  if (s.empty() || s.size() > 40) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return pkg::is_lower(c) || pkg::is_digit(c) || c == '_'; });
}

// Raises when a Status failed (op handlers only).
void check(lua_State* L, const Status& s) {
  if (!s) raise(L, s.error());
}

// The first `max_units` UTF-16 units of a UTF-8 string (JS String.slice(0, n)).
std::string utf16_prefix(const std::string& s, size_t max_units) {
  if (pkg::utf16_length(s) <= max_units) return s;
  size_t units = 0, i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    size_t u = len == 4 ? 2 : 1;
    if (units + u > max_units) break;
    units += u;
    i += len;
  }
  return s.substr(0, i);
}

}  // namespace

// ================================================================ scene building

world::ViewMode Host::view_for(const std::string& scene_path) const {
  if (options_.view) return *options_.view;
  auto it = pkg_->scenes.find(scene_path);
  return it == pkg_->scenes.end() ? world::ViewMode::Side : world::scene_view(pkg_->manifest, it->second);
}

void Host::new_physics() {
  physics_ = std::make_unique<physics::Physics2D>(world_, dt_, pkg_->manifest.physics_layers.value_or(std::vector<std::string>{}));
}

// Instantiates the current scene: entities, tiles, physics, then attached scripts in tree order.
void Host::build_scene() {
  const pkg::SceneDef& scene = pkg_->scenes.at(scene_path_);
  std::vector<EntityId> created = world_.load_scene(scene, [&](const std::string& m) { log("warn", "[scene] " + m); });
  for (EntityId id : created) {
    const Entity* e = world_.get(id);
    if (e && e->type->name == "TileMap2D") (void)load_tiles(id, false);
    physics_->mark_dirty(id);
  }
  physics_->flush();
  for (EntityId id : created) {
    const Entity* e = world_.get(id);
    if (!e) continue;
    const std::vector<std::string> scripts = e->scripts;
    for (const std::string& p : scripts) run_script(p, id);
  }
}

// ================================================================ values across the boundary

EntityId Host::entity_arg(lua_State* L, int index) {
  EntityId e = world_.by_handle(lua_tonumber(L, index));
  if (!world_.alive(e)) raise(L, "entity was destroyed");
  return e;
}

void Host::push_entity(lua_State* L, uint32_t handle) {
  lua_getref(L, refs_.proxy);
  lua_pushnumber(L, handle);
  if (vm_->pcall(L, 1, 1)) {
    lua_pop(L, 1);
    lua_pushnil(L);
  }
}

// A tagged value (tag at `index`, payload after). Entities arrive as their handle (a number);
// nil arrives as nullopt.
std::optional<Value> Host::read_tagged(lua_State* L, int index) {
  switch (static_cast<int>(lua_tonumber(L, index))) {
    case TAG_VEC2: return Value::vec(lua_tonumber(L, index + 1), lua_tonumber(L, index + 2));
    case TAG_VEC3: return Value::vec3(lua_tonumber(L, index + 1), lua_tonumber(L, index + 2), lua_tonumber(L, index + 3));
    case TAG_COLOR:
      return Value::color(lua_tonumber(L, index + 1), lua_tonumber(L, index + 2), lua_tonumber(L, index + 3),
                          lua_tonumber(L, index + 4));
    case TAG_ENTITY: return Value::number(lua_tonumber(L, index + 1));
    default: break;
  }
  const int i = index + 1;
  switch (lua_type(L, i)) {
    case LUA_TNUMBER: return Value::number(lua_tonumber(L, i));
    case LUA_TSTRING: return Value::string(str_arg(L, i));
    case LUA_TBOOLEAN: return Value::boolean(lua_toboolean(L, i));
    case LUA_TNIL:
    case LUA_TNONE: return std::nullopt;
    default: raise(L, "unsupported value type");
  }
}

int Host::push_prop(lua_State* L, EntityId id, const std::string& key_in) {
  const Entity& e = *world_.get(id);
  const std::string& type = e.type->name;
  auto plain_nil = [&] {
    lua_pushnumber(L, TAG_PLAIN);
    lua_pushnil(L);
    return 2;
  };
  auto plain_str = [&](const std::string& s) {
    lua_pushnumber(L, TAG_PLAIN);
    push_str(L, s);
    return 2;
  };
  if (key_in == "id") return plain_str(e.id);
  if (key_in == "type") return plain_str(type);
  if (key_in == "name") return plain_str(e.name);
  if (key_in == "parent") {
    const Entity* p = world_.get(e.parent);
    if (!p) return plain_nil();
    lua_pushnumber(L, TAG_ENTITY);
    lua_pushnumber(L, p->handle);
    return 2;
  }
  if (key_in == "velocity" && type == "Body2D") {
    const Point v = physics_->velocity(id);
    lua_pushnumber(L, TAG_VEC2);
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    return 3;
  }
  if (key_in == "onFloor" && type == "Body2D") {
    const bool floor = physics_->on_floor(id);
    lua_pushnumber(L, TAG_PLAIN);
    lua_pushboolean(L, floor);
    return 2;
  }
  // 2.5D (§14.5): resting on the ground / the ground height under the body's center.
  if (key_in == "onGround" && type == "Body2D") {
    lua_pushnumber(L, TAG_PLAIN);
    lua_pushboolean(L, physics_->on_ground(id));
    return 2;
  }
  if (key_in == "groundZ" && type == "Body2D") {
    lua_pushnumber(L, TAG_PLAIN);
    lua_pushnumber(L, physics_->ground_of(id));
    return 2;
  }
  if ((key_in == "cols" || key_in == "rows") && type == "TileMap2D") {
    lua_pushnumber(L, TAG_PLAIN);
    lua_pushnumber(L, !e.tiles ? 0 : key_in == "cols" ? e.tiles->cols() : e.tiles->rows());
    return 2;
  }
  if (key_in == "particleCount" && type == "Particles2D") {
    lua_pushnumber(L, TAG_PLAIN);
    lua_pushnumber(L, e.emitter ? e.emitter->count() : 0);
    return 2;
  }
  const std::string key(world::prop_alias(e, key_in));
  const pkg::PropDef* def = e.type->prop(key);
  if (!def) raise(L, type + " \"" + e.id + "\" has no property \"" + key + "\"");
  const std::optional<Value> v = world_.get_prop(e, key);
  const bool vector_like = def->kind == pkg::PropKind::Vec2 || def->kind == pkg::PropKind::Vec3 || def->kind == pkg::PropKind::Color;
  if (!v && vector_like) return plain_nil();
  switch (def->kind) {
    case pkg::PropKind::Vec2:
      lua_pushnumber(L, TAG_VEC2);
      lua_pushnumber(L, v->v[0]);
      lua_pushnumber(L, v->v[1]);
      return 3;
    case pkg::PropKind::Vec3:
      lua_pushnumber(L, TAG_VEC3);
      for (int i = 0; i < 3; ++i) lua_pushnumber(L, v->v[i]);
      return 4;
    case pkg::PropKind::Color:
      lua_pushnumber(L, TAG_COLOR);
      for (int i = 0; i < 4; ++i) lua_pushnumber(L, v->v[i]);
      return 5;
    case pkg::PropKind::Entity: {
      const Entity* t = v && v->kind == world::ValueKind::String ? world_.get(world_.by_id(v->s)) : nullptr;
      if (!t) return plain_nil();
      lua_pushnumber(L, TAG_ENTITY);
      lua_pushnumber(L, t->handle);
      return 2;
    }
    default:
      lua_pushnumber(L, TAG_PLAIN);
      if (!v) lua_pushnil(L);
      else if (v->kind == world::ValueKind::Number) lua_pushnumber(L, v->v[0]);
      else if (v->kind == world::ValueKind::Bool) lua_pushboolean(L, v->b);
      else push_str(L, world::js_string(*v));
      return 2;
  }
}

Status Host::set_prop(EntityId id, const std::string& key, const std::optional<Value>& value) {
  Entity* e = world_.get(id);
  if (!e) return Error{"entity was destroyed"};
  const std::string& type = e->type->name;
  if (key == "color" && type == "Sprite2D") return set_prop(id, "modulate", value);
  if (key == "name") {
    e->name = value ? world::js_string(*value) : e->id;
    return Status::success();
  }
  if (key == "velocity" && type == "Body2D") {
    if (!value || value->kind != world::ValueKind::Vec) return Error{"velocity expects a Vec2"};
    physics_->set_velocity(id, value->v[0], value->v[1]);
    return Status::success();
  }
  if (key == "parent") {
    EntityId p = !value ? world_.root() : value->kind == world::ValueKind::Number ? world_.by_handle(value->v[0]) : EntityId{};
    if (!world_.alive(p)) return Error{"parent must be an Entity"};
    for (const Entity* x = world_.get(p); x; x = world_.get(x->parent))
      if (x == e) return Error{"cannot parent an entity to its own descendant"};
    world_.attach(id, p);
    after_transform(id);
    return Status::success();
  }
  if (key == "id" || key == "type" || key == "onFloor" || key == "onGround" || key == "groundZ" || key == "particleCount")
    return Error{key + " is read-only"};
  if ((key == "cols" || key == "rows") && type == "TileMap2D") return Error{key + " is read-only (edit tiles with setTile)"};

  const int index = world::prop_index(*e->type, key);
  if (index < 0) return Error{type + " \"" + e->id + "\" has no property \"" + key + "\""};
  std::optional<Value> v;
  if (value) {
    auto c = world_.coerce(*e, key, *value);
    if (!c) return Error{c.error()};
    v = std::move(c).value();
  }
  if (world::is_elevation_key(key)) {
    // 2.5D props live in Entity fields that physics reads and writes every tick (world.h).
    if (v) world_.set_raw(*e, key, *v);
    else world_.unset(*e, key);
    if (key == "height" && physics_) physics_->height_changed(id);
    return Status::success();
  }
  std::optional<Value>& slot = e->props[static_cast<size_t>(index)];
  const std::optional<Value> prev = slot;
  slot = v;  // nil resets a prop to its catalog default
  if ((key == "layer" || key == "mask") && physics_) {
    auto g = physics_->groups(*e);
    if (!g) {
      e->props[static_cast<size_t>(index)] = prev;
      return Error{g.error()};
    }
  }
  if (is_transform_key(key)) {
    after_transform(id);
    if (key == "scale")
      for (EntityId x : world_.walk(id)) physics_->mark_dirty(x);
  } else if (is_physics_key(key)) {
    physics_->mark_dirty(id);
  } else if (key == "visible") {
    physics_->apply_enabled(id);
  } else if (type == "TileMap2D" && (key == "tileset" || key == "data") && !spawning_.count(e->handle)) {
    return load_tiles(id, true);
  }
  return Status::success();
}

void Host::after_transform(EntityId e) {
  if (!physics_) return;
  const Entity* ent = world_.get(e);
  if (ent && ent->children.empty()) return physics_->sync_transform(e);  // no subtree to walk
  for (EntityId x : world_.walk(e)) physics_->sync_transform(x);
}

// ================================================================ destroy and clone

Status Host::destroy_entity(EntityId id) {
  if (id == world_.root()) return Error{"cannot destroy the scene root"};
  std::vector<EntityId> subtree = world_.walk(id);
  for (EntityId x : subtree) physics_->remove(x);
  world_.remove(id);
  auto removed = [&](EntityId x) { return std::find(subtree.begin(), subtree.end(), x) != subtree.end(); };
  lua_State* L = vm_->main();
  for (auto it = conns_.begin(); it != conns_.end();) {
    const Conn& c = it->second;
    if ((!c.entity.is_null() && removed(c.entity)) || (!c.inst->owner.is_null() && removed(c.inst->owner))) {
      lua_unref(L, c.ref);
      it = conns_.erase(it);
    } else {
      ++it;
    }
  }
  std::erase_if(ticks_, [&](const TickCb& t) {
    if (t.inst->owner.is_null() || !removed(t.inst->owner)) return false;
    lua_unref(L, t.ref);
    return true;
  });
  std::vector<int64_t> timers;
  for (const Timer& t : timers_)
    if (!t.inst->owner.is_null() && removed(t.inst->owner)) timers.push_back(t.id);
  for (int64_t t : timers) remove_timer(t, true);
  return Status::success();
}

EntityId Host::clone_entity(EntityId src_id, EntityId parent) {
  std::vector<EntityId> made;
  std::function<EntityId(EntityId, EntityId)> copy = [&](EntityId from, EntityId to) -> EntityId {
    const Entity* src = world_.get(from);
    auto created = world_.create("", src->type->name, src->name, src->scripts);
    EntityId c = created.value();  // the type exists: the source has it
    Entity* ce = world_.get(c);
    src = world_.get(from);  // the pool may have grown
    ce->props = src->props;
    if (src->tiles) {
      ce->tiles = src->tiles->clone();
      ce->tiles_key = src->tiles_key;
    }
    ce->tags = src->tags;
    ce->z = src->z;
    ce->height = src->height;
    ce->z_velocity = src->z_velocity;
    ce->z_gravity_scale = src->z_gravity_scale;
    const std::vector<EntityId> kids = src->children;
    world_.attach(c, to);
    made.push_back(c);
    for (EntityId k : kids)
      if (world_.alive(k)) copy(k, c);
    return c;
  };
  const Entity* src = world_.get(src_id);
  EntityId target = world_.alive(parent) ? parent : world_.alive(src->parent) ? src->parent : world_.root();
  EntityId root = copy(src_id, target);
  for (EntityId c : made) physics_->mark_dirty(c);
  for (EntityId c : made) {
    const Entity* e = world_.get(c);
    if (!e) continue;
    const std::vector<std::string> scripts = e->scripts;
    for (const std::string& s : scripts) run_script(s, c);
  }
  return root;
}

// ================================================================ tile maps

// (Re)loads a TileMap2D's grid from its tileset/data props. `strict`: fail (a script set a bad value).
Status Host::load_tiles(EntityId id, bool strict) {
  Entity* e = world_.get(id);
  const std::string tileset = world_.str(*e, "tileset"), data = world_.str(*e, "data");
  std::string key = tileset;
  key.push_back('\0');
  key += data;
  if (key == e->tiles_key && e->tiles) return Status::success();
  e->tiles_key = key;
  world::TileLoad r = world::load_tile_grid(pkg_->files, tileset, data);
  if (!r.errors.empty()) {
    const std::string msg = "TileMap2D \"" + e->id + "\": " + pkg::join(r.errors, "; ");
    if (strict && !r.grid) return Error{msg};
    log("warn", "[tilemap] " + msg);
  }
  e->tiles = std::move(r.grid);
  physics_->mark_dirty(id);
  return Status::success();
}

Result<world::TileGrid*> Host::tiles_of(EntityId id) {
  Entity* e = world_.get(id);
  if (e->type->name != "TileMap2D") return Error{e->type->name + " \"" + e->id + "\" is not a TileMap2D"};
  if (!e->tiles) return Error{"TileMap2D \"" + e->id + "\" has no tiles (check its tileset and data)"};
  return e->tiles.get();
}

Point Host::world_to_cell(EntityId e, double x, double y) {
  const Point l = world::apply(world::invert(world_.global(e)), x, y);
  const double ts = world_.num(*world_.get(e), "tile_size");
  return {std::floor(l.x / ts), std::floor(l.y / ts)};
}

// Fires tileTouched for (moving body, tile) pairs that started touching this tick.
void Host::detect_tile_touches() {
  if (!physics_) return;
  std::vector<EntityId> maps;
  for (const auto& [cid, c] : conns_) {
    const Entity* m = world_.get(c.entity);
    if (c.signal == "tileTouched" && m && m->tiles && std::find(maps.begin(), maps.end(), c.entity) == maps.end())
      maps.push_back(c.entity);
  }
  if (maps.empty()) {
    tile_touching_.clear();
    return;
  }
  const std::vector<EntityId> bodies = physics_->moving_bodies();
  for (EntityId map : maps) {
    const Entity* me = world_.get(map);
    if (!me || !me->tiles) continue;
    const uint32_t map_handle = me->handle;
    const std::set<std::pair<uint32_t, int>> prev = tile_touching_[map_handle];
    std::set<std::pair<uint32_t, int>> now;
    struct Fresh {
      EntityId body;
      uint32_t handle;
      int c, r;
    };
    std::vector<Fresh> fresh;
    if (effective_visible(map)) {
      const world::TileGrid& grid = *me->tiles;
      const world::Mat2D inv = world::invert(world_.global(map));
      const double ts = world_.num(*me, "tile_size");
      for (EntityId b : bodies) {
        const Entity* be = world_.get(b);
        if (!be) continue;
        std::optional<physics::Aabb> box = physics_->aabb(b);
        if (!box) continue;
        // body AABB in map-local pixels
        double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        const double corners[4][2] = {{box->x0, box->y0}, {box->x1, box->y0}, {box->x0, box->y1}, {box->x1, box->y1}};
        for (const auto& corner : corners) {
          const Point l = world::apply(inv, corner[0], corner[1]);
          x0 = std::min(x0, l.x), y0 = std::min(y0, l.y), x1 = std::max(x1, l.x), y1 = std::max(y1, l.y);
        }
        const int c0 = static_cast<int>(std::max(0.0, std::floor((x0 - 1) / ts)));
        const int c1 = static_cast<int>(std::min<double>(grid.cols() - 1, std::floor((x1 + 1) / ts)));
        const int r0 = static_cast<int>(std::max(0.0, std::floor((y0 - 1) / ts)));
        const int r1 = static_cast<int>(std::min<double>(grid.rows() - 1, std::floor((y1 + 1) / ts)));
        for (int r = r0; r <= r1; ++r) {
          for (int c = c0; c <= c1; ++c) {
            const uint16_t idx = grid.cell(c, r);
            if (idx == 0) continue;
            const world::TileKind& k = grid.kinds().kinds[idx];
            if (!k.solid && k.tags.empty()) continue;
            const double ox = std::min(x1, (c + 1) * ts) - std::max(x0, c * ts);
            const double oy = std::min(y1, (r + 1) * ts) - std::max(y0, r * ts);
            // solid: touching (within 1px) along a shared edge; non-solid tagged: overlapping
            const bool hit = k.solid ? (ox > 0.5 && oy > -1) || (oy > 0.5 && ox > -1) : ox > 0.5 && oy > 0.5;
            if (!hit) continue;
            const std::pair<uint32_t, int> key{be->handle, r * grid.cols() + c};
            now.insert(key);
            if (!prev.count(key)) fresh.push_back({b, be->handle, c, r});
          }
        }
      }
    }
    tile_touching_[map_handle] = std::move(now);
    for (const Fresh& f : fresh) {
      const Entity* m = world_.get(map);
      if (!m || !world_.alive(f.body) || !m->tiles) break;
      const world::TileKind* k = m->tiles->kind_at(f.c, f.r);
      if (!k) continue;  // removed by an earlier handler this tick
      const char ch = k->ch;
      fire(map, "tileTouched", [&](lua_State* S) {
        push_entity(S, f.handle);
        push_str(S, std::string(1, ch));
        lua_pushnumber(S, f.c);
        lua_pushnumber(S, f.r);
        return 4;
      });
    }
  }
  for (auto it = tile_touching_.begin(); it != tile_touching_.end();) {
    if (!world_.alive(world_.by_handle(it->first))) it = tile_touching_.erase(it);
    else ++it;
  }
}

// ================================================================ physics step

void Host::step_physics(double sdt) {
  std::vector<physics::ContactEvent> events = physics_->step(sdt);
  for (const physics::ContactEvent& ev : events) {
    if (!world_.alive(ev.a) || !world_.alive(ev.b)) continue;
    const uint32_t other = world_.get(ev.b)->handle;
    fire(ev.a, ev.started ? "bodyEntered" : "bodyExited", [&](lua_State* S) {
      push_entity(S, other);
      return 1;
    });
  }
  detect_tile_touches();
}

// ================================================================ tweens

Result<int64_t> Host::start_tween(EntityId e, std::vector<world::TweenTrack> tracks, double duration, const std::string& easing,
                                  double delay, bool loop, bool yoyo) {
  world::Easing ease = world::find_easing(easing);
  if (!ease) {
    std::vector<std::string> names(world::easing_names().begin(), world::easing_names().end());
    return Error{"tween: unknown easing \"" + easing + "\" (use " + pkg::join(names, ", ") + ")"};
  }
  if (!(duration >= 0)) return Error{"tween: seconds must be a number ≥ 0"};
  // a new tween of the same prop on the same entity replaces the old one's track
  for (world::Tween& tw : tweens_) {
    if (tw.entity != e) continue;
    std::erase_if(tw.tracks, [&](const world::TweenTrack& t) {
      return std::any_of(tracks.begin(), tracks.end(), [&](const world::TweenTrack& n) { return n.key == t.key; });
    });
  }
  const int64_t id = next_id_++;
  world::Tween tw;
  tw.id = id;
  tw.entity = e;
  tw.tracks = std::move(tracks);
  tw.duration = duration;
  tw.delay = std::max(0.0, delay);
  tw.ease = ease;
  tw.loop = loop;
  tw.yoyo = yoyo;
  tweens_.push_back(std::move(tw));
  return id;
}

void Host::step_tweens(double sdt) {
  if (tweens_.empty()) return;
  std::vector<int64_t> order;
  for (const world::Tween& tw : tweens_) order.push_back(tw.id);
  std::vector<int64_t> finished;
  auto index_of = [&](int64_t id) -> int {
    for (size_t i = 0; i < tweens_.size(); ++i)
      if (tweens_[i].id == id) return static_cast<int>(i);
    return -1;
  };
  for (int64_t id : order) {
    int i = index_of(id);
    if (i < 0) continue;
    if (!world_.alive(tweens_[static_cast<size_t>(i)].entity)) {
      tweens_.erase(tweens_.begin() + i);
      call_ref(refs_.done_fire, nullptr, 3, [&](lua_State* S) {
        lua_pushnumber(S, static_cast<double>(id));
        lua_pushnil(S);
        lua_pushboolean(S, true);
      });
      continue;
    }
    world::Tween& tw = tweens_[static_cast<size_t>(i)];
    tw.t += sdt;
    const world::TweenProgress p = world::tween_progress(tw);
    if (p.k < 0) continue;
    const EntityId target = tw.entity;
    const std::vector<world::TweenTrack> tracks = tw.tracks;
    for (const world::TweenTrack& tr : tracks) {
      if (Status s = set_prop(target, tr.key, world::track_value(tr, p.k)); !s) log("warn", "[tween] " + s.error());
    }
    if (p.finished) {
      if (int j = index_of(id); j >= 0) tweens_.erase(tweens_.begin() + j);
      finished.push_back(id);
    }
  }
  for (int64_t id : finished) {
    call_ref(refs_.done_fire, nullptr, 2, [&](lua_State* S) {
      lua_pushnumber(S, static_cast<double>(id));
      lua_pushnil(S);
    });
  }
}

int Host::tween_op(lua_State* L) {
  const std::string what = str_arg(L, 2);
  if (what == "cancel") {
    const double id = lua_tonumber(L, 3);
    auto it = std::find_if(tweens_.begin(), tweens_.end(), [&](const world::Tween& t) { return static_cast<double>(t.id) == id; });
    if (it != tweens_.end()) tweens_.erase(it);
    return 0;
  }
  // "start", handle, seconds, easing, delay, loop, yoyo, then (key, tag, a, b, c, d)*
  const EntityId id = entity_arg(L, 3);
  const Entity& e = *world_.get(id);
  const double seconds = lua_tonumber(L, 4);
  const std::string easing = lua_isnoneornil(L, 5) ? std::string(world::kDefaultEasing) : str_arg(L, 5);
  const double delay = lua_isnoneornil(L, 6) ? 0 : lua_tonumber(L, 6);
  const bool loop = lua_toboolean(L, 7), yoyo = lua_toboolean(L, 8);
  std::vector<world::TweenTrack> tracks;
  for (int i = 9; i + 5 <= lua_gettop(L) && !lua_isnil(L, i); i += 6) {
    const std::string key(world::prop_alias(e, str_arg(L, i)));
    const pkg::PropDef* def = e.type->prop(key);
    if (!def || !is_tweenable(def->kind)) {
      std::vector<std::string> list;
      for (const pkg::PropDef& d : e.type->props)
        if (is_tweenable(d.kind)) list.push_back(d.name);
      raise(L, "tween: " + e.type->name + " \"" + e.id + "\" has no tweenable property \"" + key + "\" (numbers, Vec2s and colors: " +
                   pkg::join(list, ", ") + ")");
    }
    const std::optional<Value> target = read_tagged(L, i + 1);
    world::TweenTrack tr;
    tr.key = key;
    tr.kind = def->kind == pkg::PropKind::Vec2    ? world::TweenTrack::Kind::Vec2
              : def->kind == pkg::PropKind::Color ? world::TweenTrack::Kind::Color
                                                  : world::TweenTrack::Kind::Number;
    const std::optional<Value> cur = world_.get_prop(e, key);
    switch (tr.kind) {
      case world::TweenTrack::Kind::Number:
        if (!target || target->kind != world::ValueKind::Number) raise(L, "tween: " + key + " expects a number");
        tr.to = {target->v[0]};
        tr.from = {cur && cur->kind == world::ValueKind::Number ? cur->v[0] : 0};
        break;
      case world::TweenTrack::Kind::Vec2:
        if (!target || !target->is_vec(2)) raise(L, "tween: " + key + " expects a Vec2");
        tr.to = {target->v[0], target->v[1]};
        if (cur && cur->kind == world::ValueKind::Vec) tr.from = {cur->v[0], cur->v[1]};
        else tr.from = {0, 0};
        break;
      case world::TweenTrack::Kind::Color: {
        double c[4];
        if (target && target->is_vec(4)) std::copy(target->v, target->v + 4, c);
        else if (!target || target->kind != world::ValueKind::String || !world::parse_color(target->s, c))
          raise(L, "tween: " + key + " expects a Color or \"#rrggbb\"");
        tr.to = {c[0], c[1], c[2], c[3]};
        if (cur && cur->kind == world::ValueKind::Vec && cur->n >= 3) tr.from = {cur->v[0], cur->v[1], cur->v[2], cur->n > 3 ? cur->v[3] : 1};
        else tr.from = {1, 1, 1, 1};
        break;
      }
    }
    tracks.push_back(std::move(tr));
  }
  if (tracks.empty()) raise(L, "tween: give at least one property, e.g. tween(e, { position = vec2(100, 50) }, 0.5)");
  auto started = start_tween(id, std::move(tracks), seconds, easing, delay, loop, yoyo);
  if (!started) raise(L, started.error());
  lua_pushnumber(L, static_cast<double>(started.value()));
  return 1;
}

// ================================================================ sprite animation and particles

// Parsed data/anims/<name>.json (nullptr when missing or invalid; logged once).
const pkg::AnimSet* Host::anim_set(std::string_view path_view) {
  auto it = anims_cache_.find(path_view);
  if (it == anims_cache_.end()) {
    const std::string path(path_view);
    std::optional<pkg::AnimSet> set;
    auto text = pkg_->files.text(path);
    if (!text) {
      log("warn", "[anims] " + path + " does not exist");
    } else {
      pkg::Parsed<pkg::AnimSet> r = pkg::parse_anims(*text);
      if (!r.errors.empty()) log("warn", "[anims] " + path + ": " + pkg::join(r.errors, "; "));
      if (r.value && !r.value->empty()) set = std::move(r.value);
    }
    it = anims_cache_.emplace(path, std::move(set)).first;
  }
  return it->second ? &*it->second : nullptr;
}

namespace {
const pkg::Anim* find_anim(const pkg::AnimSet& set, std::string_view name) {
  for (const auto& [n, a] : set)
    if (n == name) return &a;
  return nullptr;
}
}  // namespace

// e:play(name): on a Sprite2D with `anims`, else its first descendant that has them.
Status Host::play_anim(EntityId id, const std::string& name, bool restart) {
  EntityId sprite;
  for (EntityId x : world_.walk(id)) {
    const Entity* xe = world_.get(x);
    if (xe->type->name == "Sprite2D" && !world_.str(*xe, "anims").empty()) {
      sprite = x;
      break;
    }
  }
  const Entity& e = *world_.get(id);
  if (sprite.is_null())
    return Error{"play: " + e.type->name + " \"" + e.id + "\" has no Sprite2D with anims (set anims = \"data/anims/<name>.json\" on a Sprite2D)"};
  const std::string anims = world_.str(*world_.get(sprite), "anims");
  const pkg::AnimSet* set = anim_set(anims);
  if (!set) return Error{"play: " + anims + " could not be loaded"};
  if (!find_anim(*set, name)) {
    std::vector<std::string> names;
    for (const auto& [n, a] : *set) names.push_back(n);
    return Error{"play: no animation \"" + name + "\" in " + anims + " (has: " + pkg::join(names, ", ") + ")"};
  }
  Entity& s = *world_.get(sprite);
  s.anim_owner = sprite == id ? EntityId{} : id;
  if (world_.str(s, "anim") == name && !restart && !s.anim_done) return Status::success();
  world_.set_raw(s, "anim", Value::string(name));
  s.anim_name.clear();
  advance_anim(sprite, 0);
  return Status::success();
}

void Host::advance_anim(EntityId id, double dt) {
  Entity* e = world_.get(id);
  // Valid until `anim` is written, which happens only after finished_name copies it.
  const std::string_view name = world_.str_view(*e, world::Hot::Anim);
  if (name.empty()) {
    e->anim_name.clear();
    return;
  }
  const pkg::AnimSet* set = anim_set(world_.str_view(*e, "anims"));
  e = world_.get(id);
  const pkg::Anim* a = set ? find_anim(*set, name) : nullptr;
  if (!a || a->frames.empty()) {
    e->anim_name.clear();
    return;
  }
  if (e->anim_name != name) {
    e->anim_name = name;
    e->anim_t = 0;
    e->anim_done = false;
  } else if (!e->anim_done) {
    e->anim_t += dt;
  }
  double f = std::floor(e->anim_t * a->fps + 1e-9);
  const double n = static_cast<double>(a->frames.size());
  bool finished = false;
  if (a->loop) f = std::fmod(f, n);
  else if (f >= n) {
    f = n - 1;
    finished = !e->anim_done;
  }
  const std::string& tex = a->frames[static_cast<size_t>(f)];
  const std::optional<Value>& cur = e->props[static_cast<size_t>(world::World::index(*e, world::Hot::Texture))];
  if (!cur || cur->s != tex) world_.set_raw(*e, "texture", Value::string(tex));
  if (!finished) return;
  e->anim_done = true;
  const std::string finished_name(name);  // scripts below may change `anim`
  const EntityId owner = e->anim_owner;
  for (EntityId target : {id, owner}) {
    if (target.is_null() || !world_.alive(target)) continue;
    fire(target, "animFinished", [&](lua_State* S) {
      push_str(S, finished_name);
      return 1;
    });
  }
  e = world_.get(id);
  if (a->next && e && world_.str_view(*e, world::Hot::Anim) == finished_name) {
    world_.set_raw(*e, "anim", Value::string(*a->next));
    advance_anim(id, 0);
  }
}

world::EmitterParams Host::emitter_params(const Entity& e) const {
  world::EmitterParams p;
  p.max_particles = static_cast<int>(world_.num(e, "max_particles"));
  p.direction = world_.num(e, "direction");
  p.spread = world_.num(e, "spread");
  p.speed = world_.num(e, "speed");
  p.speed_random = world_.num(e, "speed_random");
  p.lifetime = world_.num(e, "lifetime");
  p.lifetime_random = world_.num(e, "lifetime_random");
  const Point area = world_.vec(e, "emission_size");
  p.emission_w = area.x, p.emission_h = area.y;
  const Point g = world_.vec(e, "gravity");
  p.gravity_x = g.x, p.gravity_y = g.y;
  p.damping = world_.num(e, "damping");
  p.rate = world_.num(e, "rate");
  p.emitting = world_.flag(e, "emitting");
  return p;
}

world::Emitter& Host::emitter_of(Entity& e) {
  if (!e.emitter) {
    // engine-web: new Random(seed * 7919 + handle * 104729), which takes the value mod 2^32
    const uint64_t seed = static_cast<uint64_t>(options_.seed) * 7919u + static_cast<uint64_t>(e.handle) * 104729u;
    e.emitter = std::make_unique<world::Emitter>(static_cast<uint32_t>(seed));
  }
  return *e.emitter;
}

Status Host::burst(EntityId id, double n) {
  Entity& e = *world_.get(id);
  if (e.type->name != "Particles2D") return Error{"burst: " + e.type->name + " \"" + e.id + "\" is not a Particles2D"};
  const world::Mat2D m = world_.global(id);
  emitter_of(e).emit(emitter_params(e), static_cast<int>(std::max(0.0, std::floor(n))), m.tx, m.ty, std::atan2(m.b, m.a) * 180 / kPi);
  return Status::success();
}

void Host::step_sprites_and_particles(double sdt) {
  std::vector<EntityId>& sprites = scratch_sprites_;
  std::vector<EntityId>& emitters = scratch_emitters_;
  sprites.clear();
  emitters.clear();
  for (EntityId id : world_.order()) {
    const world::Kind kind = world_.get(id)->info->kind;
    if (kind == world::Kind::Sprite2D) sprites.push_back(id);
    else if (kind == world::Kind::Particles2D) emitters.push_back(id);
  }
  for (EntityId id : sprites) {
    Entity* e = world_.get(id);
    if (!e) continue;
    const std::optional<Value>& anim = e->props[static_cast<size_t>(world::World::index(*e, world::Hot::Anim))];
    if (anim) {
      if (anim->kind == world::ValueKind::String && !anim->s.empty()) advance_anim(id, sdt);
      e = world_.get(id);
      if (!e) continue;
    }
    if (world_.flag(*e, world::Hot::AutoFlip)) {
      std::optional<double> vx;
      for (EntityId p = e->parent; world_.alive(p); p = world_.get(p)->parent) {
        if (world_.get(p)->info->kind == world::Kind::Body2D) {
          vx = physics_ ? physics_->velocity(p).x : 0;
          break;
        }
      }
      e = world_.get(id);
      const double gx = world_.global(id).tx;
      if (!vx) vx = std::isnan(e->last_x) || sdt <= 0 ? 0 : (gx - e->last_x) / sdt;
      e->last_x = gx;
      const bool flip_now = world_.flag(*e, world::Hot::FlipH);
      const bool flip = *vx < -1 ? true : *vx > 1 ? false : flip_now;
      if (flip != flip_now) world_.set_raw(*e, "flip_h", Value::boolean(flip));
    }
  }
  for (EntityId id : emitters) {
    Entity* e = world_.get(id);
    if (!e) continue;
    const world::Mat2D m = world_.global(id);
    emitter_of(*e).step(emitter_params(*e), sdt, m.tx, m.ty, std::atan2(m.b, m.a) * 180 / kPi, effective_visible(id));
  }
}

// ================================================================ queries

bool Host::effective_visible(EntityId id) const {
  for (const Entity* x = world_.get(id); x; x = world_.get(x->parent))
    if (!world_.visible(*x)) return false;
  return true;
}

EntityId Host::avatar() const {
  std::vector<EntityId> players = world_.tagged("player");
  return players.empty() ? EntityId{} : players[0];
}

EntityId Host::current_camera() const {
  for (EntityId id : world_.order()) {
    const Entity* e = world_.get(id);
    if (e->info->kind == world::Kind::Camera2D && world_.flag(*e, "current")) return id;
  }
  return {};
}

// ================================================================ camera

Host::CameraView Host::camera_view() {
  if (!cam_.init) update_camera(true);
  const Entity* cam = world_.get(cam_.entity);
  const double zoom = cam ? std::max(0.05, world_.num(*cam, "zoom")) : 1;
  return {cam_.x + cam_.ox, cam_.y + cam_.oy, zoom};
}

void Host::update_camera(bool snap) {
  const double rw = pkg_->manifest.reference_width(), rh = pkg_->manifest.reference_height();
  const EntityId cam_id = current_camera();
  Camera& c = cam_;
  if (const Entity* cam = world_.get(cam_id); !cam) {
    c.x = rw / 2;
    c.y = rh / 2;
  } else {
    const double zoom = std::max(0.05, world_.num(*cam, "zoom"));
    const std::optional<Value> follow = world_.get_prop(*cam, "follow");
    EntityId target = follow && follow->kind == world::ValueKind::String ? world_.by_id(follow->s) : EntityId{};
    const world::Mat2D m = world_.global(world_.alive(target) ? target : cam_id);
    double x = m.tx, y = m.ty;
    if (!snap && c.init && cam_id == c.entity && world_.alive(target)) {
      const Point dz = world_.vec(*cam, "deadzone");
      const double dx = x - c.x, dy = y - c.y;
      const double hx = dz.x / 2, hy = dz.y / 2;
      auto sign = [](double v) { return v > 0 ? 1.0 : v < 0 ? -1.0 : 0.0; };
      x = std::fabs(dx) <= hx ? c.x : x - sign(dx) * hx;
      y = std::fabs(dy) <= hy ? c.y : y - sign(dy) * hy;
      const double sm = world_.num(*cam, "smoothing");
      if (sm > 0) {
        const double k = 1 - std::pow(sm, dt_ * 10);
        x = c.x + (x - c.x) * k;
        y = c.y + (y - c.y) * k;
      }
    }
    // limits: keep the visible rect inside [limit_min, limit_max]
    const std::optional<Value> lo = world_.get_prop(*cam, "limit_min"), hi = world_.get_prop(*cam, "limit_max");
    const double vw = rw / zoom, vh = rh / zoom;
    auto clamp = [](double v, std::optional<double> a, std::optional<double> b, double half) {
      const double mn = a ? *a + half : -INFINITY, mx = b ? *b - half : INFINITY;
      if (mn > mx) return (a.value_or(0) + b.value_or(0)) / 2;
      return std::max(mn, std::min(mx, v));
    };
    auto comp = [](const std::optional<Value>& v, int i) -> std::optional<double> {
      if (!v || v->kind != world::ValueKind::Vec) return std::nullopt;
      return v->v[i];
    };
    c.x = clamp(x, comp(lo, 0), comp(hi, 0), vw / 2);
    c.y = clamp(y, comp(lo, 1), comp(hi, 1), vh / 2);
  }
  c.entity = cam_id;
  c.init = true;
  if (!snap) {
    if (c.shake_t > 0) {
      c.shake_t = std::max(0.0, c.shake_t - dt_);
      const double amp = c.shake_i * (c.shake_dur > 0 ? c.shake_t / c.shake_dur : 0);
      c.ox = (cam_rng_.next() * 2 - 1) * amp;
      c.oy = (cam_rng_.next() * 2 - 1) * amp;
    } else {
      c.ox = 0;
      c.oy = 0;
    }
  }
}

// camera.shake: world-pixel amplitude, decaying linearly over `seconds`. Stronger shakes replace weaker ones.
void Host::shake(double intensity, double seconds) {
  Camera& c = cam_;
  const double remaining = c.shake_dur > 0 ? c.shake_i * (c.shake_t / c.shake_dur) : 0;
  if (intensity < remaining) return;
  c.shake_i = std::max(0.0, intensity);
  c.shake_dur = c.shake_t = std::max(dt_, seconds);
}

int Host::camera_op(lua_State* L) {
  const std::string what = str_arg(L, 2);
  const double rw = pkg_->manifest.reference_width(), rh = pkg_->manifest.reference_height();
  if (what == "shake") {
    shake(lua_tonumber(L, 3), lua_isnoneornil(L, 4) ? 0.3 : lua_tonumber(L, 4));
    return 0;
  }
  if (what == "current") {
    if (const Entity* c = world_.get(current_camera())) lua_pushnumber(L, c->handle);
    else lua_pushnil(L);
    return 1;
  }
  if (what == "center") {
    const CameraView v = camera_view();
    lua_pushnumber(L, v.x - cam_.ox);
    lua_pushnumber(L, v.y - cam_.oy);
    return 2;
  }
  if (what == "snap") {
    cam_.init = false;
    return 0;
  }
  if (what == "viewSize") {
    const double z = camera_view().zoom;
    lua_pushnumber(L, rw / z);
    lua_pushnumber(L, rh / z);
    return 2;
  }
  if (what == "screenSize") {
    lua_pushnumber(L, rw);
    lua_pushnumber(L, rh);
    return 2;
  }
  if (what == "toWorld" || what == "toScreen") {
    // Through the view's projection (iso: the 2:1 diamond), on the ground plane (z = 0).
    const CameraView v = camera_view();
    const Point c = world::project_ground(world_.view, v.x - cam_.ox, v.y - cam_.oy);
    const double x = lua_tonumber(L, 3), y = lua_tonumber(L, 4);
    if (what == "toWorld") {
      const Point w = world::unproject_ground(world_.view, c.x + (x - rw / 2) / v.zoom, c.y + (y - rh / 2) / v.zoom);
      lua_pushnumber(L, w.x);
      lua_pushnumber(L, w.y);
    } else {
      const Point p = world::project_ground(world_.view, x, y);
      lua_pushnumber(L, (p.x - c.x) * v.zoom + rw / 2);
      lua_pushnumber(L, (p.y - c.y) * v.zoom + rh / 2);
    }
    return 2;
  }
  if (what == "view") {
    push_str(L, world::view_mode_name(world_.view));
    return 1;
  }
  raise(L, "camera: unknown call " + what);
}

// ================================================================ entity methods (OP_CALL)

int Host::entity_call(lua_State* L) {
  const EntityId id = entity_arg(L, 2);
  const std::string m = str_arg(L, 3);
  auto tiles = [&]() -> world::TileGrid& {
    auto t = tiles_of(id);
    if (!t) raise(L, t.error());
    return *t.value();
  };
  if (m == "children") {
    const std::vector<EntityId> kids = world_.get(id)->children;
    lua_createtable(L, static_cast<int>(kids.size()), 0);
    for (size_t i = 0; i < kids.size(); ++i) {
      lua_pushnumber(L, world_.get(kids[i])->handle);
      lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
  }
  if (m == "hasTag") {
    lua_pushboolean(L, world_.get(id)->has_tag(str_arg(L, 4)));
    return 1;
  }
  if (m == "addTag") {
    world_.get(id)->add_tag(str_arg(L, 4));
    return 0;
  }
  if (m == "removeTag") {
    world_.get(id)->remove_tag(str_arg(L, 4));
    return 0;
  }
  if (m == "destroy") {
    check(L, destroy_entity(id));
    return 0;
  }
  if (m == "clone") {
    const EntityId parent = lua_isnoneornil(L, 4) ? EntityId{} : entity_arg(L, 4);
    const EntityId c = clone_entity(id, parent);
    lua_pushnumber(L, world_.get(c) ? world_.get(c)->handle : 0);
    return 1;
  }
  if (m == "tileAt" || m == "tagsAt" || m == "worldToCell") {
    const world::TileGrid& grid = tiles();
    const std::optional<Value> p = read_tagged(L, 4);
    if (!p || !p->is_vec(2)) raise(L, m + " expects a Vec2 world position");
    const Point cell = world_to_cell(id, p->v[0], p->v[1]);
    if (m == "worldToCell") {
      lua_pushnumber(L, cell.x);
      lua_pushnumber(L, cell.y);
      return 2;
    }
    const bool fits = cell.x >= -2147483648.0 && cell.x <= 2147483647.0 && cell.y >= -2147483648.0 && cell.y <= 2147483647.0;
    const world::TileKind* k = fits ? grid.kind_at(static_cast<int>(cell.x), static_cast<int>(cell.y)) : nullptr;
    if (m == "tileAt") {
      if (k) push_str(L, std::string(1, k->ch));
      else lua_pushnil(L);
      return 1;
    }
    const std::vector<std::string> tags = k ? k->tags : std::vector<std::string>{};
    lua_createtable(L, static_cast<int>(tags.size()), 0);
    for (size_t i = 0; i < tags.size(); ++i) {
      push_str(L, tags[i]);
      lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
  }
  auto cell_arg = [&](int i) {
    const double v = std::floor(lua_tonumber(L, i));
    return v >= -2147483648.0 && v <= 2147483647.0 ? static_cast<int>(v) : -1;
  };
  if (m == "isSolid") {
    const world::TileKind* k = tiles().kind_at(cell_arg(4), cell_arg(5));
    lua_pushboolean(L, k && k->solid);
    return 1;
  }
  if (m == "tileDef") {
    const world::TileGrid& grid = tiles();
    const std::string ch = str_arg(L, 4);
    const world::TileKind* k = ch.size() == 1 ? grid.by_char(ch[0]) : nullptr;
    if (!k) {
      lua_pushnil(L);
      return 1;
    }
    lua_createtable(L, 0, 5);
    push_str(L, std::string(1, k->ch)), lua_setfield(L, -2, "char");
    lua_pushboolean(L, k->solid), lua_setfield(L, -2, "solid");
    lua_createtable(L, static_cast<int>(k->tags.size()), 0);
    for (size_t i = 0; i < k->tags.size(); ++i) {
      push_str(L, k->tags[i]);
      lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "tags");
    if (k->sprite) push_str(L, pkg::starts_with(*k->sprite, "asset:") ? *k->sprite : "asset:" + *k->sprite), lua_setfield(L, -2, "sprite");
    if (k->has_color) {
      static const char* hex = "0123456789abcdef";
      std::string s;
      for (double x : k->color) {
        const int b = static_cast<int>(world::js_round(x * 255));
        s.push_back(hex[(b >> 4) & 15]);
        s.push_back(hex[b & 15]);
      }
      if (pkg::ends_with(s, "ff")) s.resize(s.size() - 2);
      push_str(L, "#" + s), lua_setfield(L, -2, "color");
    }
    return 1;
  }
  if (m == "getTile") {
    const world::TileKind* k = tiles().kind_at(cell_arg(4), cell_arg(5));
    if (k) push_str(L, std::string(1, k->ch));
    else lua_pushnil(L);
    return 1;
  }
  if (m == "cellToWorld") {
    tiles();
    const double ts = world_.num(*world_.get(id), "tile_size");
    const Point p = world::apply(world_.global(id), (lua_tonumber(L, 4) + 0.5) * ts, (lua_tonumber(L, 5) + 0.5) * ts);
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    return 2;
  }
  if (m == "setTile") {
    world::TileGrid& grid = tiles();
    if (lua_type(L, 4) != LUA_TNUMBER || lua_type(L, 5) != LUA_TNUMBER) raise(L, "setTile expects (col: number, row: number, char: string?)");
    const double cd = std::floor(lua_tonumber(L, 4)), rd = std::floor(lua_tonumber(L, 5));
    if (!(cd >= 0 && rd >= 0 && cd < grid.cols() && rd < grid.rows())) {
      raise(L, "setTile: cell (" + pkg::number_text(cd) + ", " + pkg::number_text(rd) + ") is outside the map (cols 0.." +
                   std::to_string(grid.cols() - 1) + ", rows 0.." + std::to_string(grid.rows() - 1) + ")");
    }
    const std::string ch = lua_isnoneornil(L, 6) ? "." : str_arg(L, 6);
    const world::TileKind* kind = nullptr;
    if (ch != "." && ch != " " && !ch.empty()) {
      kind = ch.size() == 1 ? grid.by_char(ch[0]) : nullptr;
      if (!kind) raise(L, "setTile: tile \"" + ch + "\" is not defined in " + world_.str(*world_.get(id), "tileset"));
    }
    const uint32_t solid = grid.solid_rev();
    grid.set(static_cast<int>(cd), static_cast<int>(rd), kind);
    if (grid.solid_rev() != solid) physics_->mark_dirty(id);
    return 0;
  }
  if (m == "play") {
    const std::string name = str_arg(L, 4);
    if (name.empty()) raise(L, "play expects an animation name, e.g. e:play(\"run\")");
    check(L, play_anim(id, name, lua_toboolean(L, 5)));
    return 0;
  }
  if (m == "stop") {
    for (EntityId x : world_.walk(id)) {
      Entity* xe = world_.get(x);
      if (xe->type->name == "Sprite2D" && !world_.str(*xe, "anims").empty()) {
        world_.set_raw(*xe, "anim", Value::string(""));
        xe->anim_name.clear();
        break;
      }
    }
    return 0;
  }
  if (m == "burst") {
    check(L, burst(id, lua_isnoneornil(L, 4) ? 16 : lua_tonumber(L, 4)));
    return 0;
  }
  if (m == "clearParticles") {
    if (Entity* e = world_.get(id); e->emitter) e->emitter->clear();
    return 0;
  }
  if (m == "applyImpulse") {
    const std::optional<Value> v = read_tagged(L, 4);
    if (!v || v->kind != world::ValueKind::Vec) raise(L, "applyImpulse expects a Vec2");
    if (world_.get(id)->type->name == "Body2D") physics_->apply_impulse(id, v->v[0], v->v[1]);
    return 0;
  }
  raise(L, "unknown method " + m);
}

int Host::spawn_op(lua_State* L) {
  if (lua_isnoneornil(L, 2)) {  // finish: props are set, build tiles and physics
    const EntityId e = world_.by_handle(lua_tonumber(L, 4));
    Entity* ent = world_.get(e);
    if (!ent) return 0;
    spawning_.erase(ent->handle);
    if (ent->type->name == "TileMap2D" && !ent->tiles) check(L, load_tiles(e, true));
    physics_->mark_dirty(e);
    return 0;
  }
  const std::string type = str_arg(L, 2);
  const EntityId parent = lua_isnoneornil(L, 3) ? world_.root() : entity_arg(L, 3);
  auto made = world_.create("", type);
  if (!made) raise(L, "spawn: unknown entity type \"" + type + "\"");
  const EntityId e = made.value();
  if (world_.get(e)->type->dimension == pkg::Dimension::D3) {
    world_.remove(e);
    raise(L, "spawn: " + type + " is a 3D type; this is a 2D game");
  }
  world_.attach(e, parent);
  const Entity& ent = *world_.get(e);
  if (ent.type->name == "TileMap2D") spawning_.insert(ent.handle);
  lua_pushnumber(L, ent.handle);
  return 1;
}

// ================================================================ dialogs

int Host::dialog_op(lua_State* L) {
  const std::string what = str_arg(L, 2);
  if (what == "open") {
    // kind, lines table, options table|nil, speed, position, block
    ui::DialogState st;
    st.choice = str_arg(L, 3) == "choice";
    const int n = lua_istable(L, 4) ? lua_objlen(L, 4) : 0;
    for (int i = 1; i <= std::min(n, 200); ++i) {
      lua_rawgeti(L, 4, i);
      const int t = lua_gettop(L);
      auto field = [&](const char* k) -> std::optional<std::string> {
        if (!lua_istable(L, t)) return std::nullopt;
        lua_rawgetfield(L, t, k);
        std::optional<std::string> s;
        if (!lua_isnil(L, -1) && lua_tostring(L, -1)) s = str_arg(L, -1);
        lua_pop(L, 1);
        return s;
      };
      ui::DialogLine line;
      line.text = utf16_prefix(field("text").value_or(""), 2000);
      line.speaker = field("speaker");
      line.portrait = field("portrait");
      line.length = pkg::utf16_length(line.text);
      st.lines.push_back(std::move(line));
      lua_pop(L, 1);
    }
    if (st.lines.empty()) raise(L, "ui.dialog: give at least one line");
    if (!lua_isnoneornil(L, 5)) {
      const int count = lua_istable(L, 5) ? lua_objlen(L, 5) : 0;
      for (int i = 1; i <= std::min(count, 8); ++i) {
        lua_rawgeti(L, 5, i);
        st.options.push_back(str_arg(L, -1));
        lua_pop(L, 1);
      }
      if (st.choice && st.options.empty()) raise(L, "ui.choice: give at least one option");
    }
    st.speed = lua_isnoneornil(L, 6) ? 40 : std::max(0.0, lua_tonumber(L, 6));
    st.top = str_arg(L, 7) == "top";
    st.blocking = lua_isnoneornil(L, 8) ? true : lua_toboolean(L, 8);
    st.id = next_id_++;
    st.shown = st.speed > 0 ? 0 : static_cast<double>(st.lines[0].length);
    const int64_t id = st.id;
    dialogs_.dialogs.open(std::move(st));
    lua_pushnumber(L, static_cast<double>(id));
    return 1;
  }
  if (what == "close") {
    dialogs_.dialogs.close(static_cast<int64_t>(lua_tonumber(L, 3)));
    return 0;
  }
  if (what == "isOpen") {
    lua_pushboolean(L, dialogs_.open());
    return 1;
  }
  raise(L, "ui.dialog: unknown call " + what);
}

// ================================================================ leaderboards (offline)

int Host::leaderboard_op(lua_State* L) {
  const std::string what = str_arg(L, 2);
  auto board_arg = [&](int i, const char* fn) -> std::optional<std::string> {
    if (lua_isnoneornil(L, i)) return std::nullopt;
    const std::string b = str_arg(L, i);
    if (!is_board_name(b)) {
      raise(L, std::string("leaderboard.") + fn +
                   ": board must be a name like \"fastest\" or \"track_1\" (lowercase letters, digits and _, at most 40), got \"" + b + "\"");
    }
    return b;
  };
  auto find_run = [&](const std::string& board) {
    return std::find_if(lb_runs_.begin(), lb_runs_.end(), [&](const LbRun& r) { return r.board == board; });
  };
  // nil board -> "fastest" if running, else the most recently started run, else "fastest"
  auto running_board = [&](const std::optional<std::string>& b) -> std::string {
    if (b) return *b;
    if (find_run(kDefaultBoard) != lb_runs_.end()) return kDefaultBoard;
    const LbRun* best = nullptr;
    for (const LbRun& r : lb_runs_)
      if (!best || r.seq > best->seq) best = &r;
    return best ? best->board : kDefaultBoard;
  };
  if (what == "start") {
    const std::string board = board_arg(3, "start").value_or(kDefaultBoard);
    if (!lua_isnoneornil(L, 4)) entity_arg(L, 4);
    LbRun run{board, time_, lb_seq_++};
    if (auto it = find_run(board); it != lb_runs_.end()) *it = run;  // Map.set keeps the position
    else lb_runs_.push_back(run);
    return 0;
  }
  if (what == "finish") {
    const std::string board = running_board(board_arg(3, "finish"));
    auto it = find_run(board);
    if (it == lb_runs_.end()) {
      log("warn", "[leaderboard] finish(\"" + board + "\") without a running leaderboard.start(\"" + board + "\")");
      lua_pushnil(L);
      return 1;
    }
    const double t0 = it->t0;
    lb_runs_.erase(it);
    const double seconds = world::js_round((time_ - t0) * 1000) / 1000;
    local_scores_.push_back({board, seconds, true});
    if (options_.on_run_end) options_.on_run_end(RunEnd{"finish", board, seconds});
    lua_pushnumber(L, seconds);
    return 1;
  }
  if (what == "submit") {
    const std::optional<std::string> board = board_arg(3, "submit");
    if (!board) raise(L, "leaderboard.submit expects a board name, e.g. leaderboard.submit(\"score\", 1200)");
    if (lua_type(L, 4) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, 4))) raise(L, "leaderboard.submit expects a number value");
    const std::string order = lua_isnoneornil(L, 5) ? "desc" : str_arg(L, 5);
    if (order != "asc" && order != "desc") raise(L, "leaderboard.submit: order must be \"asc\" (lower is better) or \"desc\" (higher is better)");
    local_scores_.push_back({*board, lua_tonumber(L, 4), order == "asc"});
    if (options_.on_run_end) options_.on_run_end(RunEnd{"submit", *board, lua_tonumber(L, 4)});
    return 0;  // the submission resolves asynchronously (never during a headless run)
  }
  if (what == "cancel") {
    const std::optional<std::string> b = board_arg(3, "cancel");
    if (!b) lb_runs_.clear();
    else if (auto it = find_run(*b); it != lb_runs_.end()) lb_runs_.erase(it);
    return 0;
  }
  if (what == "elapsed") {
    auto it = find_run(running_board(board_arg(3, "elapsed")));
    lua_pushnumber(L, it != lb_runs_.end() ? time_ - it->t0 : 0);
    return 1;
  }
  if (what == "running") {
    lua_pushboolean(L, find_run(running_board(board_arg(3, "running"))) != lb_runs_.end());
    return 1;
  }
  if (what == "raceGhost") {
    if (str_arg(L, 3).empty()) raise(L, "leaderboard.raceGhost expects \"best\", \"pb\" or a run id");
    board_arg(4, "raceGhost");
    return 0;  // ghosts are fetched asynchronously (never during a headless run)
  }
  raise(L, "leaderboard: unknown call " + what);
}

// ================================================================ state hash

uint64_t Host::state_hash() {
  Hasher h;
  h.add(tick_);
  h.add(time_);
  world_.hash_into(h);
  if (physics_) physics_->hash_into(h);
  h.add(cam_.x), h.add(cam_.y), h.add(cam_.ox), h.add(cam_.oy);
  h.add(static_cast<uint64_t>(tweens_.size()));
  for (const world::Tween& t : tweens_) {
    h.add(t.id);
    h.add(t.t);
  }
  return h.value();
}

}  // namespace dreamsy::script
