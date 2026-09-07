#include "svg_postprocess.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "svg_entry.h"
#include "svg_geometry.h"
#include "svg_output.h"
#include "svg_paint.h"
#include "svg_util.h"

namespace svg_squisher {
namespace {

std::optional<std::pair<double, double>> parse_viewbox_size(const pugi::xml_node& svg_node) {
  if (svg_node.attribute("viewBox")) {
    const std::vector<double> nums = parse_number_list(svg_node.attribute("viewBox").as_string());
    if (nums.size() >= 4) return std::make_pair(nums[2], nums[3]);
  }

  const double w = parse_double_string(svg_node.attribute("width").as_string(), 0.0);
  const double h = parse_double_string(svg_node.attribute("height").as_string(), 0.0);
  if (w > 0.0 && h > 0.0) return std::make_pair(w, h);
  return std::nullopt;
}

struct EntryMeta {
  PathEntry entry;
  PathEntryAnalysis analysis;
  std::optional<BBox> bbox;
  double bbox_ratio = 0.0;
};

}  // namespace

std::vector<PathEntry> prepare_output_paths(const pugi::xml_node& svg_node,
                                            const std::vector<PathEntry>& paths,
                                            bool remove_background) {
  if (!remove_background) return paths;

  const auto view_size = parse_viewbox_size(svg_node);
  const double view_area = view_size ? view_size->first * view_size->second : 0.0;

  std::vector<EntryMeta> metas;
  metas.reserve(paths.size());
  for (const PathEntry& entry : paths) {
    EntryMeta meta;
    meta.entry = entry;
    meta.analysis = analyze_path_entry(entry);
    meta.bbox = path_bbox(entry.d);
    if (meta.bbox && view_area > 0.0) {
      meta.bbox_ratio = (bbox_width(*meta.bbox) * bbox_height(*meta.bbox)) / view_area;
    }
    metas.push_back(std::move(meta));
  }

  const bool has_foreground_details = std::any_of(metas.begin(), metas.end(), [](const EntryMeta& meta) {
    return meta.bbox_ratio > 0.0 && meta.bbox_ratio < 0.25;
  });

  std::vector<EntryMeta> kept;
  kept.reserve(metas.size());
  for (const EntryMeta& meta : metas) {
    const auto& active_paint = meta.entry.emit_fill ? meta.analysis.fill_paint : meta.analysis.stroke_paint;
    const bool is_large_fill = !meta.analysis.is_stroke_shape && meta.entry.emit_fill && meta.bbox_ratio >= 0.35;
    const bool is_background_tone = active_paint.is_gradient || (active_paint.brightness.has_value() && *active_paint.brightness <= 180.0);
    bool has_solid_outline_over_fill = false;
    if (is_large_fill && meta.analysis.has_non_default_opacity) {
      for (const EntryMeta& other : metas) {
        if (&other == &meta) continue;
        if (other.analysis.is_stroke_shape || !other.entry.emit_fill) continue;
        if (other.analysis.opacity < 1.0) continue;
        if (!other.analysis.fill_paint.brightness.has_value() || *other.analysis.fill_paint.brightness <= 180.0) continue;
        if (other.bbox_ratio < meta.bbox_ratio * 0.65) continue;
        if (other.bbox_ratio > meta.bbox_ratio * 1.15) continue;
        if (bbox_contains(other.bbox, meta.bbox)) {
          has_solid_outline_over_fill = true;
          break;
        }
      }
    }
    if (is_large_fill && has_solid_outline_over_fill) continue;
    if (has_foreground_details && is_large_fill && is_background_tone) continue;
    kept.push_back(meta);
  }

  std::vector<PathEntry> final_paths;
  final_paths.reserve(kept.size());
  for (EntryMeta& meta : kept) {
    final_paths.push_back(std::move(meta.entry));
  }
  return final_paths;
}

}  // namespace svg_squisher
