// The renderer's CPU side (E4): depth sorting per view mode, letterbox and camera math, image
// decoding, atlas packing, text layout, batching. Everything here runs without a GPU.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "doctest.h"
#include "pkg/validate.h"
#include "render/atlas.h"
#include "render/draw_list.h"
#include "render/font.h"
#include "render/geometry.h"
#include "render/image.h"
#include "render/sort.h"
#include "render/style.h"
#include "render/textures.h"
#include "render/view.h"

using namespace dreamsy;
using namespace dreamsy::render;
namespace fs = std::filesystem;

namespace {

const std::string kRoot = DREAMSY_REPO_ROOT;

SpriteCmd sprite_at(float x, float y, float z = 0, int32_t layer = 0, int32_t z_index = 0) {
  SpriteCmd s;
  s.position = {x, y};
  s.z = z;
  s.sort.layer = layer;
  s.sort.z_index = z_index;
  return s;
}

// The sprite indices of the sorted world list, in draw order (shadows as -1 - index).
std::vector<int> world_order(const DrawList& list) {
  SortedDrawList sorted;
  sort_draw_list(list, sorted);
  std::vector<int> out;
  for (const DrawRef& r : sorted.world) {
    if (r.kind == DrawKind::Sprite) out.push_back(static_cast<int>(r.index));
    if (r.kind == DrawKind::Shadow) out.push_back(-1 - static_cast<int>(r.index));
    if (r.kind == DrawKind::TileChunk) out.push_back(1000 + static_cast<int>(r.index));
  }
  return out;
}

std::unique_ptr<pkg::Package> load_demo() {
  auto files = pkg::Files::from_dir(kRoot + "/games/examples/feature-demo");
  REQUIRE_MESSAGE(files.ok(), files.error());
  auto r = pkg::load_package(std::move(files).value());
  REQUIRE(r.ok);
  return std::move(r.package);
}

}  // namespace

// ---------------------------------------------------------------- sorting

TEST_CASE("sort: side view draws by layer, then z_index, then submission order") {
  DrawList list;
  list.view = ViewMode::Side;
  list.add(sprite_at(0, 500, 0, 1, 0));   // 0: layer 1
  list.add(sprite_at(0, 100, 0, 0, 5));   // 1: layer 0, z 5
  list.add(sprite_at(0, 900, 0, 0, 0));   // 2: layer 0, z 0
  list.add(sprite_at(0, 50, 0, 0, 0));    // 3: layer 0, z 0, later
  list.add(sprite_at(0, 0, 0, -1, 99));   // 4: layer -1
  // y plays no part in side view.
  CHECK(world_order(list) == std::vector<int>{4, 2, 3, 1, 0});
}

TEST_CASE("sort: topdown sorts by y inside a layer and z_index") {
  DrawList list;
  list.view = ViewMode::TopDown;
  list.add(sprite_at(0, 300));           // 0
  list.add(sprite_at(0, 100));           // 1
  list.add(sprite_at(0, 200));           // 2
  list.add(sprite_at(0, 50, 0, 0, -1));  // 3: z_index -1 stays behind everything at 0
  list.add(sprite_at(0, 10, 0, 1));      // 4: layer 1 stays in front
  CHECK(world_order(list) == std::vector<int>{3, 1, 2, 0, 4});

  // sort_offset moves the sort point (e.g. a tall tree sorting by its trunk).
  list.sprites[1].sort.sort_offset = 250;  // 100 + 250 = 350
  CHECK(world_order(list) == std::vector<int>{3, 2, 0, 1, 4});
}

TEST_CASE("sort: oblique sorts by y, then height z") {
  DrawList list;
  list.view = ViewMode::Oblique;
  list.add(sprite_at(0, 100, 30));  // 0: same y, higher
  list.add(sprite_at(0, 100, 0));   // 1: same y, on the ground
  list.add(sprite_at(0, 90, 50));   // 2: further back
  const auto order = world_order(list);
  // 0 casts no shadow (shadow flag off): 2 (y 90), then 1 (z 0), then 0 (z 30).
  CHECK(order == std::vector<int>{2, 1, 0});
}

TEST_CASE("sort: iso sorts by x + y, then z") {
  DrawList list;
  list.view = ViewMode::Iso;
  list.add(sprite_at(100, 100));     // 0: depth 200
  list.add(sprite_at(150, 0));       // 1: depth 150
  list.add(sprite_at(0, 200, 10));   // 2: depth 200, higher than 0
  list.add(sprite_at(300, -100));    // 3: depth 200, z 0, submitted after 0 -> after 0
  CHECK(world_order(list) == std::vector<int>{1, 0, 3, 2});
}

TEST_CASE("sort: ties keep submission order, so equal items never flicker") {
  for (ViewMode view : {ViewMode::Side, ViewMode::TopDown, ViewMode::Oblique, ViewMode::Iso}) {
    DrawList list;
    list.view = view;
    for (int i = 0; i < 50; i++) list.add(sprite_at(10, 10, 0));
    std::vector<int> expected(50);
    for (int i = 0; i < 50; i++) expected[i] = i;
    CHECK(world_order(list) == expected);
  }
}

TEST_CASE("sort: tile chunks lie under upright items of the same layer, shadows between") {
  DrawList list;
  list.view = ViewMode::TopDown;
  SpriteCmd jumper = sprite_at(0, -500, 20);
  jumper.shadow = true;
  list.add(jumper);                   // sprite 0, far up the screen, in the air
  TileChunkCmd chunk;
  chunk.origin = {0, 1000};           // below everything on screen, but flat
  list.add(chunk);                    // chunk 0
  list.add(sprite_at(0, -600));       // sprite 1
  CHECK(world_order(list) == std::vector<int>{1000, -1, 1, 0});

  // In side view the shadow draws right before its sprite; the chunk keeps submission order.
  list.view = ViewMode::Side;
  CHECK(world_order(list) == std::vector<int>{-1, 0, 1000, 1});
}

