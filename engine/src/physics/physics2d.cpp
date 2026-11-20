#include "physics/physics2d.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numbers>

#include "pkg/text.h"

namespace dreamsy::physics {

using world::Entity;
using world::EntityId;
using world::Point;

namespace {

constexpr double kPi = std::numbers::pi;

float to_m(double px) { return static_cast<float>(px / kPixelsPerMeter); }
double to_px(float m) { return static_cast<double>(m) * kPixelsPerMeter; }

// Rapier's default coefficient combine rule is the average (Box2D's default differs).
float average_friction(float a, uint64_t, float b, uint64_t) { return 0.5f * (a + b); }
float average_restitution(float a, uint64_t, float b, uint64_t) { return 0.5f * (a + b); }

// A rotation from an angle with the engine's deterministic sin/cos (Box2D's b2MakeRot uses an
// approximation; we want the angle a script set to be the angle the body gets).
b2Rot make_rot(double radians) { return b2Rot{static_cast<float>(std::cos(radians)), static_cast<float>(std::sin(radians))}; }
double angle_of(b2Rot r) { return std::atan2(static_cast<double>(r.s), static_cast<double>(r.c)); }

void* handle_data(uint32_t handle) { return reinterpret_cast<void*>(static_cast<uintptr_t>(handle)); }
uint32_t data_handle(void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

}  // namespace

Physics2D::Physics2D(world::World& world, double dt, std::vector<std::string> layers)
    : world_(world), layers_(std::move(layers)) {
  (void)dt;  // the step size is passed to step() (it follows game.timeScale)
  b2WorldDef def = b2DefaultWorldDef();
  const double gy = world_.view == world::ViewMode::Side ? kGravity : 0;
  def.gravity = b2Vec2{0.0f, to_m(gy)};
  def.frictionCallback = average_friction;
  def.restitutionCallback = average_restitution;
  def.workerCount = 1;  // single-threaded: deterministic event order
  b2world_ = b2CreateWorld(&def);
  b2World_SetPreSolveCallback(b2world_, &Physics2D::pre_solve, &Physics2D::pre_continuous, this);
}

Physics2D::~Physics2D() { b2DestroyWorld(b2world_); }

bool Physics2D::is_physical(const Entity& e) {
  const std::string& t = e.type->name;
  return t == "Body2D" || t == "Area2D" || t == "TileMap2D";
}

Result<Groups> Physics2D::groups(const Entity& e) const {
  auto bits = [&](const std::string& value, const char* key) -> Result<uint32_t> {
    std::vector<std::string> names;
    for (std::string_view part : pkg::split(value, ','))
      if (!pkg::trim(part).empty()) names.emplace_back(pkg::trim(part));
    if (names.empty()) return uint32_t{0xffff};
    uint32_t b = 0;
    for (const std::string& n : names) {
      auto it = std::find(layers_.begin(), layers_.end(), n);
      if (it == layers_.end()) {
        return Error{e.type->name + " \"" + e.id + "\"." + key + ": unknown collision layer \"" + n + "\"" +
                     (layers_.empty() ? std::string(" (declare [physics] layers in dreamsy.toml)")
                                      : " (layers: " + pkg::join(layers_, ", ") + ")")};
      }
      b |= 1u << (it - layers_.begin());
    }
    return b;
  };
  auto member = bits(world_.str(e, "layer"), "layer");
  if (!member) return Error{member.error()};
  auto filter = bits(world_.str(e, "mask"), "mask");
  if (!filter) return Error{filter.error()};
  return Groups{member.value(), filter.value()};
}

Groups Physics2D::safe_groups(const Entity& e) const {
  auto g = groups(e);
  return g ? g.value() : Groups{0xffff, 0xffff};
}

b2Filter Physics2D::filter_of(const Entity& e) const {
  Groups g = safe_groups(e);
  b2Filter f = b2DefaultFilter();
  f.categoryBits = g.member;
  f.maskBits = g.filter;
  return f;
}

// ================================================================ bookkeeping

Physics2D::BodyRec* Physics2D::find(uint32_t handle) {
  auto it = by_handle_.find(handle);
  return it == by_handle_.end() ? nullptr : &recs_.at(it->second);
}

const Physics2D::BodyRec* Physics2D::find(uint32_t handle) const {
  auto it = by_handle_.find(handle);
  return it == by_handle_.end() ? nullptr : &recs_.at(it->second);
}

Physics2D::BodyRec* Physics2D::find_shape(b2ShapeId shape) {
  if (!b2Shape_IsValid(shape)) return nullptr;
  BodyRec* r = find(data_handle(b2Shape_GetUserData(shape)));
  if (!r) return nullptr;
  // The handle must belong to this exact shape (not a rebuilt body's predecessor).
  for (b2ShapeId s : r->shapes)
    if (B2_ID_EQUALS(s, shape)) return r;
  return nullptr;
}

const Entity* Physics2D::entity_of_shape(b2ShapeId s) const {
  if (!b2Shape_IsValid(s)) return nullptr;
  return world_.get(world_.by_handle(data_handle(b2Shape_GetUserData(s))));
}

void Physics2D::insert(BodyRec rec) {
  const int64_t seq = next_seq_++;
  by_handle_[rec.handle] = seq;
  recs_.emplace(seq, std::move(rec));
}

void Physics2D::remove_rec(uint32_t handle) {
  auto it = by_handle_.find(handle);
  if (it == by_handle_.end()) return;
  BodyRec& r = recs_.at(it->second);
  b2DestroyBody(r.body);
  recs_.erase(it->second);
  by_handle_.erase(it);
}

void Physics2D::mark_dirty(EntityId id) {
  const Entity* e = world_.get(id);
  if (!e || !is_physical(*e)) return;
  if (std::find(dirty_.begin(), dirty_.end(), id) == dirty_.end()) dirty_.push_back(id);
}

Physics2D::BodyRec* Physics2D::rec(EntityId id) {
  if (std::find(dirty_.begin(), dirty_.end(), id) != dirty_.end()) build(id);
  const Entity* e = world_.get(id);
  return e ? find(e->handle) : nullptr;
}

void Physics2D::flush() {
  std::vector<EntityId> list = dirty_;
  for (EntityId id : list) build(id);
}

void Physics2D::remove(EntityId id) {
  std::erase(dirty_, id);
  if (const Entity* e = world_.get(id)) remove_rec(e->handle);
}

// ================================================================ building bodies

void Physics2D::build(EntityId id) {
  std::erase(dirty_, id);
  Entity* e = world_.get(id);
  if (!e) return;
  std::optional<b2Vec2> old_velocity;
  bool existed = false;
  if (BodyRec* old = find(e->handle)) {
    existed = true;
    old_velocity = b2Body_GetLinearVelocity(old->body);
    remove_rec(e->handle);
  }
  if (e->type->name == "TileMap2D") {
    build_tiles(id, existed);
    return;
  }
  const bool is_area = e->type->name == "Area2D";
  Kind kind = Kind::Area;
  if (!is_area) {
    std::string body = world_.str(*e, "body");
    kind = body == "dynamic" ? Kind::Dynamic : body == "kinematic" ? Kind::Kinematic : Kind::Static;
  }
  const world::Mat2D m = world_.global(id);

  b2BodyDef bd = b2DefaultBodyDef();
  bd.type = kind == Kind::Dynamic ? b2_dynamicBody : (kind == Kind::Kinematic || kind == Kind::Area) ? b2_kinematicBody : b2_staticBody;
  bd.position = b2Vec2{to_m(m.tx), to_m(m.ty)};
  bd.rotation = make_rot(world::rotation_of(m));
  bd.userData = handle_data(e->handle);
  if (!is_area) {
    bd.gravityScale = static_cast<float>(world_.num(*e, "gravity_scale"));
    if (world_.flag(*e, "lock_rotation")) bd.motionLocks.angularZ = true;
  }
  // 2.5D: contacts between bodies with a height are re-checked every step (physics2d.h).
  bd.enableContactRecycling = e->height <= 0;
  b2BodyId body = b2CreateBody(b2world_, &bd);

  const double sx = std::hypot(m.a, m.b) != 0 ? std::hypot(m.a, m.b) : 1;
  const double sy = std::hypot(m.c, m.d) != 0 ? std::hypot(m.c, m.d) : 1;
  const Point size = world_.vec(*e, "size");
  const double w = std::max(0.5, size.x * sx), h = std::max(0.5, size.y * sy);
  const std::string shape = world_.str(*e, "shape");

  b2ShapeDef sd = b2DefaultShapeDef();
  sd.userData = handle_data(e->handle);
  sd.filter = filter_of(*e);
  if (is_area) {
    sd.isSensor = true;  // never pushes; overlaps are found by update_areas()
  } else {
    sd.enableContactEvents = true;
    sd.enablePreSolveEvents = true;  // z ranges (pre_solve)
    sd.material.friction = static_cast<float>(world_.num(*e, "friction"));
    sd.material.restitution = static_cast<float>(world_.num(*e, "bounce"));
  }
  // Rapier was given a mass; Box2D takes a density, so divide by the shape's area (m²).
  double area_m2 = 1;
  b2ShapeId sid{};
  if (shape == "circle") {
    const double r = w / 2;
    area_m2 = kPi * (r / kPixelsPerMeter) * (r / kPixelsPerMeter);
    if (kind == Kind::Dynamic) sd.density = static_cast<float>(world_.num(*e, "mass") / area_m2);
    b2Circle c{b2Vec2{0, 0}, to_m(r)};
    sid = b2CreateCircleShape(body, &sd, &c);
  } else if (shape == "capsule") {
    const double r = w / 2, half = std::max(0.01, h / 2 - r);
    area_m2 = (2 * r * 2 * half + kPi * r * r) / (kPixelsPerMeter * kPixelsPerMeter);
    if (kind == Kind::Dynamic) sd.density = static_cast<float>(world_.num(*e, "mass") / area_m2);
    b2Capsule c{b2Vec2{0, -to_m(half)}, b2Vec2{0, to_m(half)}, to_m(r)};
    sid = b2CreateCapsuleShape(body, &sd, &c);
  } else {
    area_m2 = (w / kPixelsPerMeter) * (h / kPixelsPerMeter);
    if (kind == Kind::Dynamic) sd.density = static_cast<float>(world_.num(*e, "mass") / area_m2);
    b2Polygon box = b2MakeBox(to_m(w / 2), to_m(h / 2));
    sid = b2CreatePolygonShape(body, &sd, &box);
  }

  BodyRec r;
  r.entity = id;
  r.handle = e->handle;
  r.body = body;
  r.shapes = {sid};
  r.kind = kind;
  insert(std::move(r));

  std::optional<b2Vec2> v = old_velocity;
  if (e->pending_velocity) v = b2Vec2{to_m(e->pending_velocity->first), to_m(e->pending_velocity->second)};
  e->pending_velocity.reset();
  if (v && kind != Kind::Static && kind != Kind::Area) b2Body_SetLinearVelocity(body, *v);
  apply_enabled(id);
}

void Physics2D::build_tiles(EntityId id, bool rebuilt) {
  Entity* e = world_.get(id);
  if (rebuilt) {
    for (auto it = tile_pairs_.begin(); it != tile_pairs_.end();) {
      const std::pair<uint32_t, uint32_t> key = it->first;
      const int n = it->second;
      if (n > 0 && key.first == e->handle) {
        EntityId other = world_.by_handle(key.second);
        bool known = std::any_of(stale_tile_pairs_.begin(), stale_tile_pairs_.end(), [&](const auto& p) { return p.first == key; });
        if (find(key.second) && !known) stale_tile_pairs_.push_back({key, {id, other}});
      }
      if (key.first == e->handle || key.second == e->handle) it = tile_pairs_.erase(it);
      else ++it;
    }
  }
  const world::TileGrid* grid = e->tiles.get();
  if (!grid || !world_.flag(*e, "collision")) return;
  std::vector<world::TileRect> rects = grid->solid_rects();
  if (rects.empty()) return;
  const world::Mat2D m = world_.global(id);
  b2BodyDef bd = b2DefaultBodyDef();
  bd.type = b2_staticBody;
  bd.position = b2Vec2{to_m(m.tx), to_m(m.ty)};
  bd.rotation = make_rot(world::rotation_of(m));
  bd.userData = handle_data(e->handle);
  bd.enableContactRecycling = e->height <= 0;  // see the 2.5D notes in physics2d.h
  b2BodyId body = b2CreateBody(b2world_, &bd);
  const double sx = std::hypot(m.a, m.b) != 0 ? std::hypot(m.a, m.b) : 1;
  const double sy = std::hypot(m.c, m.d) != 0 ? std::hypot(m.c, m.d) : 1;
  const double ts = world_.num(*e, "tile_size");
  BodyRec r;
  r.entity = id;
  r.handle = e->handle;
  r.body = body;
  r.kind = Kind::Tiles;
  for (const world::TileRect& t : rects) {
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.userData = handle_data(e->handle);
    sd.filter = filter_of(*e);
    sd.material.friction = 0.5f;
    sd.material.restitution = 0;
    sd.enableContactEvents = true;
    sd.enablePreSolveEvents = true;
    sd.invokeContactCreation = true;
    b2Polygon box = b2MakeOffsetBox(to_m(t.w * ts * sx / 2), to_m(t.h * ts * sy / 2),
                                    b2Vec2{to_m((t.col + t.w / 2.0) * ts * sx), to_m((t.row + t.h / 2.0) * ts * sy)}, b2Rot_identity);
    r.shapes.push_back(b2CreatePolygonShape(body, &sd, &box));
  }
  insert(std::move(r));
  apply_enabled(id);
}

int Physics2D::collider_count(EntityId id) const {
  const Entity* e = world_.get(id);
  const BodyRec* r = e ? find(e->handle) : nullptr;
  return r ? static_cast<int>(r->shapes.size()) : 0;
}

// ================================================================ queries and commands

void Physics2D::apply_enabled(EntityId id) {
  const Entity* e = world_.get(id);
  BodyRec* r = e ? find(e->handle) : nullptr;
  if (!r) return;
  const bool on = world_.visible(*e);
  if (on && !b2Body_IsEnabled(r->body)) b2Body_Enable(r->body);
  else if (!on && b2Body_IsEnabled(r->body)) b2Body_Disable(r->body);
}

void Physics2D::sync_transform(EntityId id) {
  if (std::find(dirty_.begin(), dirty_.end(), id) != dirty_.end()) return;
  const Entity* e = world_.get(id);
  BodyRec* r = e ? find(e->handle) : nullptr;
  if (!r) return;
  const world::Mat2D m = world_.global(id);
  // Rapier teleported areas at the next step and other bodies now; Box2D teleports both now
  // (area overlaps are evaluated at the end of the step, so the effect is the same).
  b2Body_SetTransform(r->body, b2Vec2{to_m(m.tx), to_m(m.ty)}, make_rot(world::rotation_of(m)));
  if (r->kind == Kind::Dynamic || r->kind == Kind::Kinematic) b2Body_SetAwake(r->body, true);
}

Point Physics2D::velocity(EntityId id) {
  BodyRec* r = rec(id);
  if (!r) {
    const Entity* e = world_.get(id);
    if (e && e->pending_velocity) return {e->pending_velocity->first, e->pending_velocity->second};
    return {0, 0};
  }
  b2Vec2 v = b2Body_GetLinearVelocity(r->body);
  return {to_px(v.x), to_px(v.y)};
}

void Physics2D::set_velocity(EntityId id, double vx, double vy) {
  BodyRec* r = rec(id);
  if (!r) {
    if (Entity* e = world_.get(id)) e->pending_velocity = std::make_pair(vx, vy);
    return;
  }
  if (r->kind == Kind::Dynamic || r->kind == Kind::Kinematic) {
    b2Body_SetLinearVelocity(r->body, b2Vec2{to_m(vx), to_m(vy)});
    b2Body_SetAwake(r->body, true);
  }
}

void Physics2D::apply_impulse(EntityId id, double ix, double iy) {
  BodyRec* r = rec(id);
  // kg·px/s -> kg·m/s
  if (r && r->kind == Kind::Dynamic) b2Body_ApplyLinearImpulseToCenter(r->body, b2Vec2{to_m(ix), to_m(iy)}, true);
}

bool Physics2D::on_floor(EntityId id) {
  BodyRec* r = rec(id);
  if (!r || r->kind == Kind::Area) return false;
  const int cap = b2Body_GetContactCapacity(r->body);
  if (cap <= 0) return false;
  std::vector<b2ContactData> contacts(static_cast<size_t>(cap));
  const int n = b2Body_GetContactData(r->body, contacts.data(), cap);
  const float near = to_m(0.5);  // ignore contacts that are only predicted, not touching
  for (int i = 0; i < n; ++i) {
    const b2ContactData& c = contacts[static_cast<size_t>(i)];
    const b2Manifold& mf = c.manifold;
    if (mf.pointCount == 0) continue;
    const bool we_are_a = B2_ID_EQUALS(b2Shape_GetBody(c.shapeIdA), r->body);
    b2ShapeId other = we_are_a ? c.shapeIdB : c.shapeIdA;
    if (b2Shape_IsSensor(other)) continue;
    // The normal points from shape A to shape B; we want it pointing away from this body.
    const float ny = we_are_a ? mf.normal.y : -mf.normal.y;
    if (ny <= 0.6f) continue;
    for (int k = 0; k < mf.pointCount; ++k)
      if (mf.points[k].separation < near) return true;
  }
  return false;
}

std::optional<Aabb> Physics2D::aabb(EntityId id) {
  const Entity* e = world_.get(id);
  BodyRec* r = e ? find(e->handle) : nullptr;
  if (!r || r->kind == Kind::Tiles || !b2Body_IsEnabled(r->body)) return std::nullopt;
  const b2Vec2 p = b2Body_GetPosition(r->body);
  const double tx = to_px(p.x), ty = to_px(p.y);
  const world::Mat2D m = world_.global(id);
  const double sx = std::hypot(m.a, m.b) != 0 ? std::hypot(m.a, m.b) : 1;
  const double sy = std::hypot(m.c, m.d) != 0 ? std::hypot(m.c, m.d) : 1;
  const Point size = world_.vec(*e, "size");
  double hw = size.x * sx / 2;
  double hh = (world_.str(*e, "shape") == "circle" ? size.x : size.y) * sy / 2;
  const double rot = angle_of(b2Body_GetRotation(r->body));
  if (rot != 0) {
    const double c = std::fabs(std::cos(rot)), s = std::fabs(std::sin(rot));
    const double nw = hw * c + hh * s, nh = hw * s + hh * c;
    hw = nw, hh = nh;
  }
  return Aabb{tx - hw, ty - hh, tx + hw, ty + hh};
}

std::vector<EntityId> Physics2D::moving_bodies() const {
  std::vector<EntityId> out;
  for (const auto& [seq, r] : recs_)
    if ((r.kind == Kind::Dynamic || r.kind == Kind::Kinematic) && b2Body_IsEnabled(r.body)) out.push_back(r.entity);
  return out;
}

// ================================================================ z ranges (2.5D)

bool Physics2D::z_overlap(const Entity& a, const Entity& b) {
  if (a.height <= 0 || b.height <= 0) return true;  // flat shapes interact with everything
  return a.z < b.z + b.height && b.z < a.z + a.height;
}

void Physics2D::pre_solve(b2ShapeId a, b2ShapeId b, b2Manifold* manifold, void* context) {
  auto* self = static_cast<Physics2D*>(context);
  const Entity* ea = self->entity_of_shape(a);
  const Entity* eb = self->entity_of_shape(b);
  if (ea && eb && !z_overlap(*ea, *eb)) manifold->pointCount = 0;  // not touching this step
}

bool Physics2D::pre_continuous(b2ShapeId a, b2ShapeId b, b2Pos, b2Vec2, void* context) {
  return static_cast<Physics2D*>(context)->z_allows(a, b);  // continuous collision respects z too
}

bool Physics2D::z_allows(b2ShapeId a, b2ShapeId b) const {
  const Entity* ea = entity_of_shape(a);
  const Entity* eb = entity_of_shape(b);
  return !(ea && eb) || z_overlap(*ea, *eb);
}

// Does the body's footprint (or, for a tile map, a solid tile) contain the world point?
bool Physics2D::covers(const BodyRec& r, double x, double y) {
  const Entity* e = world_.get(r.entity);
  if (!e) return false;
  if (r.kind == Kind::Tiles) {
    if (!e->tiles) return false;
    // World point -> the map's local pixels -> cell.
    const Point local = world::apply(world::invert(world_.global(r.entity)), x, y);
    const double ts = world_.num(*e, "tile_size");
    if (!(ts > 0)) return false;
    const int col = static_cast<int>(std::floor(local.x / ts)), row = static_cast<int>(std::floor(local.y / ts));
    if (!e->tiles->in_bounds(col, row)) return false;
    const world::TileKind* kind = e->tiles->kind_at(col, row);
    return kind && kind->solid;
  }
  std::optional<Aabb> box = aabb(r.entity);
  return box && box->x0 <= x && x <= box->x1 && box->y0 <= y && y <= box->y1;
}

double Physics2D::ground_below(double x, double y, double z, uint32_t ignore) {
  double ground = 0;
  for (auto& [seq, r] : recs_) {
    if (r.handle == ignore) continue;
    if (r.kind != Kind::Static && r.kind != Kind::Kinematic && r.kind != Kind::Tiles) continue;
    if (!b2Body_IsEnabled(r.body)) continue;
    const Entity* s = world_.get(r.entity);
    if (!s || s->height <= 0) continue;
    const double top = s->z + s->height;
    if (top > z + 1e-6 || top <= ground) continue;  // above us, or no higher than what we have
    if (covers(r, x, y)) ground = top;
  }
  return ground;
}

double Physics2D::ground_of(EntityId id) {
  const Entity* e = world_.get(id);
  BodyRec* r = e ? rec(id) : nullptr;
  if (!r) return 0;
  const b2Vec2 p = b2Body_GetPosition(r->body);
  return ground_below(to_px(p.x), to_px(p.y), e->z, r->handle);
}

void Physics2D::height_changed(EntityId id) {
  const Entity* e = world_.get(id);
  BodyRec* r = e ? find(e->handle) : nullptr;
  if (r) b2Body_EnableContactRecycling(r->body, e->height <= 0);
}

bool Physics2D::on_ground(EntityId id) {
  const Entity* e = world_.get(id);
  if (!e) return false;
  return e->z_velocity <= 0 && e->z <= ground_of(id) + 1e-6;
}

void Physics2D::step_z(double dt) {
  for (auto& [seq, r] : recs_) {
    if (r.kind != Kind::Dynamic && r.kind != Kind::Kinematic) continue;
    Entity* e = world_.get(r.entity);
    if (!e || (e->z == 0 && e->z_velocity == 0 && e->height <= 0) || !b2Body_IsEnabled(r.body)) continue;
    const b2Vec2 p = b2Body_GetPosition(r.body);
    const double ground = ground_below(to_px(p.x), to_px(p.y), e->z, r.handle);
    if (e->z > ground || e->z_velocity > 0) {
      // In the air: semi-implicit Euler (velocity first), then land when we reach the ground.
      e->z_velocity -= kZGravity * e->z_gravity_scale * dt;
      e->z += e->z_velocity * dt;
      if (e->z <= ground) {
        e->z = ground;
        e->z_velocity = 0;
      }
    } else {
      e->z = ground;  // on the ground (or pushed below it, e.g. a script set z < ground)
      e->z_velocity = 0;
    }
  }
}

// ================================================================ Area2D overlaps

namespace {

b2ShapeProxy proxy_of(b2ShapeId s) {
  switch (b2Shape_GetType(s)) {
    case b2_circleShape: {
      b2Circle c = b2Shape_GetCircle(s);
      return b2MakeProxy(&c.center, 1, c.radius);
    }
    case b2_capsuleShape: {
      b2Capsule c = b2Shape_GetCapsule(s);
      b2Vec2 pts[2] = {c.center1, c.center2};
      return b2MakeProxy(pts, 2, c.radius);
    }
    default: {
      b2Polygon p = b2Shape_GetPolygon(s);
      return b2MakeProxy(p.vertices, p.count, p.radius);
    }
  }
}

bool shape_less(b2ShapeId a, b2ShapeId b) { return a.index1 != b.index1 ? a.index1 < b.index1 : a.generation < b.generation; }

}  // namespace

// The exact overlap test Box2D's sensors use (GJK distance below a tiny epsilon).
bool Physics2D::shapes_overlap(b2ShapeId a, b2ShapeId b) const {
  b2DistanceInput in{};
  in.proxyA = proxy_of(a);
  in.proxyB = proxy_of(b);
  in.transform = b2InvMulWorldTransforms(b2Body_GetTransform(b2Shape_GetBody(a)), b2Body_GetTransform(b2Shape_GetBody(b)));
  in.useRadii = true;
  b2SimplexCache cache{};
  return b2ComputeShapeDistance(&in, &cache, nullptr, 0).distance < 10.0f * FLT_EPSILON;
}

// Area2D overlaps at the start of the step (where Rapier's narrow phase ran in engine-web):
// diff each area's overlapping shapes against the last step's and report begins and ends.
// Shapes that were destroyed (their body was removed or rebuilt) end silently, as in engine-web.
void Physics2D::update_areas(std::vector<ContactEvent>& out) {
  struct Query {
    const Physics2D* self;
    b2ShapeId sensor;
    b2BodyId body;
    std::vector<b2ShapeId> hits;
  };
  for (auto& [seq, r] : recs_) {
    if (r.kind != Kind::Area) continue;
    Query q{this, r.shapes[0], r.body, {}};
    if (b2Body_IsEnabled(r.body)) {
      const b2Filter f = b2Shape_GetFilter(q.sensor);
      b2QueryFilter qf = b2DefaultQueryFilter();
      qf.categoryBits = f.categoryBits;
      qf.maskBits = f.maskBits;
      b2World_OverlapAABB(b2world_, b2Pos{0, 0}, b2Shape_GetBounds(q.sensor), qf, [](b2ShapeId s, void* ctx) {
        auto* query = static_cast<Query*>(ctx);
        if (B2_ID_EQUALS(b2Shape_GetBody(s), query->body) || b2Shape_IsSensor(s)) return true;  // areas never detect areas
        if (!query->self->z_allows(query->sensor, s) || !query->self->shapes_overlap(query->sensor, s)) return true;
        query->hits.push_back(s);
        return true;
      }, &q);
      std::sort(q.hits.begin(), q.hits.end(), shape_less);
    }
    for (b2ShapeId s : q.hits) {
      bool known = std::any_of(r.overlaps.begin(), r.overlaps.end(), [&](b2ShapeId o) { return B2_ID_EQUALS(o, s); });
      if (!known) push_events(out, q.sensor, s, true);
    }
    for (b2ShapeId o : r.overlaps) {
      bool still = std::any_of(q.hits.begin(), q.hits.end(), [&](b2ShapeId s) { return B2_ID_EQUALS(o, s); });
      if (!still && b2Shape_IsValid(o)) push_events(out, q.sensor, o, false);
    }
    r.overlaps = std::move(q.hits);
  }
}

// ================================================================ stepping

void Physics2D::push_events(std::vector<ContactEvent>& out, b2ShapeId s1, b2ShapeId s2, bool started) {
  BodyRec* r1 = find_shape(s1);
  BodyRec* r2 = find_shape(s2);
  if (!r1 || !r2 || r1 == r2) return;
  const bool a1 = r1->kind == Kind::Area, a2 = r2->kind == Kind::Area;
  if (a1 && a2) return;
  if (r1->kind == Kind::Tiles || r2->kind == Kind::Tiles) {
    BodyRec* t = r1->kind == Kind::Tiles ? r1 : r2;
    BodyRec* o = r1->kind == Kind::Tiles ? r2 : r1;
    const std::pair<uint32_t, uint32_t> key{t->handle, o->handle};
    const int n = tile_pairs_[key];
    const int next = std::max(0, n + (started ? 1 : -1));
    tile_pairs_[key] = next;
    if (started && n == 0) {
      auto it = std::find_if(stale_tile_pairs_.begin(), stale_tile_pairs_.end(), [&](const auto& p) { return p.first == key; });
      if (it != stale_tile_pairs_.end()) {  // same contact, new colliders
        stale_tile_pairs_.erase(it);
        return;
      }
    }
    if ((started && n == 0) || (!started && n > 0 && next == 0)) {
      out.push_back({o->entity, t->entity, started});
      if (o->kind != Kind::Area) out.push_back({t->entity, o->entity, started});
    }
    return;
  }
  if (a1) out.push_back({r1->entity, r2->entity, started});
  else if (a2) out.push_back({r2->entity, r1->entity, started});
  else {
    out.push_back({r1->entity, r2->entity, started});
    out.push_back({r2->entity, r1->entity, started});
  }
}

std::vector<ContactEvent> Physics2D::step(double dt) {
  flush();
  std::vector<ContactEvent> area_events;
  update_areas(area_events);
  b2World_Step(b2world_, static_cast<float>(dt), kSubSteps);

  // Write simulated transforms back to entities (relative to their parents).
  for (auto& [seq, r] : recs_) {
    if (r.kind != Kind::Dynamic && r.kind != Kind::Kinematic) continue;
    if (!b2Body_IsAwake(r.body)) continue;
    Entity* e = world_.get(r.entity);
    if (!e) continue;
    const b2Vec2 p = b2Body_GetPosition(r.body);
    const world::Mat2D parent = world_.global(e->parent);
    const Point l = world::apply(world::invert(parent), to_px(p.x), to_px(p.y));
    const Point cur = world_.vec(*e, world::Hot::Position);
    if (std::fabs(cur.x - l.x) > 1e-6 || std::fabs(cur.y - l.y) > 1e-6) world_.set_raw(*e, "position", world::Value::vec(l.x, l.y));
    if (!world_.flag(*e, world::Hot::LockRotation)) {
      const double parent_rot = world::rotation_of(parent);
      const double deg = (angle_of(b2Body_GetRotation(r.body)) - parent_rot) * 180 / kPi;
      if (std::fabs(deg - world_.num(*e, world::Hot::Rotation)) > 1e-6)
        world_.set_raw(*e, "rotation", world::Value::number(deg));
    }
  }

  std::vector<ContactEvent> events;
  const b2ContactEvents contacts = b2World_GetContactEvents(b2world_);
  for (int i = 0; i < contacts.beginCount; ++i) push_events(events, contacts.beginEvents[i].shapeIdA, contacts.beginEvents[i].shapeIdB, true);
  events.insert(events.end(), area_events.begin(), area_events.end());
  for (int i = 0; i < contacts.endCount; ++i) push_events(events, contacts.endEvents[i].shapeIdA, contacts.endEvents[i].shapeIdB, false);
  for (const auto& [key, pair] : stale_tile_pairs_) {
    events.push_back({pair.second, pair.first, false});
    events.push_back({pair.first, pair.second, false});
  }
  stale_tile_pairs_.clear();

  step_z(dt);
  return events;
}

void Physics2D::hash_into(Hasher& h) const {
  for (const auto& [seq, r] : recs_) {
    h.add(r.handle);
    h.add(static_cast<uint8_t>(r.kind));
    h.add(static_cast<uint32_t>(r.shapes.size()));
    const b2Vec2 p = b2Body_GetPosition(r.body);
    const b2Rot q = b2Body_GetRotation(r.body);
    const b2Vec2 v = b2Body_GetLinearVelocity(r.body);
    h.add(p.x), h.add(p.y), h.add(q.c), h.add(q.s), h.add(v.x), h.add(v.y);
    h.add(b2Body_GetAngularVelocity(r.body));
    h.add(static_cast<uint8_t>(b2Body_IsAwake(r.body)));
    h.add(static_cast<uint8_t>(b2Body_IsEnabled(r.body)));
  }
}

}  // namespace dreamsy::physics
