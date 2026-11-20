// dreamsy-bench [package] [--ticks N] [--warmup N] [--json]: CPU time of one player frame, per stage, no GPU.
// Stages: step (Host::step), draw (play::draw_world), sort (sort_draw_list), geometry (build_geometry).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pkg/files.h"
#include "pkg/validate.h"
#include "play/scene_draw.h"
#include "render/atlas.h"
#include "render/draw_list.h"
#include "render/font.h"
#include "render/geometry.h"
#include "render/sort.h"
#include "render/style.h"
#include "render/textures.h"
#include "script/embedded_kits.h"
#include "script/host.h"

using namespace dreamsy;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Per-stage samples (ms per frame), summarized as mean / median / p95 / max.
struct Stage {
  const char* name;
  std::vector<double> samples;

  double mean() const {
    double s = 0;
    for (double v : samples) s += v;
    return samples.empty() ? 0 : s / static_cast<double>(samples.size());
  }
  double percentile(double p) const {
    if (samples.empty()) return 0;
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    size_t i = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[i];
  }
};

int usage() {
  std::fprintf(stderr, "usage: dreamsy-bench [package] [--ticks N] [--warmup N] [--json]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string path = DREAMSY_BENCH_DEFAULT_PACKAGE;
  int ticks = 600, warmup = 60;
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--ticks" && i + 1 < argc) ticks = std::atoi(argv[++i]);
    else if (arg == "--warmup" && i + 1 < argc) warmup = std::atoi(argv[++i]);
    else if (arg == "--json") json = true;
    else if (!arg.empty() && arg[0] != '-') path = arg;
    else return usage();
  }
  if (ticks <= 0 || warmup < 0) return usage();

  script::use_embedded_kits();
  Result<pkg::Files> files = pkg::Files::from_dir(path);
  if (!files) {
    std::fprintf(stderr, "✗ %s\n", files.error().c_str());
    return 1;
  }
  pkg::LoadResult loaded = pkg::load_package(std::move(files).value());
  if (!loaded.ok) {
    std::fprintf(stderr, "✗ %s: the package doesn't validate (run dreamsy-tool validate)\n", path.c_str());
    return 1;
  }
  std::shared_ptr<const pkg::Package> package(std::move(loaded.package));

  script::HostOptions opts;
  opts.on_log = [](const script::LogLine&) {};
  auto created = script::Host::create(package, opts);
  if (!created) {
    std::fprintf(stderr, "✗ %s\n", created.error().c_str());
    return 1;
  }
  script::Host& host = *created.value();

  const render::RenderStyle style = render::style_from_manifest(package->manifest);
  render::Atlas atlas;
  render::TextureStore textures(package.get(), atlas, style.nearest);
  render::FontStore fonts(atlas, style.theme_font);
  if (Status s = fonts.init(); !s) {
    std::fprintf(stderr, "✗ %s\n", s.error().c_str());
    return 1;
  }
  render::DrawList list;
  render::SortedDrawList sorted;
  render::FrameGeometry geometry;
  render::GeometryParams params;
  params.ref_w = style.ref_w;
  params.ref_h = style.ref_h;
  params.pixel_snap = style.pixel_snap;

  Stage step{"step", {}}, draw{"draw", {}}, sort{"sort", {}}, geo{"geometry", {}}, total{"total", {}};
  for (Stage* s : {&step, &draw, &sort, &geo, &total}) s->samples.reserve(static_cast<size_t>(ticks));

  // `frame` times the whole run, so it stays exact where the clock is coarse.
  Clock::time_point run_start;
  for (int i = 0; i < warmup + ticks; ++i) {
    const bool record = i >= warmup;
    if (i == warmup) run_start = Clock::now();
    auto t0 = Clock::now();
    host.step();
    const double t_step = ms_since(t0);
    auto t1 = Clock::now();
    play::draw_world(host, textures, list);
    const double t_draw = ms_since(t1);
    auto t2 = Clock::now();
    render::sort_draw_list(list, sorted);
    const double t_sort = ms_since(t2);
    auto t3 = Clock::now();
    render::build_geometry(list, sorted, textures, fonts, params, geometry);
    const double t_geo = ms_since(t3);
    if (!record) continue;
    step.samples.push_back(t_step);
    draw.samples.push_back(t_draw);
    sort.samples.push_back(t_sort);
    geo.samples.push_back(t_geo);
    total.samples.push_back(t_step + t_draw + t_sort + t_geo);
  }

  const double frame_ms = ms_since(run_start) / ticks;
  const size_t entities = host.world().count();
  const size_t errors = host.errors().size();
  const uint32_t quads = geometry.quad_count();
  const size_t batches = geometry.world.size() + geometry.emissive.size() + geometry.screen.size();
  if (json) {
    std::printf("{\"package\":\"%s\",\"ticks\":%d,\"entities\":%zu,\"quads\":%u,\"batches\":%zu,\"script_errors\":%zu,"
                "\"frame\":%.4f",
                package->manifest.name.c_str(), ticks, entities, quads, batches, errors, frame_ms);
    for (const Stage* s : {&step, &draw, &sort, &geo, &total})
      std::printf(",\"%s\":{\"mean\":%.4f,\"p50\":%.4f,\"p95\":%.4f,\"max\":%.4f}", s->name, s->mean(), s->percentile(0.5),
                  s->percentile(0.95), s->percentile(1.0));
    std::printf("}\n");
  } else {
    std::printf("%s: %zu entities, %u quads in %zu batches, %zu script errors, %d ticks\n",
                package->manifest.name.c_str(), entities, quads, batches, errors, ticks);
    std::printf("%-10s %9s %9s %9s %9s   (ms per frame)\n", "stage", "mean", "p50", "p95", "max");
    for (const Stage* s : {&step, &draw, &sort, &geo, &total})
      std::printf("%-10s %9.3f %9.3f %9.3f %9.3f\n", s->name, s->mean(), s->percentile(0.5), s->percentile(0.95),
                  s->percentile(1.0));
    std::printf("frame      %9.3f   (whole run / ticks: exact even with a coarse clock)\n", frame_ms);
  }
  for (const script::ScriptError& e : host.errors())
    std::fprintf(stderr, "script error %s:%d: %s\n", e.script.c_str(), e.line, e.message.c_str());
  return errors == 0 ? 0 : 1;
}