TEST_CASE("sort: shadows only for raised world sprites with the flag") {
  SpriteCmd s = sprite_at(0, 0, 0);
  s.shadow = true;
  CHECK_FALSE(casts_shadow(s));  // on the ground
  s.z = 5;
  CHECK(casts_shadow(s));
  s.space = Space::Screen;
  CHECK_FALSE(casts_shadow(s));
}

TEST_CASE("sort: UI items are separate and never depth-sorted") {
  DrawList list;
  list.view = ViewMode::Iso;
  SpriteCmd ui_a = sprite_at(0, 900);
  ui_a.space = Space::Screen;
  SpriteCmd ui_b = sprite_at(0, 0);
  ui_b.space = Space::Screen;
  list.add(ui_a);
  list.add(sprite_at(0, 0));
  list.add(ui_b);
  SortedDrawList sorted;
  sort_draw_list(list, sorted);
  REQUIRE(sorted.world.size() == 1);
  REQUIRE(sorted.screen.size() == 2);
  CHECK(sorted.screen[0] == DrawRef{DrawKind::Sprite, 0});
  CHECK(sorted.screen[1] == DrawRef{DrawKind::Sprite, 2});
}

TEST_CASE("sort: NaN positions don't break the sort") {
  DrawList list;
  list.view = ViewMode::TopDown;
  for (int i = 0; i < 100; i++) list.add(sprite_at(0, (i % 3 == 0) ? NAN : static_cast<float>(i % 7)));
  SortedDrawList sorted;
  sort_draw_list(list, sorted);
  CHECK(sorted.world.size() == 100);
}

// ---------------------------------------------------------------- view math

TEST_CASE("view: letterbox fits the reference screen and centers it") {
  // Portrait game in a landscape window: pillarbox.
  Fit f = fit_rect(720, 1280, 1920, 1080);
  CHECK(f.s == doctest::Approx(1080.0 / 1280));
  CHECK(f.h == doctest::Approx(1080));
  CHECK(f.w == doctest::Approx(607.5));
  CHECK(f.x == doctest::Approx((1920 - 607.5) / 2));
  CHECK(f.y == doctest::Approx(0));

  // Landscape game in a tall phone: letterbox.
  f = fit_rect(1280, 720, 1170, 2532);
  CHECK(f.w == doctest::Approx(1170));
  CHECK(f.x == doctest::Approx(0));
  CHECK(f.y == doctest::Approx((2532 - 720 * (1170.0 / 1280)) / 2));

  // Exact fit.
  f = fit_rect(1280, 720, 2560, 1440);
  CHECK(f.s == doctest::Approx(2));
  CHECK(f.x == 0);
  CHECK(f.y == 0);

  // Snapped (pixel art): whole pixels.
  const Fit s = snap_fit(fit_rect(720, 1280, 1001, 999));
  CHECK(s.x == std::round(s.x));
  CHECK(s.w == std::round(s.w));

  // Framebuffer <-> reference.
  f = fit_rect(720, 1280, 1920, 1080);
  const Vec2 p = framebuffer_to_ref(f, ref_to_framebuffer(f, {100, 200}));
  CHECK(p.x == doctest::Approx(100));
  CHECK(p.y == doctest::Approx(200));
  const Vec2 corner = framebuffer_to_ref(f, {f.x, f.y});
  CHECK(corner.x == doctest::Approx(0));
}

TEST_CASE("view: projection per view mode") {
  Vec2 p = project(ViewMode::Side, 10, 20, 5);
  CHECK(p.x == 10);
  CHECK(p.y == 15);  // height draws upward (y is down)
  p = project(ViewMode::Iso, 10, 20, 5);
  CHECK(p.x == -10);
  CHECK(p.y == doctest::Approx(10));  // (10 + 20) / 2 - 5
  // A square tile becomes a 2:1 diamond in iso.
  const Vec2 right = project(ViewMode::Iso, 16, 0, 0), down = project(ViewMode::Iso, 0, 16, 0);
  CHECK(right.x - down.x == doctest::Approx(32));
  CHECK(right.y == doctest::Approx(8));
}

TEST_CASE("view: camera center semantics and round trips") {
  const Camera cam{{500, 300}, 2};
  for (ViewMode view : {ViewMode::Side, ViewMode::TopDown, ViewMode::Oblique, ViewMode::Iso}) {
    // The camera center is the middle of the screen.
    const Vec2 c = world_to_ref(view, cam, 720, 1280, 500, 300, 0);
    CHECK(c.x == doctest::Approx(360));
    CHECK(c.y == doctest::Approx(640));
    // ref -> world -> ref.
    const Vec2 w = ref_to_world(view, cam, 720, 1280, {100, 900});
    const Vec2 r = world_to_ref(view, cam, 720, 1280, w.x, w.y, 0);
    CHECK(r.x == doctest::Approx(100));
    CHECK(r.y == doctest::Approx(900));
  }
  // Zoom 2: 10 world px right of the center is 20 reference px right (engine-web worldToRef).
  const Vec2 q = world_to_ref(ViewMode::Side, cam, 720, 1280, 510, 300, 0);
  CHECK(q.x == doctest::Approx(380));
  // Default camera: the view's top-left is world (0, 0).
  const Vec2 tl = world_to_ref(ViewMode::Side, DrawList::default_camera(1280, 720), 1280, 720, 0, 0, 0);
  CHECK(tl.x == doctest::Approx(0));
  CHECK(tl.y == doctest::Approx(0));
}

