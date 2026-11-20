// Package images -> atlas regions, loaded on first use (engine-web render/textures.ts).
//
//   ImageId coin = store.find_asset("asset:coin");   // cheap; keep the id
//   std::optional<AtlasRegion> r = store.region(coin, 64);   // decodes + packs the first time
//
// SVG rasterization follows engine-web: pixel-art games rasterize at the SVG's intrinsic size and
// rely on nearest filtering; smooth games rasterize at the on-screen size, bucketed to powers of
// two (32..1024), and re-rasterize when a sprite needs a bigger version.
//
// A missing or broken image logs one warning and region() returns nullopt; the renderer then
// draws a solid rect (engine-web does the same).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "render/atlas.h"
#include "render/draw_list.h"
#include "render/image.h"

namespace dreamsy::pkg {
struct Package;
}

namespace dreamsy::render {

class TextureStore {
 public:
  // `package` may be null (tests, generated images only). It must outlive the store.
  TextureStore(const pkg::Package* package, Atlas& atlas, bool pixel_art);

  // Built-in images, always available.
  static constexpr ImageId kWhite{1};   // solid white (rects, untextured sprites)
  static constexpr ImageId kShadow{2};  // soft round blob (drop shadows)
  static constexpr ImageId kDisc{3};    // hard-edged disc (Particles2D shape = "circle")

  // "asset:<id>" or "<id>" -> an image id. Invalid when the asset doesn't exist or isn't an image.
  ImageId find_asset(std::string_view ref);

  // A runtime image (generated, or decoded elsewhere). Straight alpha; packed immediately.
  Result<ImageId> add_image(std::string name, Image image);

  // Where the image is in the atlas, decoding it if needed. `screen_px` is the longest side the
  // image will cover on screen, in target pixels (used to pick an SVG raster size).
  // nullopt if the image can't be loaded.
  std::optional<AtlasRegion> region(ImageId id, float screen_px = 0);

  // Intrinsic size of an image in pixels (SVG: width/height or viewBox), if it loads.
  std::optional<Vec2> intrinsic_size(ImageId id);

  bool pixel_art() const { return pixel_art_; }
  Atlas& atlas() { return atlas_; }

  // The SVG raster size engine-web would pick (exposed for tests).
  static int svg_raster_bucket(float screen_px);

 private:
  struct Entry {
    std::string name;  // asset id or runtime name (for messages)
    std::string file;  // "assets/<file>"; empty for runtime images
    ImageFormat format = ImageFormat::Unknown;
    bool loaded = false;
    bool failed = false;
    std::optional<Svg> svg;   // parsed once, rasterized per size
    Vec2 size;                // intrinsic size
    int raster_px = 0;        // SVG: longest side of the current raster (0 = intrinsic)
    AtlasRegion region;
  };

  bool load(Entry& e, float screen_px);
  bool fail(Entry& e, const std::string& why);  // logs once; returns false

  const pkg::Package* package_;
  Atlas& atlas_;
  bool pixel_art_;
  std::vector<Entry> entries_;                     // ImageId.value - 1
  // Hashes a string_view, so find_asset() looks names up without building a std::string.
  struct NameHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };
  std::unordered_map<std::string, ImageId, NameHash, std::equal_to<>> by_asset_;
};

}  // namespace dreamsy::render
