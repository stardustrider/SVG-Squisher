#include "svg_output.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "svg_entry.h"
#include "svg_geometry.h"
#include "svg_paint.h"
#include "svg_path.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace fs = std::filesystem;

namespace svg_squisher {
namespace {

void collect_referenced_def_ids(const std::vector<PathEntry>& paths,
                                std::unordered_set<std::string>& referenced_ids) {
  for (const PathEntry& entry : paths) {
    const PathEntryAnalysis analysis = analyze_path_entry(entry);
    if (entry.emit_fill && analysis.fill_paint.is_gradient) {
      if (const auto id = parse_paint(entry.fill).url_id) referenced_ids.insert(*id);
    }
    if (entry.emit_stroke && analysis.stroke_paint.is_gradient) {
      if (const auto id = parse_paint(entry.stroke).url_id) referenced_ids.insert(*id);
    }
  }
}

}  // namespace

std::string xml_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

std::vector<std::string> collect_serialized_defs(const pugi::xml_node& svg_root,
                                                 const std::vector<PathEntry>& paths) {
  std::unordered_set<std::string> referenced_ids;
  collect_referenced_def_ids(paths, referenced_ids);
  if (referenced_ids.empty()) return {};

  std::vector<std::string> serialized_defs;
  for (const pugi::xpath_node& defs_match : svg_root.select_nodes(".//defs")) {
    const pugi::xml_node defs = defs_match.node();
    for (const pugi::xml_node& child : defs.children()) {
      if (child.type() == pugi::node_element) {
        std::ostringstream buffer;
        child.print(buffer, "  ", pugi::format_default, pugi::encoding_utf8);
        serialized_defs.push_back(buffer.str());
      }
    }
  }
  return serialized_defs;
}

std::string render_path_entry(const PathEntry& entry, const std::optional<std::string>& fill_override) {
  std::ostringstream out;
  const bool flatten_mode = fill_override.has_value();
  const PathEntryAnalysis analysis = analyze_path_entry(entry);

  if (entry.emit_fill) {
    out << "  <path d=\"" << xml_escape(entry.d) << "\"";
    if (!entry.transform.empty()) out << " transform=\"" << xml_escape(entry.transform) << "\"";
    out << " fill=\"" << xml_escape(fill_override.value_or(analysis.fill_paint.value)) << "\"";
    if (!analysis.fill_rule.empty() && (analysis.fill_rule != "nonzero" || flatten_mode)) {
      out << " fill-rule=\"" << xml_escape(analysis.fill_rule) << "\"";
    } else if (flatten_mode) {
      out << " fill-rule=\"nonzero\"";
    }
    if (!flatten_mode && analysis.has_non_default_opacity) {
      out << " opacity=\"" << xml_escape(entry.opacity) << "\"";
    }
    out << "/>\n";
  }

  if (entry.emit_stroke) {
    out << "  <path d=\"" << xml_escape(entry.d) << "\"";
    if (!entry.transform.empty()) out << " transform=\"" << xml_escape(entry.transform) << "\"";
    out << " fill=\"none\"";
    out << " stroke=\"" << xml_escape(fill_override.value_or(analysis.stroke_paint.value)) << "\"";
    out << " stroke-width=\"" << xml_escape(analysis.stroke_width) << "\"";
    if (!analysis.stroke_dasharray.empty() && lower_copy(analysis.stroke_dasharray) != "none") {
      out << " stroke-dasharray=\"" << xml_escape(analysis.stroke_dasharray) << "\"";
    }
    const std::string linecap = to_string(analysis.stroke_linecap);
    if (!linecap.empty() && linecap != "butt" && linecap != "undefined") {
      out << " stroke-linecap=\"" << xml_escape(linecap) << "\"";
    }
    const std::string linejoin = to_string(analysis.stroke_linejoin);
    if (!linejoin.empty() && linejoin != "miter" && linejoin != "undefined") {
      out << " stroke-linejoin=\"" << xml_escape(linejoin) << "\"";
    }
    if (!analysis.stroke_miterlimit.empty() && analysis.stroke_miterlimit != "4") {
      out << " stroke-miterlimit=\"" << xml_escape(analysis.stroke_miterlimit) << "\"";
    }
    if (!flatten_mode && analysis.has_non_default_opacity) {
      out << " opacity=\"" << xml_escape(entry.opacity) << "\"";
    }
    out << "/>\n";
  }

  return out.str();
}

std::string render_svg_document(const pugi::xml_node& svg_root,
                                const std::vector<PathEntry>& paths,
                                const std::optional<std::string>& fill_override) {
  // Parse viewBox to detect non-zero origin that needs normalization.
  double vb_min_x = 0.0, vb_min_y = 0.0, vb_w = 0.0, vb_h = 0.0;
  bool has_viewbox = false;
  bool needs_viewbox_normalize = false;
  if (svg_root.attribute("viewBox")) {
    const std::vector<double> vb = parse_number_list(svg_root.attribute("viewBox").as_string());
    if (vb.size() >= 4) {
      vb_min_x = vb[0];
      vb_min_y = vb[1];
      vb_w = vb[2];
      vb_h = vb[3];
      has_viewbox = true;
      needs_viewbox_normalize = (std::abs(vb_min_x) > 1e-6 || std::abs(vb_min_y) > 1e-6);
    }
  }

  // If viewBox has non-zero origin, translate all paths so the viewBox can start at 0,0.
  std::vector<PathEntry> normalized_paths;
  const std::vector<PathEntry>& output_paths = needs_viewbox_normalize ? normalized_paths : paths;
  if (needs_viewbox_normalize) {
    const std::string translate_str = "translate(" + fmt(-vb_min_x) + " " + fmt(-vb_min_y) + ")";
    const Matrix translate_matrix = parse_transform(translate_str);
    normalized_paths.reserve(paths.size());
    for (const PathEntry& entry : paths) {
      PathEntry shifted = entry;
      if (shifted.transform.empty()) {
        const auto baked = bake_path_transform(shifted.d, translate_matrix);
        if (baked) {
          shifted.d = *baked;
        } else {
          shifted.transform = translate_str;
        }
      } else {
        shifted.transform = translate_str + " " + shifted.transform;
      }
      normalized_paths.push_back(std::move(shifted));
    }
  }

  std::ostringstream out;
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\"";
  const std::vector<std::string> serialized_defs = fill_override.has_value()
    ? std::vector<std::string>{}
    : collect_serialized_defs(svg_root, output_paths);
  if (!serialized_defs.empty()) {
    out << " xmlns:xlink=\"http://www.w3.org/1999/xlink\"";
  }
  if (svg_root.attribute("width")) out << " width=\"" << xml_escape(svg_root.attribute("width").as_string()) << "\"";
  if (svg_root.attribute("height")) out << " height=\"" << xml_escape(svg_root.attribute("height").as_string()) << "\"";
  if (has_viewbox) {
    if (needs_viewbox_normalize) {
      out << " viewBox=\"0 0 " << fmt(vb_w) << " " << fmt(vb_h) << "\"";
    } else {
      out << " viewBox=\"" << xml_escape(svg_root.attribute("viewBox").as_string()) << "\"";
    }
  }
  out << ">\n";

  if (!serialized_defs.empty()) {
    out << "  <defs>\n";
    for (const std::string& def : serialized_defs) {
      out << def;
    }
    out << "  </defs>\n";
  }

  for (const PathEntry& entry : output_paths) {
    out << render_path_entry(entry, fill_override);
  }

  out << "</svg>\n";
  return out.str();
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Unable to open input file: " + path.string());
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

void write_file(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Unable to open output file: " + path.string());
  }
  out << text;
}

}  // namespace svg_squisher