TEST_CASE("view: pixel snapping moves the camera to whole texels") {
  const Camera cam{{100.3f, 50.8f}, 1};
  Vec2 c = snapped_camera_center(ViewMode::Side, cam, true);
  CHECK(c.x == doctest::Approx(100));
  CHECK(c.y == doctest::Approx(51));
  c = snapped_camera_center(ViewMode::Side, Camera{{100.3f, 50.8f}, 4}, true);
  CHECK(c.x == doctest::Approx(100.25));  // 1 texel = 1/4 world px at zoom 4
  c = snapped_camera_center(ViewMode::Side, cam, false);
  CHECK(c.x == doctest::Approx(100.3));
}

// ---------------------------------------------------------------- images

TEST_CASE("image: formats from file names") {
  CHECK(image_format_from_name("a.svg") == ImageFormat::Svg);
  CHECK(image_format_from_name("A.PNG") == ImageFormat::Png);
  CHECK(image_format_from_name("x.jpeg") == ImageFormat::Jpg);
  CHECK(image_format_from_name("x.webp") == ImageFormat::WebP);
  CHECK(image_format_from_name("x.glb") == ImageFormat::Unknown);
}

TEST_CASE("image: PNG decodes; WebP and garbage fail with a message") {
  auto files = pkg::Files::from_dir(kRoot + "/games/examples/coin-dash");
  REQUIRE(files.ok());
  auto bytes = files.value().bytes("assets/coin.png");
  REQUIRE(bytes);
  auto img = decode_raster(*bytes, ImageFormat::Png);
  REQUIRE_MESSAGE(img.ok(), img.error());
  CHECK(img.value().width > 0);
  CHECK(img.value().pixels.size() == static_cast<size_t>(img.value().width) * img.value().height * 4);

  const std::vector<uint8_t> junk{1, 2, 3, 4, 5, 6, 7, 8};
  CHECK_FALSE(decode_raster(junk, ImageFormat::Png).ok());
  auto webp = decode_raster(junk, ImageFormat::WebP);
  REQUIRE_FALSE(webp.ok());
  CHECK(webp.error().find("WebP") != std::string::npos);
}

TEST_CASE("image: SVG sprites parse and rasterize at any size") {
  auto files = pkg::Files::from_dir(kRoot + "/games/examples/feature-demo");
  REQUIRE(files.ok());
  auto text = files.value().text("assets/fox_idle_1.svg");
  REQUIRE(text);
  auto svg = Svg::parse(*text);
  REQUIRE_MESSAGE(svg.ok(), svg.error());
  CHECK(svg.value().width() == 16);
  CHECK(svg.value().height() == 16);

  auto img = svg.value().rasterize(16, 16);
  REQUIRE(img.ok());
  // fox_idle_1.svg: <rect x="2" y="2" width="1" height="1" fill="#ef7d57"/>
  const uint8_t* p = img.value().pixels.data() + (2 * 16 + 2) * 4;
  CHECK(p[0] == 0xef);
  CHECK(p[1] == 0x7d);
  CHECK(p[2] == 0x57);
  CHECK(p[3] == 255);
  // (0, 0) is empty.
  CHECK(img.value().pixels[3] == 0);

  auto big = svg.value().rasterize(64, 64);
  REQUIRE(big.ok());
  CHECK(big.value().pixels.size() == 64 * 64 * 4);
  CHECK_FALSE(Svg::parse("not svg").ok());
}

TEST_CASE("image: bounding-box gradients span the shape (nanosvg unitless fix)") {
  // Unitless coordinates become percentages; user-space gradients and units stay as they are.
  CHECK(fix_bbox_gradients(R"(<linearGradient id="g" x1="0" y1="0" x2="0" y2="1">)") ==
        R"(<linearGradient id="g" x1="0%" y1="0%" x2="0%" y2="100%">)");
  CHECK(fix_bbox_gradients(R"(<radialGradient cx=".5" r='0.45'/>)") == R"(<radialGradient cx="50%" r='45%'/>)");
  const std::string user = R"(<linearGradient gradientUnits="userSpaceOnUse" x1="0" y2="25">)";
  CHECK(fix_bbox_gradients(user) == user);
  CHECK(fix_bbox_gradients(R"(<linearGradient x2="10px" y2="50%">)") == R"(<linearGradient x2="10px" y2="50%">)");

  // A top-to-bottom sky: blue at the top, pale at the bottom, in between halfway down.
  auto svg = Svg::parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 64"><defs>
    <linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#000000"/>
    <stop offset="1" stop-color="#ffffff"/></linearGradient></defs>
    <rect width="8" height="64" fill="url(#g)"/></svg>)svg");
  REQUIRE(svg.ok());
  auto img = svg.value().rasterize(8, 64);
  REQUIRE(img.ok());
  auto red = [&](int y) { return static_cast<int>(img.value().pixels[(static_cast<size_t>(y) * 8 + 4) * 4]); };
  CHECK(red(0) < 16);
  CHECK(red(32) > 100);
  CHECK(red(32) < 156);
  CHECK(red(63) > 240);
}

