#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pkg/text.h"

namespace dreamsy::world {

// ================================================================ small helpers

const char* view_mode_name(ViewMode v) {
  switch (v) {
    case ViewMode::Side: return "side";
    case ViewMode::TopDown: return "topdown";
    case ViewMode::Oblique: return "oblique";
    case ViewMode::Iso: return "iso";
  }
  return "side";
}

Point project_ground(ViewMode v, double x, double y) {
  if (v == ViewMode::Iso) return {x - y, (x + y) * 0.5};
  return {x, y};
}

Point unproject_ground(ViewMode v, double px, double py) {
  // Iso: px = x - y and py = (x + y) / 2, so x = py + px / 2 and y = py - px / 2.
  if (v == ViewMode::Iso) return {py + px * 0.5, py - px * 0.5};
  return {px, py};
}

ViewMode parse_view_mode(std::string_view name) {
  if (name == "topdown") return ViewMode::TopDown;
  if (name == "oblique") return ViewMode::Oblique;
  if (name == "iso") return ViewMode::Iso;
  return ViewMode::Side;
}

ViewMode scene_view(const pkg::Manifest& manifest, const pkg::SceneDef& scene) {
  if (scene.view) return parse_view_mode(*scene.view);
  if (manifest.view) return parse_view_mode(*manifest.view);
  return ViewMode::Side;
}

bool is_elevation_key(std::string_view key) {
  return key == "z" || key == "height" || key == "z_velocity" || key == "z_gravity_scale";
}

namespace {

const TypeInfo& info_of(const Entity& e) { return e.info ? *e.info : type_info(*e.type); }

// Elevation slot (0 z, 1 height, 2 z_velocity, 3 z_gravity_scale) of an elevation key, else -1.
int elevation_slot(std::string_view key) {
  if (key == "z") return 0;
  if (key == "height") return 1;
  if (key == "z_velocity") return 2;
  if (key == "z_gravity_scale") return 3;
  return -1;
}

const double* elevation_member(const Entity& e, int slot) {
  switch (slot) {
    case 0: return &e.z;
    case 1: return &e.height;
    case 2: return &e.z_velocity;
    default: return &e.z_gravity_scale;
  }
}

// The Entity field behind prop index `i` if it is one of the type's elevation props.
const double* elevation_at(const Entity& e, int i) {
  const TypeInfo& info = info_of(e);
  for (int slot = 0; slot < 4; ++slot)
    if (info.elevation[slot] == i) return elevation_member(e, slot);
  return nullptr;
}

}  // namespace

std::string_view hot_name(Hot key) {
  switch (key) {
    case Hot::Position: return "position";
    case Hot::Rotation: return "rotation";
    case Hot::Scale: return "scale";
    case Hot::Visible: return "visible";
    case Hot::ZIndex: return "z_index";
    case Hot::Texture: return "texture";
    case Hot::Size: return "size";
    case Hot::Anchor: return "anchor";
    case Hot::FlipH: return "flip_h";
    case Hot::FlipV: return "flip_v";
    case Hot::Modulate: return "modulate";
    case Hot::Blend: return "blend";
    case Hot::Emissive: return "emissive";
    case Hot::Anim: return "anim";
    case Hot::AutoFlip: return "auto_flip";
    case Hot::LockRotation: return "lock_rotation";
    case Hot::Count: break;
  }
  return "";
}

const TypeInfo& type_info(const pkg::EntityType& type) {
  // Built once for the immutable catalog; static init is thread-safe.
  static const std::unordered_map<const pkg::EntityType*, TypeInfo> infos = [] {
    std::unordered_map<const pkg::EntityType*, TypeInfo> out;
    for (const pkg::EntityType& t : pkg::Catalog::get().types()) {
      TypeInfo info;
      static const std::pair<const char*, Kind> kKinds[] = {
          {"Sprite2D", Kind::Sprite2D},       {"Text2D", Kind::Text2D}, {"TileMap2D", Kind::TileMap2D},
          {"Particles2D", Kind::Particles2D}, {"Body2D", Kind::Body2D}, {"Light2D", Kind::Light2D},
          {"Lighting2D", Kind::Lighting2D},   {"Camera2D", Kind::Camera2D}};
      for (const auto& [name, kind] : kKinds)
        if (t.name == name) info.kind = kind;
      info.ui = t.name.rfind("UI", 0) == 0;
      for (int k = 0; k < static_cast<int>(Hot::Count); ++k)
        info.hot[k] = static_cast<int16_t>(prop_index(t, hot_name(static_cast<Hot>(k))));
      info.defaults.reserve(t.props.size());
      for (size_t i = 0; i < t.props.size(); ++i) {
        info.defaults.push_back(default_value(t.props[i]));
        const int slot = elevation_slot(t.props[i].name);
        if (slot >= 0) info.elevation[slot] = static_cast<int>(i);
      }
      out.emplace(&t, std::move(info));
    }
    return out;
  }();
  auto it = infos.find(&type);
  DREAMSY_CHECK(it != infos.end(), "type_info: type is not from the catalog");
  return it->second;
}

// Only for types whose catalog has the prop: `z` on a Camera2D is not a prop (and Vec3 `.z`
// is not an entity prop at all), so those keys fall through to the normal "no property" path.
const double* elevation_field(const Entity& e, std::string_view key) {
  const int slot = elevation_slot(key);
  if (slot < 0 || info_of(e).elevation[slot] < 0) return nullptr;
  return elevation_member(e, slot);
}

double* elevation_field(Entity& e, std::string_view key) {  // same as above, for writing
  return const_cast<double*>(elevation_field(static_cast<const Entity&>(e), key));
}

bool parse_color(std::string_view text, double out[4]) {
  std::string_view h = pkg::trim(text);
  if (!h.empty() && h[0] == '#') h.remove_prefix(1);
  std::string hex(h);
  if (hex.size() == 3) hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
  if (hex.size() != 6 && hex.size() != 8) return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (char c : hex)
    if (digit(c) < 0) return false;
  auto byte = [&](size_t i) { return (digit(hex[i]) * 16 + digit(hex[i + 1])) / 255.0; };
  out[0] = byte(0), out[1] = byte(2), out[2] = byte(4);
  out[3] = hex.size() == 8 ? byte(6) : 1;
  return true;
}

double js_round(double x) {
  // Math.round: the nearest integer, halves toward +infinity. floor(x + 0.5) is wrong for
  // 0.49999999999999994, so compare against the floor instead.
  if (!std::isfinite(x)) return x;
  double r = std::floor(x);
  if (x - r >= 0.5) r += 1;
  return r;
}

std::string js_string(const Value& v) {
  switch (v.kind) {
    case ValueKind::Number: return pkg::number_text(v.v[0]);
    case ValueKind::Bool: return v.b ? "true" : "false";
    case ValueKind::String: return v.s;
    case ValueKind::Vec: {
      std::string out;
      for (int i = 0; i < v.n; ++i) out += (i ? "," : "") + pkg::number_text(v.v[i]);
      return out;
    }
  }
  return {};
}

int prop_index(const pkg::EntityType& type, std::string_view key) {
  // Reject on length and first letter inline: string == string_view calls memcmp for every prop.
  if (key.empty()) return -1;
  for (size_t i = 0; i < type.props.size(); ++i) {
    const std::string& name = type.props[i].name;
    if (name.size() == key.size() && name[0] == key[0] && std::memcmp(name.data(), key.data(), key.size()) == 0)
      return static_cast<int>(i);
  }
  return -1;
}

std::optional<Value> default_value(const pkg::PropDef& def) {
  if (!def.default_value) return std::nullopt;
  const pkg::PropValue& d = *def.default_value;
  switch (def.kind) {
    case pkg::PropKind::Number:
    case pkg::PropKind::Int: return Value::number(d.num[0]);
    case pkg::PropKind::Bool: return Value::boolean(d.flag);
    case pkg::PropKind::Vec2: return Value::vec(d.num[0], d.num[1]);
    case pkg::PropKind::Vec3: return Value::vec3(d.num[0], d.num[1], d.num[2]);
    case pkg::PropKind::Color: {
      double c[4];
      if (!parse_color(d.text, c)) return std::nullopt;
      return Value::color(c[0], c[1], c[2], c[3]);
    }
    case pkg::PropKind::String:
    case pkg::PropKind::Enum:
    case pkg::PropKind::Asset:
    case pkg::PropKind::Entity: return Value::string(d.text);
  }
  return std::nullopt;
}

std::string_view prop_alias(const Entity& e, std::string_view key) {
  return key == "color" && e.type->name == "Sprite2D" ? std::string_view("modulate") : key;
}

// ================================================================ entities

bool Entity::has_tag(std::string_view tag) const { return std::find(tags.begin(), tags.end(), tag) != tags.end(); }

void Entity::add_tag(const std::string& tag) {
  if (!has_tag(tag)) tags.push_back(tag);
}

void Entity::remove_tag(std::string_view tag) {
  auto it = std::find(tags.begin(), tags.end(), tag);
  if (it != tags.end()) tags.erase(it);
}

// ================================================================ transforms

Mat2D mul(const Mat2D& m, const Mat2D& n) {
  return {m.a * n.a + m.c * n.b,         m.b * n.a + m.d * n.b,         m.a * n.c + m.c * n.d,
          m.b * n.c + m.d * n.d,         m.a * n.tx + m.c * n.ty + m.tx, m.b * n.tx + m.d * n.ty + m.ty};
}

Mat2D invert(const Mat2D& m) {
  double det = m.a * m.d - m.b * m.c;
  if (det == 0) det = 1e-12;  // JS: `|| 1e-12`
  const double a = m.d / det, b = -m.b / det, c = -m.c / det, d = m.a / det;
  return {a, b, c, d, -(a * m.tx + c * m.ty), -(b * m.tx + d * m.ty)};
}

Point apply(const Mat2D& m, double x, double y) { return {m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty}; }

double rotation_of(const Mat2D& m) { return std::atan2(m.b, m.a); }

Mat2D World::local(const Entity& e) const {
  if (index(e, Hot::Position) < 0 || !e.is_2d()) return {};
  const Point p = vec(e, Hot::Position);
  const double r = num(e, Hot::Rotation) * M_PI / 180;
  const Value* sv = peek(e, Hot::Scale);
  const double sx = sv ? sv->v[0] : 1, sy = sv ? sv->v[1] : 1;
  const double cos = std::cos(r), sin = std::sin(r);
  return {cos * sx, sin * sx, -sin * sy, cos * sy, p.x, p.y};
}

Mat2D World::global(EntityId id) const {
  Mat2D m;
  for (const Entity* x = get(id); x; x = get(x->parent)) m = mul(local(*x), m);
  return m;
}

// ================================================================ the tree

Result<EntityId> World::create(std::string_view id_in, std::string_view type_name, std::optional<std::string> name,
                               std::vector<std::string> scripts) {
  const pkg::EntityType* type = pkg::Catalog::get().type(type_name);
  if (!type) return Error{"unknown entity type \"" + std::string(type_name) + "\""};
  std::string id(id_in);
  if (id.empty()) {
    do id = "rt_" + std::to_string(next_auto_++);
    while (by_id_.count(id));
  } else if (by_id_.count(id)) {
    return Error{"duplicate entity id \"" + id + "\""};
  }
  Entity e;
  e.handle = next_handle_++;
  e.id = id;
  e.type = type;
  e.info = &type_info(*type);
  e.name = name.value_or(id);
  e.scripts = std::move(scripts);
  e.props.resize(type->props.size());
  const uint32_t handle = e.handle;
  EntityId eid = pool_.insert(std::move(e));
  by_handle_[handle] = eid;
  by_id_[id] = eid;
  return eid;
}

void World::attach(EntityId child_id, EntityId parent_id, int index) {
  Entity* child = get(child_id);
  if (!child) return;
  order_stale_ = true;
  if (Entity* old = get(child->parent)) {
    auto it = std::find(old->children.begin(), old->children.end(), child_id);
    if (it != old->children.end()) old->children.erase(it);
  }
  child->parent = parent_id;
  if (Entity* p = get(parent_id)) {
    if (index < 0 || index > static_cast<int>(p->children.size())) p->children.push_back(child_id);
    else p->children.insert(p->children.begin() + index, child_id);
  } else {
    child->parent = EntityId{};
  }
}

std::vector<EntityId> World::remove(EntityId e) {
  std::vector<EntityId> out;
  // Children first, like engine-web.
  std::function<void(EntityId)> collect = [&](EntityId x) {
    const Entity* ent = get(x);
    if (!ent) return;
    std::vector<EntityId> kids = ent->children;
    for (EntityId c : kids) collect(c);
    out.push_back(x);
  };
  collect(e);
  if (out.empty()) return out;
  order_stale_ = true;
  attach(e, EntityId{});
  for (EntityId x : out) {
    const Entity* ent = get(x);
    by_id_.erase(ent->id);
    by_handle_.erase(ent->handle);
    pool_.remove(x);
  }
  if (e == root_) root_ = EntityId{};
  return out;
}

void World::clear() {
  if (alive(root_)) remove(root_);
  // Detached entities (created but never attached) go too.
  pool_.clear();
  by_id_.clear();
  by_handle_.clear();
  root_ = EntityId{};
  order_stale_ = true;
}

std::vector<EntityId> World::load_scene(const pkg::SceneDef& scene, const Warn& warn) {
  std::vector<EntityId> created;
  struct Ref {
    EntityId entity;
    std::string key, target;
  };
  std::vector<Ref> refs;
  std::function<void(const std::string&, EntityId)> build = [&](const std::string& id, EntityId parent) {
    const pkg::EntityDef* src = scene.entity(id);
    if (!src) {
      warn("missing entity \"" + id + "\"");
      return;
    }
    auto made = create(id, src->type, src->name, src->scripts);
    if (!made) {
      warn(id + ": " + made.error());
      return;
    }
    EntityId eid = made.value();
    Entity& e = *get(eid);
    for (const std::string& t : src->tags) e.add_tag(t);
    for (const auto& [key, pv] : src->props) {
      int index = prop_index(*e.type, key);
      if (index < 0) {
        warn(e.id + ": unknown prop \"" + key + "\" on " + e.type->name);
        continue;
      }
      const pkg::PropDef& def = e.type->props[static_cast<size_t>(index)];
      if (def.kind == pkg::PropKind::Entity) {
        refs.push_back({eid, key, pv.text});
        continue;
      }
      // Scene values were validated by the loader; coerce() normalizes them (colors to RGBA).
      Value raw;
      switch (pv.kind) {
        case pkg::PropKind::Number:
        case pkg::PropKind::Int: raw = Value::number(pv.num[0]); break;
        case pkg::PropKind::Bool: raw = Value::boolean(pv.flag); break;
        case pkg::PropKind::Vec2: raw = Value::vec(pv.num[0], pv.num[1]); break;
        case pkg::PropKind::Vec3: raw = Value::vec3(pv.num[0], pv.num[1], pv.num[2]); break;
        default: raw = Value::string(pv.text); break;
      }
      auto v = coerce(e, key, raw);
      if (!v) {
        warn(e.id + ": " + v.error());
        continue;
      }
      set_raw(e, prop_alias(e, key), std::move(v).value());
    }
    if (alive(parent)) {
      attach(eid, parent);
    } else {
      root_ = eid;
      order_stale_ = true;
    }
    created.push_back(eid);
    std::vector<std::string> kids = src->children;
    for (const std::string& c : kids) build(c, eid);
  };
  build(scene.root, EntityId{});
  for (const Ref& r : refs) {
    Entity* e = get(r.entity);
    EntityId target = by_id(r.target);
    if (e && alive(target)) e->props[static_cast<size_t>(prop_index(*e->type, r.key))] = Value::string(get(target)->id);
    else if (e) warn(e->id + "." + r.key + ": no entity \"" + r.target + "\"");
  }
  return created;
}

EntityId World::by_handle(double handle) const {
  if (!(handle >= 1 && handle <= 4294967295.0) || handle != std::floor(handle)) return {};
  auto it = by_handle_.find(static_cast<uint32_t>(handle));
  return it == by_handle_.end() ? EntityId{} : it->second;
}

EntityId World::by_id(std::string_view id) const {
  auto it = by_id_.find(std::string(id));
  return it == by_id_.end() ? EntityId{} : it->second;
}

std::vector<EntityId> World::walk(EntityId from) const {
  std::vector<EntityId> out;
  if (!alive(from)) return out;
  // Iterative pre-order walk (deep trees can't overflow the C++ stack).
  std::vector<EntityId> stack{from};
  while (!stack.empty()) {
    EntityId id = stack.back();
    stack.pop_back();
    const Entity* e = get(id);
    if (!e) continue;
    out.push_back(id);
    for (auto it = e->children.rbegin(); it != e->children.rend(); ++it) stack.push_back(*it);
  }
  return out;
}

const std::vector<EntityId>& World::order() const {
  if (order_stale_) {
    order_ = walk(root_);
    order_stale_ = false;
  }
  return order_;
}

std::vector<EntityId> World::tagged(std::string_view tag) const {
  std::vector<EntityId> out;
  for (EntityId id : order())
    if (get(id)->has_tag(tag)) out.push_back(id);
  return out;
}

// ================================================================ props

std::optional<Value> World::get_prop(const Entity& e, std::string_view key) const {
  const int i = prop_index(*e.type, key);
  if (i < 0) return std::nullopt;
  if (const double* f = elevation_at(e, i)) return Value::number(*f);
  if (const Value* v = peek(e, i)) return *v;
  return std::nullopt;
}

const Value* World::peek(const Entity& e, int i) const {
  const std::optional<Value>& set = e.props[static_cast<size_t>(i)];
  if (set) return &*set;
  const std::optional<Value>& d = info_of(e).defaults[static_cast<size_t>(i)];
  return d ? &*d : nullptr;
}

double World::num(const Entity& e, std::string_view key) const {
  const int i = prop_index(*e.type, key);
  if (i < 0) return 0;
  if (const double* f = elevation_at(e, i)) return *f;
  const Value* v = peek(e, i);
  return v && v->kind == ValueKind::Number ? v->v[0] : 0;
}

bool World::flag(const Entity& e, std::string_view key) const {
  const int i = prop_index(*e.type, key);
  if (i < 0 || elevation_at(e, i)) return false;
  const Value* v = peek(e, i);
  return v && v->kind == ValueKind::Bool && v->b;
}

std::string World::str(const Entity& e, std::string_view key) const { return std::string(str_view(e, key)); }

std::string_view World::str_view(const Entity& e, std::string_view key) const {
  const int i = prop_index(*e.type, key);
  if (i < 0 || elevation_at(e, i)) return {};
  const Value* v = peek(e, i);
  return v && v->kind == ValueKind::String ? std::string_view(v->s) : std::string_view();
}

Point World::vec(const Entity& e, std::string_view key) const {
  const int i = prop_index(*e.type, key);
  if (i < 0 || elevation_at(e, i)) return {};
  const Value* v = peek(e, i);
  if (!v || v->kind != ValueKind::Vec) return {};
  return {v->v[0], v->v[1]};
}

bool World::visible(const Entity& e) const {
  const Value* v = peek(e, Hot::Visible);
  return !(v && v->kind == ValueKind::Bool && !v->b);
}

int World::index(const Entity& e, Hot key) { return info_of(e).hot[static_cast<int>(key)]; }
const Value* World::peek(const Entity& e, Hot key) const {
  const int i = index(e, key);
  return i < 0 ? nullptr : peek(e, i);
}
double World::num(const Entity& e, Hot key) const {
  const Value* v = peek(e, key);
  return v && v->kind == ValueKind::Number ? v->v[0] : 0;
}
bool World::flag(const Entity& e, Hot key) const {
  const Value* v = peek(e, key);
  return v && v->kind == ValueKind::Bool && v->b;
}
std::string_view World::str_view(const Entity& e, Hot key) const {
  const Value* v = peek(e, key);
  return v && v->kind == ValueKind::String ? std::string_view(v->s) : std::string_view();
}
Point World::vec(const Entity& e, Hot key) const {
  const Value* v = peek(e, key);
  if (!v || v->kind != ValueKind::Vec) return {};
  return {v->v[0], v->v[1]};
}

void World::set_raw(Entity& e, std::string_view key, Value v) {
  int i = prop_index(*e.type, key);
  DREAMSY_CHECK(i >= 0, "set_raw: no such prop");
  if (double* f = elevation_field(e, key)) {
    *f = v.kind == ValueKind::Number ? v.v[0] : 0;
    return;
  }
  e.props[static_cast<size_t>(i)] = std::move(v);
}

void World::unset(Entity& e, std::string_view key) {
  int i = prop_index(*e.type, key);
  if (i < 0) return;
  if (double* f = elevation_field(e, key)) {
    // Back to the catalog default (z_gravity_scale 1, the others 0).
    std::optional<Value> d = default_value(e.type->props[static_cast<size_t>(i)]);
    *f = d && d->kind == ValueKind::Number ? d->v[0] : 0;
    return;
  }
  e.props[static_cast<size_t>(i)].reset();
}

namespace {

// engine-web describe(): how a bad value is named in an error.
std::string describe(const Value& v) {
  if (v.kind == ValueKind::Vec) return v.n == 2 ? "Vec2" : v.n == 3 ? "Vec3" : "Color";
  if (v.kind == ValueKind::String) return "\"" + v.s + "\"";
  return js_string(v);
}

bool is_asset_ref(std::string_view s) {
  if (!pkg::starts_with(s, "asset:")) return false;
  std::string_view id = s.substr(6);
  if (id.empty() || !pkg::is_lower(id[0])) return false;
  return std::all_of(id.begin(), id.end(), [](char c) { return pkg::is_lower(c) || pkg::is_digit(c) || c == '_'; });
}

}  // namespace

Result<Value> World::coerce(const Entity& e, std::string_view key, const Value& value) const {
  const pkg::PropDef* def = e.type->prop(key);
  if (!def) return Error{e.type->name + " \"" + e.id + "\" has no property \"" + std::string(key) + "\""};
  auto bad = [&](const std::string& want) {
    return Error{e.type->name + "." + std::string(key) + " expects " + want + ", got " + describe(value)};
  };
  switch (def->kind) {
    case pkg::PropKind::Number:
    case pkg::PropKind::Int: {
      if (value.kind != ValueKind::Number || std::isnan(value.v[0])) return bad("a number");
      double n = def->kind == pkg::PropKind::Int ? js_round(value.v[0]) : value.v[0];
      if (def->min && n < *def->min) n = *def->min;
      if (def->max && n > *def->max) n = *def->max;
      return Value::number(n);
    }
    case pkg::PropKind::Bool:
      if (value.kind != ValueKind::Bool) return bad("true/false");
      return value;
    case pkg::PropKind::String:
      if (value.kind == ValueKind::Number) return Value::string(js_string(value));
      if (value.kind != ValueKind::String) return bad("a string");
      return value;
    case pkg::PropKind::Vec2:
      if (value.kind != ValueKind::Vec || value.n < 2) return bad("a Vec2 (use vec2(x, y))");
      return Value::vec(value.v[0], value.v[1]);
    case pkg::PropKind::Vec3:
      if (value.kind != ValueKind::Vec || value.n < 3) return bad("a Vec3 (use vec3(x, y, z))");
      return Value::vec3(value.v[0], value.v[1], value.v[2]);
    case pkg::PropKind::Color: {
      if (value.kind == ValueKind::Vec && value.n >= 3) return Value::color(value.v[0], value.v[1], value.v[2], value.n > 3 ? value.v[3] : 1);
      double c[4];
      if (value.kind != ValueKind::String || !parse_color(value.s, c)) return bad("a Color or \"#rrggbb\"");
      return Value::color(c[0], c[1], c[2], c[3]);
    }
    case pkg::PropKind::Enum: {
      bool ok = value.kind == ValueKind::String && std::find(def->values.begin(), def->values.end(), value.s) != def->values.end();
      if (!ok) {
        std::vector<std::string> quoted;
        for (const std::string& v : def->values) quoted.push_back("\"" + v + "\"");
        return bad("one of " + pkg::join(quoted, ", "));
      }
      return value;
    }
    case pkg::PropKind::Asset:
      if (value.kind != ValueKind::String || !is_asset_ref(value.s)) return bad("\"asset:<id>\"");
      return value;
    case pkg::PropKind::Entity: {
      if (value.kind == ValueKind::String || value.kind == ValueKind::Number) {
        EntityId target = value.kind == ValueKind::String ? by_id(value.s) : by_handle(value.v[0]);
        if (!alive(target)) return Error{e.type->name + "." + std::string(key) + ": no entity \"" + js_string(value) + "\""};
        return Value::string(get(target)->id);
      }
      return bad("an Entity or entity id");
    }
  }
  return bad("a value");
}

// ================================================================ state hash

void World::hash_into(Hasher& h) const {
  h.add(static_cast<uint32_t>(view));
  for (EntityId id : order()) {
    const Entity& e = *get(id);
    h.add(e.handle);
    h.str(e.id);
    h.str(e.type->name);
    h.str(e.name);
    h.add(static_cast<uint32_t>(e.children.size()));
    h.add(static_cast<uint32_t>(e.tags.size()));
    for (const std::string& t : e.tags) h.str(t);
    for (size_t i = 0; i < e.props.size(); ++i) {
      const std::optional<Value>& v = e.props[i];
      if (!v) continue;
      h.add(static_cast<uint32_t>(i));
      h.add(static_cast<uint8_t>(v->kind));
      switch (v->kind) {
        case ValueKind::Number: h.add(v->v[0]); break;
        case ValueKind::Bool: h.add(static_cast<uint8_t>(v->b)); break;
        case ValueKind::String: h.str(v->s); break;
        case ValueKind::Vec:
          for (int k = 0; k < v->n; ++k) h.add(v->v[k]);
          break;
      }
    }
    h.add(e.z);
    h.add(e.height);
    h.add(e.z_velocity);
    if (e.tiles) {
      for (int r = 0; r < e.tiles->rows(); ++r)
        for (int c = 0; c < e.tiles->cols(); ++c) h.add(e.tiles->cell(c, r));
    }
    if (e.emitter) {
      h.add(e.emitter->count());
      for (int i = 0; i < e.emitter->count(); ++i) {
        h.add(e.emitter->x(i));
        h.add(e.emitter->y(i));
      }
    }
    h.str(e.anim_name);
    h.add(e.anim_t);
  }
}

}  // namespace dreamsy::world