TEST_CASE("image: <use> copies the referenced shape (nanosvg has no <use>)") {
  CHECK(expand_svg_uses(R"x(<defs><rect id="r" width="2" height="2"/></defs><use href="#r" x="4" fill="#f00"/>)x") ==
        R"x(<defs><rect id="r" width="2" height="2"/></defs><g fill="#f00" transform=" translate(4 0)"><rect width="2" height="2"/></g>)x");
  // A red square defined once, drawn at two places.
  auto svg = Svg::parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 8"><defs>
    <rect id="sq" width="4" height="4"/></defs>
    <use href="#sq" x="1" y="2" fill="#ff0000"/><use xlink:href="#sq" x="10" y="2" fill="#0000ff"></use></svg>)svg");
  REQUIRE(svg.ok());
  auto img = svg.value().rasterize(16, 8);
  REQUIRE(img.ok());
  auto px = [&](int x, int y) { return img.value().pixels.data() + (static_cast<size_t>(y) * 16 + x) * 4; };
  CHECK(px(2, 3)[0] == 255);
  CHECK(px(2, 3)[3] == 255);
  CHECK(px(11, 3)[2] == 255);
  CHECK(px(7, 3)[3] == 0);  // between the two copies
  CHECK(px(2, 0)[3] == 0);  // above the first copy (y = 2)
}

TEST_CASE("image: rects filled with a pattern draw its tiles (nanosvg has no <pattern>)") {
  auto svg = Svg::parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><defs>
    <pattern id="h" width="8" height="8" patternUnits="userSpaceOnUse"><rect x="2" y="2" width="2" height="2" fill="#00ff00"/></pattern>
    </defs><rect width="16" height="16" fill="url(#h)" opacity="0.5"/></svg>)svg");
  REQUIRE(svg.ok());
  auto img = svg.value().rasterize(16, 16);
  REQUIRE(img.ok());
  auto px = [&](int x, int y) { return img.value().pixels.data() + (static_cast<size_t>(y) * 16 + x) * 4; };
  for (auto [x, y] : {std::pair{2, 2}, {10, 2}, {2, 10}, {11, 11}}) {
    CHECK(px(x, y)[1] == 255);
    CHECK(px(x, y)[3] == doctest::Approx(128).epsilon(0.02));  // the rect's opacity applies to the tiles
  }
  CHECK(px(6, 6)[3] == 0);
}

TEST_CASE("image: SVG <text> is drawn with the bundled fonts") {
  // viewBox 48 wide, drawn at width 96: text coordinates scale with the viewBox like shapes.
  auto svg = Svg::parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48" width="96" height="96">
    <text x="24" y="40" font-family="Arial" font-weight="bold" font-size="40" text-anchor="middle" fill="#ff0000">I&amp;</text></svg>)svg");
  REQUIRE(svg.ok());
  REQUIRE(svg.value().texts().size() == 1);
  CHECK(svg.value().texts()[0].text == "I&");
  CHECK(svg.value().texts()[0].anchor == 0);
  auto img = svg.value().rasterize(96, 96);
  REQUIRE(img.ok());
  int red = 0;
  for (size_t i = 0; i < img.value().pixels.size(); i += 4) red += img.value().pixels[i + 3] > 128 && img.value().pixels[i] > 200;
  CHECK(red > 300);  // two bold glyphs, 80 px tall
}

TEST_CASE("image: a bounding-box radial gradient on a wide rect is an ellipse, like in a browser") {
  auto svg = Svg::parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 16"><defs>
    <radialGradient id="n"><stop offset="0" stop-color="#ff0000"/><stop offset="0.99" stop-color="#ff0000"/>
    <stop offset="1" stop-color="#ff0000" stop-opacity="0"/></radialGradient></defs>
    <rect width="64" height="16" fill="url(#n)"/></svg>)svg");
  REQUIRE(svg.ok());
  auto img = svg.value().rasterize(64, 16);
  REQUIRE(img.ok());
  auto alpha = [&](int x, int y) { return img.value().pixels[(static_cast<size_t>(y) * 64 + x) * 4 + 3]; };
  CHECK(alpha(32, 8) == 255);
  CHECK(alpha(4, 8) == 255);  // 28 px from the center: inside the 32 x 8 ellipse (a circle would end at ~23 px)
  CHECK(alpha(32, 0) < 128);  // the top edge: outside the ellipse
}

TEST_CASE("image: every SVG in games/ parses and draws something") {
  int count = 0, with_text = 0;
  std::string failures;
  for (const auto& entry : fs::recursive_directory_iterator(kRoot + "/games")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".svg") continue;
    std::ifstream file(entry.path(), std::ios::binary);
    REQUIRE(file.good());
    const std::string text(std::istreambuf_iterator<char>{file}, {});
    count++;
    auto svg = Svg::parse(text);
    if (!svg) {
      failures += entry.path().string() + ": " + svg.error() + "\n";
      continue;
    }
    auto img = svg.value().rasterize(32, 32);
    bool any = false;
    if (img) {
      for (size_t i = 3; i < img.value().pixels.size(); i += 4) any = any || img.value().pixels[i] != 0;
    }
    // nanosvg doesn't draw <text>; two kit icons use it (a known gap, see engine/README.md).
    if (!any && text.find("<text") != std::string::npos) {
      with_text++;
      continue;
    }
    if (!any) failures += entry.path().string() + ": rasterized to nothing\n";
  }
  CHECK(count > 100);
  CHECK(with_text <= 2);
  CHECK_MESSAGE(failures.empty(), failures);
}

TEST_CASE("image: premultiply") {
  Image img;
  img.width = 1;
  img.height = 1;
  img.pixels = {200, 100, 50, 128};
  premultiply(img);
  CHECK(img.pixels[0] == 100);
  CHECK(img.pixels[1] == 50);
  CHECK(img.pixels[2] == 25);
  CHECK(img.pixels[3] == 128);
}

// ---------------------------------------------------------------- atlas

namespace {

Image solid(int w, int h, uint8_t v) {
  Image img;
  img.width = w;
  img.height = h;
  img.pixels.assign(static_cast<size_t>(w) * h * 4, v);
  return img;
}

bool overlaps(const AtlasRegion& a, const AtlasRegion& b, int pad) {
  if (a.page != b.page) return false;
  return a.x - pad < b.x + b.w + pad && b.x - pad < a.x + a.w + pad && a.y - pad < b.y + b.h + pad &&
         b.y - pad < a.y + a.h + pad;
}

}  // namespace

TEST_CASE("atlas: packs images without overlap, inside the page, with correct UVs") {
  Atlas atlas(AtlasOptions{.page_size = 256, .padding = 1, .max_pages = 8});
  std::vector<AtlasRegion> regions;
  for (int i = 0; i < 60; i++) {
    auto r = atlas.add(solid(5 + (i * 7) % 30, 4 + (i * 11) % 25, static_cast<uint8_t>(i)));
    REQUIRE_MESSAGE(r.ok(), r.error());
    regions.push_back(r.value());
  }
  for (size_t i = 0; i < regions.size(); i++) {
    const AtlasRegion& a = regions[i];
    const AtlasPage& page = atlas.page(a.page);
    CHECK(a.x >= 1);
    CHECK(a.y >= 1);
    CHECK(a.x + a.w <= page.width - 1);
    CHECK(a.y + a.h <= page.height - 1);
    CHECK(a.uv.u0 == doctest::Approx(a.x / 256.0));
    CHECK(a.uv.v1 == doctest::Approx((a.y + a.h) / 256.0));
    for (size_t j = i + 1; j < regions.size(); j++) CHECK_FALSE(overlaps(a, regions[j], 1));
    // The pixels landed (each image is filled with its index).
    const uint8_t* p = page.pixels.data() + (static_cast<size_t>(a.y) * page.width + a.x) * 4;
    CHECK(p[0] == static_cast<uint8_t>(i));
  }
}

TEST_CASE("atlas: edges are extruded into the padding") {
  Atlas atlas(AtlasOptions{.page_size = 64, .padding = 2});
  Image img = solid(2, 2, 0);
  // Top-left pixel red, bottom-right blue.
  img.pixels[0] = 255;
  img.pixels[3] = 255;
  img.pixels[3 * 4 + 2] = 255;
  img.pixels[3 * 4 + 3] = 255;
  auto r = atlas.add(img);
  REQUIRE(r.ok());
  const AtlasPage& page = atlas.page(0);
  auto px = [&](int x, int y) { return page.pixels.data() + (static_cast<size_t>(y) * page.width + x) * 4; };
  // Two pixels up-left of the image is still its top-left pixel; same for bottom-right.
  CHECK(px(r.value().x - 2, r.value().y - 2)[0] == 255);
  CHECK(px(r.value().x + 3, r.value().y + 3)[2] == 255);
}

TEST_CASE("atlas: new pages when full, own page for oversized images, limit enforced") {
  Atlas atlas(AtlasOptions{.page_size = 64, .padding = 1, .max_pages = 3});
  auto a = atlas.add(solid(60, 60, 1));
  auto b = atlas.add(solid(60, 60, 2));
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  CHECK(a.value().page == 0);
  CHECK(b.value().page == 1);
  const uint32_t version = atlas.page(1).version;
  auto big = atlas.add(solid(100, 80, 3));
  REQUIRE(big.ok());
  CHECK(big.value().page == 2);
  CHECK(atlas.page(2).width == 102);
  CHECK(atlas.page(1).version == version);  // untouched pages keep their version
  auto full = atlas.add(solid(60, 60, 4));
  CHECK_FALSE(full.ok());
  CHECK_FALSE(atlas.add(Image{}).ok());
}

// ---------------------------------------------------------------- text

TEST_CASE("text: bundled fonts load") {
  for (FontId id : {FontId::Pixel, FontId::Rounded, FontId::Bold, FontId::Serif}) {
    auto f = Font::load(bundled_font(id));
    REQUIRE_MESSAGE(f.ok(), f.error());
    CHECK(f.value().glyph_index('A') != 0);
    CHECK(f.value().ascent(16) > 0);
    CHECK(f.value().descent(16) <= 0);
  }
  CHECK(font_from_name("pixel") == FontId::Pixel);
  CHECK(font_from_name("serif") == FontId::Serif);
  CHECK(font_from_name("comic") == FontId::Default);
}

TEST_CASE("text: layout metrics (monospace pixel font)") {
  auto font = Font::load(bundled_font(FontId::Pixel));
  REQUIRE(font.ok());
  // DejaVu Sans Mono Bold (Menlo's metrics): every glyph advances 1233/2048 em.
  const float em = 1233.0f / 2048.0f;
  TextLayout l = layout_text(font.value(), "Hello", 8, TextAlign::Left);
  CHECK(l.width == doctest::Approx(5 * 8 * em));
  CHECK(l.line_height == doctest::Approx(10));  // ceil(8 * 1.2)
  CHECK(l.height == doctest::Approx(10));
  REQUIRE(l.glyphs.size() == 5);
  CHECK(l.glyphs[1].x == doctest::Approx(8 * em));

  // Two lines, centered: the short line is shifted by half the difference.
  l = layout_text(font.value(), "abcd\nab", 16, TextAlign::Center);
  CHECK(l.width == doctest::Approx(4 * 16 * em));
  CHECK(l.height == doctest::Approx(2 * 20));
  REQUIRE(l.glyphs.size() == 6);
  CHECK(l.glyphs[4].x == doctest::Approx(16 * em));
  CHECK(l.glyphs[4].y == doctest::Approx(l.glyphs[0].y + 20));
  l = layout_text(font.value(), "abcd\nab", 16, TextAlign::Right);
  CHECK(l.glyphs[4].x == doctest::Approx(32 * em));

  // Baselines sit inside their line.
  l = layout_text(font.value(), "X", 20, TextAlign::Left);
  CHECK(l.glyphs[0].y > 0);
  CHECK(l.glyphs[0].y < l.line_height);
}

TEST_CASE("text: characters a font lacks come from the symbol font") {
  auto font = Font::load(bundled_font(FontId::Bold));
  REQUIRE(font.ok());
  REQUIRE(symbol_font() != nullptr);
  TextLayout l = layout_text(font.value(), "A\u25B6\u2605", 20, TextAlign::Left);  // A▶★
  REQUIRE(l.glyphs.size() == 3);
  CHECK_FALSE(l.glyphs[0].fallback);
  CHECK(l.glyphs[1].fallback);
  CHECK(l.glyphs[1].glyph != 0);
  CHECK(l.glyphs[2].fallback);
  CHECK(l.glyphs[2].x > l.glyphs[1].x);
  Atlas atlas;
  FontStore fonts(atlas, FontId::Bold);
  REQUIRE(fonts.init().ok());
  auto g = fonts.glyph(FontId::Bold, l.glyphs[1].glyph, 20, true);
  REQUIRE(g);
  CHECK_FALSE(g->blank);
}

TEST_CASE("text: kerning and proportional widths") {
  auto font = Font::load(bundled_font(FontId::Bold));
  REQUIRE(font.ok());
  const Font& f = font.value();
  const float i_w = layout_text(f, "iiii", 32, TextAlign::Left).width;
  const float w_w = layout_text(f, "WWWW", 32, TextAlign::Left).width;
  CHECK(i_w < w_w);
  // Inter kerns "AV" tighter than its two advances.
  const float av = layout_text(f, "AV", 32, TextAlign::Left).width;
  const float sum = f.advance(f.glyph_index('A'), 32) + f.advance(f.glyph_index('V'), 32);
  CHECK(av < sum);
  // Width scales linearly with size.
  CHECK(layout_text(f, "Dreamsy", 40, TextAlign::Left).width ==
        doctest::Approx(2 * layout_text(f, "Dreamsy", 20, TextAlign::Left).width));
}

TEST_CASE("text: block origin (engine-web text2DOrigin)") {
  Vec2 o = text_block_origin({100, 50}, TextAlign::Center, 40, 20);
  CHECK(o.x == 80);
  CHECK(o.y == 40);
  o = text_block_origin({100, 50}, TextAlign::Left, 40, 20);
  CHECK(o.x == 100);
  o = text_block_origin({100, 50}, TextAlign::Right, 40, 20);
  CHECK(o.x == 60);
}

TEST_CASE("text: UTF-8 decoding") {
  const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xFF\xE2\x82";  // a é € 😀 <bad> <truncated>
  size_t i = 0;
  CHECK(next_codepoint(s, i) == 'a');
  CHECK(next_codepoint(s, i) == 0xE9);
  CHECK(next_codepoint(s, i) == 0x20AC);
  CHECK(next_codepoint(s, i) == 0x1F600);
  CHECK(next_codepoint(s, i) == 0xFFFD);
  CHECK(next_codepoint(s, i) == 0xFFFD);
  CHECK(i < s.size());
  CHECK(next_codepoint(s, i) == 0xFFFD);
  CHECK(i == s.size());
}

// ---------------------------------------------------------------- textures + batching

TEST_CASE("textures: package images load on first use") {
  auto pkg = load_demo();
  Atlas atlas(AtlasOptions{.page_size = 512});
  TextureStore store(pkg.get(), atlas, true);
  const ImageId fox = store.find_asset("asset:fox_idle_1");
  CHECK(fox.valid());
  CHECK(store.find_asset("fox_idle_1") == fox);  // same id with or without the prefix
  CHECK_FALSE(store.find_asset("asset:nope").valid());
  auto size = store.intrinsic_size(fox);
  REQUIRE(size);
  CHECK(size->x == 16);
  auto r = store.region(fox, 64);
  REQUIRE(r);
  CHECK(r->w == 16);  // pixel art: intrinsic size, whatever the screen size
  CHECK_FALSE(store.region(ImageId{999}).has_value());

  // Smooth games rasterize SVGs at the screen size, in power-of-two buckets.
  TextureStore smooth(pkg.get(), atlas, false);
  const ImageId coin = smooth.find_asset("coin");
  auto small = smooth.region(coin, 20);
  REQUIRE(small);
  CHECK(small->w == 32);
  auto large = smooth.region(coin, 100);
  REQUIRE(large);
  CHECK(large->w == 128);
  CHECK(TextureStore::svg_raster_bucket(5000) == 1024);
}

TEST_CASE("geometry: one batch per atlas page; additive shares it; culling; solid fallback") {
  auto pkg = load_demo();
  Atlas atlas;
  TextureStore textures(pkg.get(), atlas, true);
  FontStore fonts(atlas, FontId::Pixel);
  REQUIRE(fonts.init().ok());

  DrawList list;
  list.view = ViewMode::TopDown;
  list.camera = DrawList::default_camera(720, 1280);
  const char* names[] = {"fox_idle_1", "slime_1", "coin", "owl", "spark"};
  for (int i = 0; i < 200; i++) {
    SpriteCmd s = sprite_at(static_cast<float>(20 + (i * 37) % 680), static_cast<float>(20 + (i * 53) % 1240));
    s.image = textures.find_asset(names[i % 5]);
    s.blend = i % 4 == 0 ? BlendMode::Add : BlendMode::Alpha;
    list.add(s);
  }
  SpriteCmd off = sprite_at(-5000, 0);  // off screen
  list.add(off);
  SpriteCmd missing = sprite_at(100, 100);  // no image: solid rect
  list.add(missing);
  TextCmd t;
  t.text = "Score 10";
  t.position = {360, 100};
  t.space = Space::Screen;
  list.add(t);

  SortedDrawList sorted;
  sort_draw_list(list, sorted);
  FrameGeometry geo;
  GeometryParams params{.ref_w = 720, .ref_h = 1280, .world_raster_scale = 1, .screen_raster_scale = 1.5f, .pixel_snap = true};
  build_geometry(list, sorted, textures, fonts, params, geo);

  REQUIRE(atlas.page_count() == 1);
  CHECK(geo.world.size() == 1);   // background + 200 sprites + solid rect, one page, one draw
  CHECK(geo.screen.size() == 1);  // glyphs share the page too
  CHECK(geo.culled == 1);
  CHECK(geo.world[0].quad_count == 1 + 200 + 1);
  CHECK(geo.screen[0].quad_count == 7);  // "Score 10": 8 characters, the space draws nothing
  CHECK(geo.vertices.size() == geo.quad_count() * 4);

  // Additive quads carry alpha 0 (premultiplied "add").
  CHECK((pack_color({1, 1, 1, 1}, BlendMode::Add) >> 24) == 0);
  CHECK(pack_color({1, 0, 0, 0.5f}) == (128u | (128u << 24)));
}

TEST_CASE("geometry: clip rects split batches and cull what's outside; emissive items need lighting") {
  auto pkg = load_demo();
  Atlas atlas;
  TextureStore textures(pkg.get(), atlas, true);
  FontStore fonts(atlas, FontId::Bold);
  REQUIRE(fonts.init().ok());
  DrawList list;
  list.camera = DrawList::default_camera(720, 1280);
  const ClipRect clip{100, 100, 200, 50, true};
  TextCmd inside;
  inside.text = "AB";
  inside.position = {200, 120};
  inside.space = Space::Screen;
  inside.clip = clip;
  list.add(inside);
  TextCmd outside = inside;
  outside.position = {200, 400};  // entirely below the clip: culled
  list.add(outside);
  TextCmd free = inside;
  free.clip = {};
  list.add(free);
  SpriteCmd glow = sprite_at(50, 50);
  glow.emissive = true;
  list.add(glow);

  SortedDrawList sorted;
  sort_draw_list(list, sorted);
  FrameGeometry geo;
  GeometryParams params{.ref_w = 720, .ref_h = 1280};
  build_geometry(list, sorted, textures, fonts, params, geo);
  REQUIRE(geo.screen.size() == 2);  // clipped glyphs, then unclipped ones
  CHECK(geo.screen[0].clip == clip);
  CHECK(geo.screen[0].quad_count == 2);
  CHECK_FALSE(geo.screen[1].clip.enabled);
  CHECK(geo.screen[1].quad_count == 2);
  CHECK(geo.emissive.empty());  // no Lighting2D: emissive draws with the world

  list.lighting.enabled = true;
  build_geometry(list, sorted, textures, fonts, params, geo);
  REQUIRE(geo.emissive.size() == 1);
  CHECK(geo.emissive[0].quad_count == 1);
}

TEST_CASE("geometry: drop shadows shrink and fade with height") {
  Atlas atlas(AtlasOptions{.page_size = 256});
  TextureStore textures(nullptr, atlas, false);
  FontStore fonts(atlas, FontId::Bold);
  REQUIRE(fonts.init().ok());
  auto shadow_quad = [&](float z) {
    DrawList list;
    list.view = ViewMode::Oblique;
    list.camera = DrawList::default_camera(1280, 720);
    SpriteCmd s = sprite_at(640, 360, z);
    s.size = {40, 40};
    s.shadow = true;
    list.add(s);
    SortedDrawList sorted;
    sort_draw_list(list, sorted);
    FrameGeometry geo;
    build_geometry(list, sorted, textures, fonts, GeometryParams{}, geo);
    return geo;
  };
  const FrameGeometry low = shadow_quad(10), high = shadow_quad(150);
  REQUIRE(low.quad_count() == 3);  // background, shadow, sprite
  const auto width = [](const FrameGeometry& g) { return g.vertices[4 + 1].x - g.vertices[4].x; };
  const auto alpha = [](const FrameGeometry& g) { return g.vertices[4].color >> 24; };
  CHECK(width(high) < width(low));
  CHECK(alpha(high) < alpha(low));
  // The shadow sits on the ground (y = 360) while the sprite is raised.
  const float shadow_mid = (low.vertices[4].y + low.vertices[4 + 2].y) / 2;
  const float sprite_mid = (low.vertices[8].y + low.vertices[8 + 2].y) / 2;
  CHECK(shadow_mid == doctest::Approx(360));
  CHECK(sprite_mid == doctest::Approx(350));
}

// ---------------------------------------------------------------- 2.5D blocks (E7)

namespace {

SpriteCmd block_at(float x, float y, float w, float d, float height, float z = 0) {
  SpriteCmd s = sprite_at(x, y, z);
  s.size = {w, d};
  s.height = height;
  return s;
}

}  // namespace

TEST_CASE("sort: blocks sort by footprint center; things on a block sort right after it") {
  for (ViewMode view : {ViewMode::Oblique, ViewMode::TopDown, ViewMode::Iso}) {
    CAPTURE(static_cast<int>(view));
    DrawList list;
    list.view = view;
    list.add(block_at(100, 100, 48, 48, 24));  // 0: block, footprint y 76..124
    SpriteCmd on_back = sprite_at(100, 80, 24);  // 1: stands on its back half (y < center)
    on_back.ground_z = 24;
    on_back.shadow = true;
    on_back.z = 40;  // jumping on it: shadow on the block's top
    list.add(on_back);
    list.add(sprite_at(100, 60));   // 2: behind the block, on the floor
    list.add(sprite_at(100, 140));  // 3: in front of it
    // behind first, then the block, then the shadow on its top and the jumper, then the one in front
    CHECK(world_order(list) == std::vector<int>{2, 0, -2, 1, 3});
    CHECK(is_block(view, list.sprites[0]));
  }
  DrawList side;
  side.view = ViewMode::Side;
  side.add(block_at(0, 0, 10, 10, 10));
  CHECK_FALSE(is_block(ViewMode::Side, side.sprites[0]));  // side view: just a sprite
}

TEST_CASE("geometry: blocks draw sides then top; oblique top is raised by the height") {
  Atlas atlas(AtlasOptions{.page_size = 256});
  TextureStore textures(nullptr, atlas, false);
  FontStore fonts(atlas, FontId::Bold);
  REQUIRE(fonts.init().ok());
  auto build = [&](ViewMode view, float height = 20) {
    DrawList list;
    list.view = view;
    list.camera = DrawList::default_camera(1280, 720);
    SpriteCmd b = block_at(640, 360, 40, 20, height);
    b.tint = {1, 0, 0, 1};
    list.add(b);
    SortedDrawList sorted;
    sort_draw_list(list, sorted);
    FrameGeometry geo;
    build_geometry(list, sorted, textures, fonts, GeometryParams{}, geo);
    return geo;
  };
  const FrameGeometry ob = build(ViewMode::Oblique);
  REQUIRE(ob.quad_count() == 3);  // background, front face, top face
  // Front face: from the footprint's front edge (y 370) up by the height.
  CHECK(ob.vertices[4].y == doctest::Approx(370 - 20));
  CHECK(ob.vertices[4 + 2].y == doctest::Approx(370));
  CHECK((ob.vertices[4].color & 0xff) < 255);  // shaded darker than the top
  // Top face: the footprint (y 350..370) lifted by 20.
  CHECK(ob.vertices[8].y == doctest::Approx(350 - 20));
  CHECK(ob.vertices[8 + 2].y == doctest::Approx(370 - 20));
  CHECK((ob.vertices[8].color & 0xff) == 255);
  const FrameGeometry iso = build(ViewMode::Iso);
  CHECK(iso.quad_count() == 4);  // background, two side faces, top
  // A side three times as tall as the top is deep repeats the image three times.
  CHECK(build(ViewMode::Oblique, 60).quad_count() == 5);
}

TEST_CASE("geometry: shadows lie on the ground under the sprite (a block's top when over one)") {
  Atlas atlas(AtlasOptions{.page_size = 256});
  TextureStore textures(nullptr, atlas, false);
  FontStore fonts(atlas, FontId::Bold);
  REQUIRE(fonts.init().ok());
  DrawList list;
  list.view = ViewMode::Oblique;
  list.camera = DrawList::default_camera(1280, 720);
  SpriteCmd s = sprite_at(640, 360, 50);
  s.size = {20, 20};
  s.shadow = true;
  s.ground_z = 24;
  list.add(s);
  SortedDrawList sorted;
  sort_draw_list(list, sorted);
  FrameGeometry geo;
  build_geometry(list, sorted, textures, fonts, GeometryParams{}, geo);
  REQUIRE(geo.quad_count() == 3);
  CHECK((geo.vertices[4].y + geo.vertices[4 + 2].y) / 2 == doctest::Approx(360 - 24));
  s.z = 24;  // standing on it: no shadow
  CHECK_FALSE(casts_shadow(s));
}

// ---------------------------------------------------------------- style

TEST_CASE("style: colors and manifest render settings (engine-web rules)") {
  auto c = parse_hex_color("#ff8000");
  REQUIRE(c);
  CHECK(c->r == doctest::Approx(1));
  CHECK(c->g == doctest::Approx(128 / 255.0));
  CHECK(c->a == 1);
  CHECK(parse_hex_color("#fff")->b == doctest::Approx(1));
  CHECK(parse_hex_color("#00000080")->a == doctest::Approx(128 / 255.0));
  CHECK_FALSE(parse_hex_color("red"));
  CHECK_FALSE(parse_hex_color("#12345"));

  // feature-demo: portrait, pixel preset, palette[0] #1a1c2c, [ui] background #1a1c2c, font pixel.
  auto pkg = load_demo();
  const RenderStyle s = style_from_manifest(pkg->manifest);
  CHECK(s.ref_w == 720);
  CHECK(s.ref_h == 1280);
  CHECK(s.nearest);
  CHECK(s.pixel_snap);
  CHECK(s.clear_color.r == doctest::Approx(0x1a / 255.0));
  CHECK(s.letterbox_color.b == doctest::Approx(0x2c / 255.0));
  CHECK(s.theme_font == FontId::Pixel);

  pkg::Manifest m;
  m.style.preset = "toon";
  const RenderStyle toon = style_from_manifest(m);
  CHECK(toon.ref_w == 1280);  // landscape by default
  CHECK_FALSE(toon.nearest);
  CHECK(toon.theme_font == FontId::Rounded);
  CHECK(toon.clear_color.r == doctest::Approx(0x20 / 255.0));  // #20232a
}
