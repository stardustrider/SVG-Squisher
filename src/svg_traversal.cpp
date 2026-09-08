#include "svg_traversal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "svg_computed_style.h"
#include "svg_diagnostics.h"
#include "svg_dom.h"
#include "svg_geometry.h"
#include "svg_path.h"
#include "svg_shape.h"
#include "svg_stroke.h"
#include "svg_text.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace svg_squisher {
namespace {

struct TraversalContext {
  const SvgIdIndex* id_index = nullptr;
  std::unordered_set<std::string> active_references;
  std::size_t reference_depth = 0;
  std::size_t expanded_nodes = 1;
  std::size_t emitted_output_paths = 0;
  std::size_t symbol_instance_depth = 0;
  bool resource_budget_exhausted = false;
  bool font_path_is_authoritative = false;
  ConversionPolicy conversion_policy = ConversionPolicy::PreserveAppearance;
  std::vector<std::string>* fonts_used = nullptr;
  std::vector<Diagnostic>* diagnostics = nullptr;
  std::size_t* missing_glyphs = nullptr;
  std::size_t next_compositing_group = 1;
};

std::string traversal_element_label(const pugi::xml_node& node) {
  std::string label = "<" + std::string(node.name());
  if (node.attribute("id")) label += "#" + std::string(node.attribute("id").as_string());
  label += ">";
  return label;
}

void record_traversal_warning(TraversalContext& context,
                              const pugi::xml_node& node,
                              const std::string& code,
                              const std::string& message) {
  if (!context.diagnostics) return;
  const std::string element = traversal_element_label(node);
  const bool duplicate = std::any_of(
      context.diagnostics->begin(), context.diagnostics->end(),
      [&](const Diagnostic& diagnostic) {
        return diagnostic.code == code && diagnostic.element == element;
      });
  if (!duplicate) {
    context.diagnostics->push_back(
        {DiagnosticSeverity::Warning, code, message, element});
  }
}

void record_live_stroke_retention(TraversalContext& context,
                                  const pugi::xml_node& node,
                                  const std::string& reason) {
  if (context.conversion_policy != ConversionPolicy::FilledPaths) return;
  record_traversal_warning(
      context,
      node,
      "live-stroke-retained",
      reason + " Compatible conversion retained the stroke as live SVG stroke attributes; "
               "strict conversion rejects this fallback.");
}

bool begin_expanded_node(const pugi::xml_node& node,
                         std::size_t expanded_depth,
                         TraversalContext& context) {
  if (context.resource_budget_exhausted) return false;
  if (expanded_depth > kMaxExpandedTraversalDepth) {
    record_traversal_warning(
        context,
        node,
        "expanded-traversal-depth-limit",
        "Expanded use traversal exceeds the combined 256-element safety depth; the reference branch was truncated.");
    return false;
  }
  if (context.expanded_nodes >= kMaxExpandedNodeCount) {
    record_traversal_warning(
        context,
        node,
        "expanded-node-limit",
        "Expanded use traversal exceeds the 16384-element visit limit; remaining referenced content was skipped.");
    context.resource_budget_exhausted = true;
    return false;
  }
  ++context.expanded_nodes;
  return true;
}

bool reserve_output_paths(const pugi::xml_node& node,
                          std::size_t requested_paths,
                          TraversalContext& context) {
  if (context.resource_budget_exhausted) return false;
  if (requested_paths <= kMaxOutputPathCount - context.emitted_output_paths) {
    context.emitted_output_paths += requested_paths;
    return true;
  }
  record_traversal_warning(
      context,
      node,
      "output-path-limit",
      "Conversion reached the 8192-output-path limit; remaining content was skipped.");
  context.resource_budget_exhausted = true;
  return false;
}

void record_font(TraversalContext const& context, const std::string& font_path) {
  if (!context.fonts_used) return;
  if (std::find(context.fonts_used->begin(), context.fonts_used->end(), font_path) ==
      context.fonts_used->end()) {
    context.fonts_used->push_back(font_path);
  }
}

std::string format_codepoint(char32_t codepoint) {
  std::ostringstream out;
  out << "U+" << std::uppercase << std::hex << std::setfill('0')
      << std::setw(codepoint <= 0xffff ? 4 : 6) << static_cast<std::uint32_t>(codepoint);
  return out.str();
}

std::string format_number(double value) {
  if (!std::isfinite(value)) {
    throw std::runtime_error("Conversion produced a non-finite numeric value");
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(15) << value;
  std::string formatted = out.str();
  while (!formatted.empty() && formatted.back() == '0') formatted.pop_back();
  if (!formatted.empty() && formatted.back() == '.') formatted.pop_back();
  return formatted.empty() ? "0" : formatted;
}

std::string format_opacity(double opacity) {
  return format_number(std::clamp(opacity, 0.0, 1.0));
}

void normalize_numeric_style(StyleState& style) {
  const ComputedStyle computed = compute_style(style);
  style.opacity = format_opacity(computed.opacity);
  style.fill_opacity = format_opacity(computed.fill_opacity);
  style.stroke_opacity = format_opacity(computed.stroke_opacity);
  style.stroke_width = format_number(computed.stroke_width);
  style.stroke_miterlimit = format_number(computed.stroke_miterlimit);
  style.font_size = format_number(computed.font_size);
  style.letter_spacing = format_number(computed.letter_spacing);
}

void preserve_source_element_opacity(std::vector<PathEntry>& paths,
                                     std::size_t first_entry,
                                     double opacity,
                                     TraversalContext& context) {
  if (first_entry >= paths.size() || opacity >= 1.0) return;

  std::size_t painted_paths = 0;
  for (std::size_t index = first_entry; index < paths.size(); ++index) {
    painted_paths += paths[index].emit_fill ? 1U : 0U;
    painted_paths += paths[index].emit_stroke ? 1U : 0U;
  }
  if (painted_paths < 2) return;

  const std::size_t group = context.next_compositing_group++;
  for (std::size_t index = first_entry; index < paths.size(); ++index) {
    paths[index].compositing_group = group;
  }
}

void record_missing_glyphs(const TraversalContext& context,
                           const pugi::xml_node& node,
                           const std::string& font_path,
                           const std::vector<char32_t>& codepoints) {
  if (context.missing_glyphs) *context.missing_glyphs += codepoints.size();
  if (!context.diagnostics) return;

  std::string element = "<" + std::string(node.name());
  if (node.attribute("id")) element += "#" + std::string(node.attribute("id").as_string());
  element += ">";
  for (const char32_t codepoint : codepoints) {
    const std::string label = format_codepoint(codepoint);
    const std::string message =
      "Font '" + font_path + "' has no glyph for " + label + "; the .notdef glyph was emitted.";
    const bool duplicate = std::any_of(
      context.diagnostics->begin(), context.diagnostics->end(),
      [&](const Diagnostic& diagnostic) {
        return diagnostic.code == "missing-glyph" && diagnostic.message == message;
      });
    if (!duplicate) {
      context.diagnostics->push_back(
        {DiagnosticSeverity::Warning, "missing-glyph", message, element});
    }
  }
}

struct TextWhitespaceState {
  bool has_non_space = false;
  bool previous_was_space = true;
};

struct TextPositionState {
  std::vector<double> x;
  std::vector<double> y;
  std::vector<double> dx;
  std::vector<double> dy;
  std::size_t character_index = 0;
  std::optional<double> first_x_override;
};

enum class TextPositionProperty {
  X,
  Y,
  Dx,
  Dy,
};

struct TextPositionRun {
  std::vector<double> x;
  std::vector<double> y;
  std::vector<double> dx;
  std::vector<double> dy;
};

const std::vector<double>& position_values(const TextPositionState& state,
                                           TextPositionProperty property) {
  switch (property) {
    case TextPositionProperty::X: return state.x;
    case TextPositionProperty::Y: return state.y;
    case TextPositionProperty::Dx: return state.dx;
    case TextPositionProperty::Dy: return state.dy;
  }
  return state.x;
}

std::optional<double> resolve_position_value(
    const std::vector<TextPositionState*>& stack,
    TextPositionProperty property) {
  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    const TextPositionState& state = **it;
    const std::vector<double>& values = position_values(state, property);
    if (state.character_index >= values.size()) continue;
    if (property == TextPositionProperty::X && state.character_index == 0 &&
        state.first_x_override.has_value()) {
      return state.first_x_override;
    }
    return values[state.character_index];
  }
  return std::nullopt;
}

void trim_unset_positions(std::vector<double>& values) {
  while (!values.empty() && !std::isfinite(values.back())) values.pop_back();
}

TextPositionRun consume_text_positions(std::vector<TextPositionState*>& stack,
                                       std::size_t character_count) {
  TextPositionRun run;
  const double unset = std::numeric_limits<double>::quiet_NaN();
  run.x.reserve(character_count);
  run.y.reserve(character_count);
  run.dx.reserve(character_count);
  run.dy.reserve(character_count);

  for (std::size_t index = 0; index < character_count; ++index) {
    const auto append = [unset](std::vector<double>& values,
                                const std::optional<double>& value) {
      values.push_back(value.value_or(unset));
    };
    append(run.x, resolve_position_value(stack, TextPositionProperty::X));
    append(run.y, resolve_position_value(stack, TextPositionProperty::Y));
    append(run.dx, resolve_position_value(stack, TextPositionProperty::Dx));
    append(run.dy, resolve_position_value(stack, TextPositionProperty::Dy));
    for (TextPositionState* state : stack) ++state->character_index;
  }

  trim_unset_positions(run.x);
  trim_unset_positions(run.y);
  trim_unset_positions(run.dx);
  trim_unset_positions(run.dy);
  return run;
}

bool preserve_whitespace_for_node(const pugi::xml_node& node, bool inherited) {
  const pugi::xml_attribute xml_space = node.attribute("xml:space");
  if (!xml_space) return inherited;
  const std::string value = lower_copy(trim(xml_space.as_string()));
  if (value == "preserve") return true;
  if (value == "default") return false;
  return inherited;
}

std::string normalize_text_whitespace(const std::string& raw,
                                      bool preserve,
                                      TextWhitespaceState& state) {
  std::string normalized;
  normalized.reserve(raw.size());
  for (const unsigned char ch : raw) {
    const bool is_space = ch < 0x80 && std::isspace(ch);
    if (is_space) {
      if (preserve) {
        normalized.push_back(' ');
        state.previous_was_space = true;
      } else if (state.has_non_space && !state.previous_was_space) {
        normalized.push_back(' ');
        state.previous_was_space = true;
      }
      continue;
    }

    normalized.push_back(static_cast<char>(ch));
    state.has_non_space = true;
    state.previous_was_space = false;
  }
  return normalized;
}

bool text_styles_match(const StyleState& lhs, const StyleState& rhs) {
  return lhs.fill == rhs.fill &&
         lhs.fill_opacity == rhs.fill_opacity &&
         lhs.stroke == rhs.stroke &&
         lhs.stroke_opacity == rhs.stroke_opacity &&
         lhs.stroke_width == rhs.stroke_width &&
         lhs.stroke_dasharray == rhs.stroke_dasharray &&
         lhs.stroke_linecap == rhs.stroke_linecap &&
         lhs.stroke_linejoin == rhs.stroke_linejoin &&
         lhs.stroke_miterlimit == rhs.stroke_miterlimit &&
         lhs.fill_rule == rhs.fill_rule &&
         lhs.opacity == rhs.opacity &&
         lhs.display == rhs.display &&
         lhs.visibility == rhs.visibility &&
         lhs.font_size == rhs.font_size &&
         lhs.font_family == rhs.font_family &&
         lhs.font_weight == rhs.font_weight &&
         lhs.font_style == rhs.font_style &&
         lhs.text_anchor == rhs.text_anchor &&
         lhs.letter_spacing == rhs.letter_spacing;
}

struct CompatibleTspanText {
  std::string text;
  TextWhitespaceState whitespace;
  std::size_t element_count = 0;
  std::size_t maximum_relative_depth = 0;
};

bool has_only_shape_transparent_tspan_attributes(
    const pugi::xml_node& node) {
  for (const pugi::xml_attribute attribute : node.attributes()) {
    const std::string name = attribute.name();
    if (name != "id" && name != "class") return false;
  }
  return true;
}

bool collect_compatible_tspan_text(const pugi::xml_node& node,
                                   const std::vector<CssRule>& rules,
                                   const StyleState& inherited_style,
                                   bool preserve,
                                   std::size_t relative_depth,
                                   CompatibleTspanText& result) {
  if (std::string(node.name()) != "tspan" ||
      !has_only_shape_transparent_tspan_attributes(node)) {
    return false;
  }

  const StyleState style = resolve_style(node, rules, inherited_style);
  if (!text_styles_match(style, inherited_style) ||
      node_has_local_property(node, rules, "opacity")) {
    return false;
  }

  const std::size_t original_text_size = result.text.size();
  const TextWhitespaceState original_whitespace = result.whitespace;
  const std::size_t original_element_count = result.element_count;
  const std::size_t original_maximum_depth = result.maximum_relative_depth;
  const auto rollback = [&]() {
    result.text.resize(original_text_size);
    result.whitespace = original_whitespace;
    result.element_count = original_element_count;
    result.maximum_relative_depth = original_maximum_depth;
  };

  ++result.element_count;
  result.maximum_relative_depth =
      std::max(result.maximum_relative_depth, relative_depth);
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
      result.text += normalize_text_whitespace(
          child.value(), preserve, result.whitespace);
      continue;
    }
    if (child.type() == pugi::node_element &&
        collect_compatible_tspan_text(
            child, rules, style, preserve, relative_depth + 1, result)) {
      continue;
    }
    if (child.type() == pugi::node_element) {
      rollback();
      return false;
    }
  }

  return true;
}

void append_utf8_codepoint(std::string& text, char32_t codepoint) {
  if (codepoint <= 0x7f) {
    text.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    text.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    text.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    text.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

struct AnchorMeasureRun {
  std::string text;
  std::string font_path;
  StyleState style;
  std::string transform;
  bool preserve_whitespace = false;
};

struct AnchorMeasurement {
  std::vector<AnchorMeasureRun> runs;
  bool has_character = false;
  bool reached_next_chunk = false;
  bool merge_barrier = false;
};

void append_anchor_measure_run(AnchorMeasurement& measurement,
                               std::string text,
                               const std::optional<std::string>& font_path,
                               const StyleState& style,
                               const std::string& transform,
                               bool preserve_whitespace) {
  if (text.empty()) return;
  if (!font_path) {
    measurement.merge_barrier = true;
    return;
  }

  if (!measurement.merge_barrier && !measurement.runs.empty()) {
    AnchorMeasureRun& previous = measurement.runs.back();
    if (previous.font_path == *font_path &&
        previous.transform == transform &&
        previous.preserve_whitespace == preserve_whitespace &&
        text_styles_match(previous.style, style)) {
      previous.text += text;
      return;
    }
  }

  measurement.runs.push_back(
    {std::move(text), *font_path, style, transform, preserve_whitespace});
  measurement.merge_barrier = false;
}

void collect_anchor_measure_runs(
    const pugi::xml_node& node,
    const std::vector<CssRule>& rules,
    const StyleState& style,
    const StyleState& effective_style,
    const std::string& transform,
    const std::optional<std::string>& font_path,
    const std::optional<std::string>& fallback_font_path,
    bool font_path_is_authoritative,
    bool preserve,
    TextWhitespaceState& whitespace,
    std::vector<TextPositionState*>& position_stack,
    AnchorMeasurement& measurement) {
  const ComputedStyle computed = compute_style(effective_style);
  for (const pugi::xml_node child : node.children()) {
    if (measurement.reached_next_chunk) return;
    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
      const std::string normalized =
        normalize_text_whitespace(child.value(), preserve, whitespace);
      std::string included;
      for (const char32_t codepoint : decode_utf8(normalized)) {
        const bool has_absolute_position =
          resolve_position_value(position_stack, TextPositionProperty::X).has_value() ||
          resolve_position_value(position_stack, TextPositionProperty::Y).has_value();
        if (measurement.has_character && has_absolute_position) {
          measurement.reached_next_chunk = true;
          break;
        }
        append_utf8_codepoint(included, codepoint);
        measurement.has_character = true;
        for (TextPositionState* position : position_stack) {
          ++position->character_index;
        }
      }
      append_anchor_measure_run(
        measurement, std::move(included), font_path, effective_style, transform, preserve);
      continue;
    }

    if (child.type() != pugi::node_element) continue;
    const std::string child_name = child.name();
    if (child_name != "tspan" && child_name != "textPath") continue;

    const StyleState child_style = resolve_style(child, rules, style);
    StyleState child_effective_style = child_style;
    child_effective_style.opacity = format_opacity(
      computed.opacity * parse_double_string(child_style.opacity, 1.0));
    normalize_numeric_style(child_effective_style);
    if (!compute_style(child_effective_style).displayed) continue;

    TextPositionState child_position;
    child_position.x = coord_values(child, "x");
    child_position.y = coord_values(child, "y");
    child_position.dx = coord_values(child, "dx");
    child_position.dy = coord_values(child, "dy");
    position_stack.push_back(&child_position);

    const std::string child_transform =
      combine_transform(transform, child.attribute("transform").as_string());
    const bool child_preserve = preserve_whitespace_for_node(child, preserve);
    const std::optional<std::string> child_font_path = resolve_text_font_path(
      child_style, fallback_font_path, font_path_is_authoritative);
    collect_anchor_measure_runs(
      child,
      rules,
      child_style,
      child_effective_style,
      child_transform,
      child_font_path,
      fallback_font_path,
      font_path_is_authoritative,
      child_preserve,
      whitespace,
      position_stack,
      measurement);
    position_stack.pop_back();
  }
}

double measure_anchor_chunk_advance(
    const pugi::xml_node& node,
    const std::vector<CssRule>& rules,
    const StyleState& style,
    const StyleState& effective_style,
    const std::string& transform,
    const std::optional<std::string>& font_path,
    const std::optional<std::string>& fallback_font_path,
    bool font_path_is_authoritative,
    bool preserve,
    const TextWhitespaceState& whitespace,
    const std::vector<TextPositionState*>& position_stack) {
  std::vector<TextPositionState> copied_positions;
  copied_positions.reserve(position_stack.size());
  for (const TextPositionState* position : position_stack) {
    copied_positions.push_back(*position);
  }
  std::vector<TextPositionState*> copied_stack;
  copied_stack.reserve(copied_positions.size());
  for (TextPositionState& position : copied_positions) {
    copied_stack.push_back(&position);
  }

  TextWhitespaceState copied_whitespace = whitespace;
  AnchorMeasurement measurement;
  collect_anchor_measure_runs(
    node,
    rules,
    style,
    effective_style,
    transform,
    font_path,
    fallback_font_path,
    font_path_is_authoritative,
    preserve,
    copied_whitespace,
    copied_stack,
    measurement);

  double advance = 0.0;
  for (const AnchorMeasureRun& run : measurement.runs) {
    const ComputedStyle run_computed = compute_style(run.style);
    advance += measure_text_advance(
      run.text,
      run_computed.font_size,
      run.font_path,
      run_computed.letter_spacing);
  }
  return advance;
}

StyleState stroke_as_fill_style(const StyleState& style) {
  StyleState outline_style = style;
  outline_style.fill_opacity = style.stroke_opacity;
  return outline_style;
}

struct SymbolViewportMapping {
  std::string transform;
  double width = 0.0;
  double height = 0.0;
  bool uses_root_viewport_fallback = false;
};

std::pair<double, double> root_user_viewport_size(
    const pugi::xml_node& svg_root) {
  if (const auto viewbox =
        parse_viewbox(svg_root.attribute("viewBox").as_string())) {
    if ((*viewbox)[2] >= 0.0 && (*viewbox)[3] >= 0.0) {
      return {(*viewbox)[2], (*viewbox)[3]};
    }
  }

  const auto root_dimension = [&](const char* attribute,
                                  double initial_value) {
    double value = 0.0;
    return svg_root.attribute(attribute) &&
        parse_finite_length(svg_root.attribute(attribute).as_string(), value) &&
        value >= 0.0
      ? value
      : initial_value;
  };
  return {root_dimension("width", 300.0), root_dimension("height", 150.0)};
}

std::optional<double> supported_viewport_dimension(
    const pugi::xml_node& node,
    const char* attribute) {
  if (!node.attribute(attribute)) return std::nullopt;
  double value = 0.0;
  if (!parse_finite_length(node.attribute(attribute).as_string(), value) ||
      value < 0.0) {
    return std::nullopt;
  }
  return value;
}

std::optional<SymbolViewportMapping> symbol_viewbox_mapping(
    const pugi::xml_node& use_node,
    const pugi::xml_node& symbol_node,
    const pugi::xml_node& svg_root) {
  const auto [root_width, root_height] = root_user_viewport_size(svg_root);
  const std::optional<double> symbol_width =
    supported_viewport_dimension(symbol_node, "width");
  const std::optional<double> symbol_height =
    supported_viewport_dimension(symbol_node, "height");
  const std::optional<double> use_width =
    supported_viewport_dimension(use_node, "width");
  const std::optional<double> use_height =
    supported_viewport_dimension(use_node, "height");
  const double viewport_width =
    use_width.value_or(symbol_width.value_or(root_width));
  const double viewport_height =
    use_height.value_or(symbol_height.value_or(root_height));
  const bool uses_root_viewport_fallback =
    (!use_width && !symbol_width) || (!use_height && !symbol_height);
  if (viewport_width <= 0.0 || viewport_height <= 0.0) return std::nullopt;

  const auto parsed_viewbox =
    parse_viewbox(symbol_node.attribute("viewBox").as_string());
  if (!parsed_viewbox) {
    return SymbolViewportMapping{
      "", viewport_width, viewport_height, uses_root_viewport_fallback};
  }
  const auto& viewbox = *parsed_viewbox;

  const double min_x = viewbox[0];
  const double min_y = viewbox[1];
  const double viewbox_width = viewbox[2];
  const double viewbox_height = viewbox[3];
  if (viewbox_width <= 0.0 || viewbox_height <= 0.0) return std::nullopt;

  std::string align = "xmidymid";
  std::string meet_or_slice = "meet";
  std::istringstream preserve_stream(symbol_node.attribute("preserveAspectRatio").as_string());
  std::string token;
  if (preserve_stream >> token) {
    token = lower_copy(token);
    if (token == "defer" && (preserve_stream >> token)) token = lower_copy(token);
    align = token;
    if (preserve_stream >> token) meet_or_slice = lower_copy(token);
  }

  double scale_x = viewport_width / viewbox_width;
  double scale_y = viewport_height / viewbox_height;
  double offset_x = 0.0;
  double offset_y = 0.0;
  if (align != "none") {
    const double uniform_scale = meet_or_slice == "slice"
      ? std::max(scale_x, scale_y)
      : std::min(scale_x, scale_y);
    scale_x = uniform_scale;
    scale_y = uniform_scale;
    const double remaining_x = viewport_width - viewbox_width * uniform_scale;
    const double remaining_y = viewport_height - viewbox_height * uniform_scale;
    if (align.find("xmid") != std::string::npos) offset_x = remaining_x / 2.0;
    else if (align.find("xmax") != std::string::npos) offset_x = remaining_x;
    if (align.find("ymid") != std::string::npos) offset_y = remaining_y / 2.0;
    else if (align.find("ymax") != std::string::npos) offset_y = remaining_y;
  }

  return SymbolViewportMapping{
    "translate(" + fmt(offset_x) + " " + fmt(offset_y) + ") scale(" +
      fmt(scale_x) + " " + fmt(scale_y) + ") translate(" + fmt(-min_x) + " " +
      fmt(-min_y) + ")",
    viewport_width,
    viewport_height,
    uses_root_viewport_fallback};
}

std::optional<BBox> transformed_bbox(const BBox& source,
                                     const Matrix& transform) {
  BBox result{
    std::numeric_limits<double>::infinity(),
    std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(),
  };
  for (const Point point : {
         Point{source.min_x, source.min_y},
         Point{source.max_x, source.min_y},
         Point{source.max_x, source.max_y},
         Point{source.min_x, source.max_y}}) {
    bbox_add_point(result, apply_matrix(transform, point));
  }
  return bbox_valid(result) ? std::optional<BBox>(result) : std::nullopt;
}

enum class SymbolClipProof {
  Contained,
  Outside,
  Unprovable,
};

SymbolClipProof prove_symbol_paths_inside_viewport(
    const std::vector<PathEntry>& paths,
    std::size_t first_path,
    const std::string& use_transform,
    const SymbolViewportMapping& viewport) {
  if (first_path >= paths.size()) return SymbolClipProof::Contained;
  if (!transform_is_valid(use_transform)) return SymbolClipProof::Unprovable;
  const Matrix clip_transform = parse_transform(use_transform);
  if (!matrix_is_scale_translate_only(clip_transform)) {
    return SymbolClipProof::Unprovable;
  }

  const BBox local_viewport{0.0, 0.0, viewport.width, viewport.height};
  const std::optional<BBox> clip_bounds =
    transformed_bbox(local_viewport, clip_transform);
  if (!clip_bounds) return SymbolClipProof::Unprovable;

  for (std::size_t index = first_path; index < paths.size(); ++index) {
    const PathEntry& path = paths[index];
    if (path.emit_stroke) return SymbolClipProof::Unprovable;
    const std::optional<BBox> local_bounds = path_bbox(path.d);
    if (!local_bounds || !transform_is_valid(path.transform)) {
      return SymbolClipProof::Unprovable;
    }
    const std::optional<BBox> output_bounds =
      transformed_bbox(*local_bounds, parse_transform(path.transform));
    if (!bbox_contains(clip_bounds, output_bounds, 1e-9)) {
      return SymbolClipProof::Outside;
    }
  }
  return SymbolClipProof::Contained;
}

std::size_t collect_text_node(const pugi::xml_node& node,
                              const pugi::xml_node& svg_root,
                              const std::vector<CssRule>& rules,
                              const StyleState& inherited,
                              const std::string& parent_transform,
                              const std::optional<std::string>& fallback_font_path,
                              std::vector<PathEntry>& out_paths,
                              TextCursor& cursor,
                              TextWhitespaceState& whitespace,
                              std::vector<TextPositionState*>& position_stack,
                              bool inherited_preserve,
                              double ancestor_opacity,
                              std::size_t expanded_depth,
                              TraversalContext& context) {
  (void)svg_root;
  if (node.type() != pugi::node_element) return 0;

  const StyleState style = resolve_style(node, rules, inherited);
  StyleState effective_style = style;
  effective_style.opacity = format_opacity(
    ancestor_opacity * parse_double_string(style.opacity, 1.0));
  normalize_numeric_style(effective_style);
  const ComputedStyle computed = compute_style(effective_style);
  if (!computed.displayed) return 0;

  const std::string transform =
    combine_transform(parent_transform, node.attribute("transform").as_string());
  const bool preserve = preserve_whitespace_for_node(node, inherited_preserve);
  TextPositionState position_state;
  position_state.x = coord_values(node, "x");
  position_state.y = coord_values(node, "y");
  position_state.dx = coord_values(node, "dx");
  position_state.dy = coord_values(node, "dy");
  position_stack.push_back(&position_state);

  if (!cursor.has_x) {
    cursor.x = 0.0;
    cursor.has_x = true;
  }
  if (!cursor.has_y) {
    cursor.y = 0.0;
    cursor.has_y = true;
  }

  const TextFontResolution font_resolution = resolve_text_font(
    style, fallback_font_path, context.font_path_is_authoritative);
  const std::optional<std::string>& text_font_path = font_resolution.path;
  const bool report_family_resolution =
    std::string(node.name()) == "text" ||
    node_has_local_property(node, rules, "font-family");
  if (report_family_resolution && font_resolution.requested_family_unresolved) {
    const std::string requested_family = trim(style.font_family);
    if (font_resolution.used_fallback && text_font_path) {
      record_traversal_warning(
        context,
        node,
        "font-family-substituted",
        "Requested font-family '" + requested_family +
          "' could not be resolved; compatible conversion used fallback font '" +
          *text_font_path + "'. Pass --font to select an authoritative font.");
    } else {
      record_traversal_warning(
        context,
        node,
        "font-family-unresolved",
        "Requested font-family '" + requested_family +
          "' could not be resolved and no fallback font is available; compatible "
          "conversion skipped this text. Provide --font with a readable font file.");
    }
  }

  const bool establishes_anchor_chunk = std::string(node.name()) == "text" ||
    !position_state.x.empty() || !position_state.y.empty();
  if (establishes_anchor_chunk &&
      (computed.text_anchor == TextAnchorMode::Middle ||
       computed.text_anchor == TextAnchorMode::End)) {
    const double advance = measure_anchor_chunk_advance(
      node,
      rules,
      style,
      effective_style,
      transform,
      text_font_path,
      fallback_font_path,
      context.font_path_is_authoritative,
      preserve,
      whitespace,
      position_stack);
    cursor.x = (position_state.x.empty() ? cursor.x : position_state.x.front()) -
      (computed.text_anchor == TextAnchorMode::Middle ? advance / 2.0 : advance);
    if (!position_state.x.empty()) position_state.first_x_override = cursor.x;
  }

  std::size_t total_characters = 0;
  const auto emit_text_run = [&](const std::string& text) {
    const std::size_t character_count = decode_utf8(text).size();
    const TextPositionRun positions =
      consume_text_positions(position_stack, character_count);
    if (!text.empty() && text_font_path.has_value()) {
      record_font(context, *text_font_path);
      const TextLayoutResult text_layout = text_to_path(
        text,
        cursor.x,
        cursor.y,
        computed.font_size,
        *text_font_path,
        computed.letter_spacing,
        positions.x,
        positions.y,
        positions.dx,
        positions.dy);
      record_missing_glyphs(
        context, node, *text_font_path, text_layout.missing_codepoints);
      const std::size_t emitted_text_paths =
          (computed.has_fill ? 1U : 0U) + (computed.has_stroke ? 1U : 0U);
      if (computed.visible && !text_layout.d.empty() && emitted_text_paths != 0 &&
          reserve_output_paths(node, emitted_text_paths, context)) {
        if (computed.has_stroke) {
          record_live_stroke_retention(
              context,
              node,
              "Filled-path conversion does not outline strokes applied to converted text geometry.");
        }
        const std::size_t first_entry = out_paths.size();
        append_path_entry(
          out_paths,
          text_layout.d,
          transform,
          effective_style.fill,
          effective_style.stroke,
          effective_style,
          computed.has_fill,
          computed.has_stroke);
        preserve_source_element_opacity(
          out_paths, first_entry, computed.opacity, context);
      }
      cursor.x = text_layout.end_x;
      cursor.y = text_layout.end_y;
    }
    total_characters += character_count;
  };

  for (pugi::xml_node child = node.first_child(); child;) {
    if (context.resource_budget_exhausted) break;

    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata ||
        (child.type() == pugi::node_element &&
         std::string(child.name()) == "tspan")) {
      CompatibleTspanText run;
      run.whitespace = whitespace;
      bool included_content = false;
      pugi::xml_node next = child;
      while (next) {
        if (next.type() == pugi::node_pcdata || next.type() == pugi::node_cdata) {
          run.text += normalize_text_whitespace(
              next.value(), preserve, run.whitespace);
          included_content = true;
          next = next.next_sibling();
          continue;
        }
        if (next.type() != pugi::node_element) {
          next = next.next_sibling();
          continue;
        }
        if (std::string(next.name()) != "tspan") break;

        const std::size_t original_text_size = run.text.size();
        const TextWhitespaceState original_whitespace = run.whitespace;
        const std::size_t original_element_count = run.element_count;
        const std::size_t original_maximum_depth = run.maximum_relative_depth;
        if (!collect_compatible_tspan_text(
              next, rules, style, preserve, 1, run) ||
            run.element_count >
              kMaxExpandedNodeCount - context.expanded_nodes ||
            expanded_depth + run.maximum_relative_depth >
              kMaxExpandedTraversalDepth) {
          run.text.resize(original_text_size);
          run.whitespace = original_whitespace;
          run.element_count = original_element_count;
          run.maximum_relative_depth = original_maximum_depth;
          break;
        }
        included_content = true;
        next = next.next_sibling();
      }

      if (included_content) {
        context.expanded_nodes += run.element_count;
        whitespace = run.whitespace;
        emit_text_run(run.text);
        child = next;
        continue;
      }
    }

    const pugi::xml_node next = child.next_sibling();
    if (child.type() == pugi::node_element) {
      const std::string child_name = child.name();
      if ((child_name == "tspan" || child_name == "textPath") &&
          begin_expanded_node(child, expanded_depth + 1, context)) {
        const std::size_t child_characters = collect_text_node(
          child,
          svg_root,
          rules,
          style,
          transform,
          fallback_font_path,
          out_paths,
          cursor,
          whitespace,
          position_stack,
          preserve,
          parse_double_string(effective_style.opacity, 1.0),
          expanded_depth + 1,
          context);
        total_characters += child_characters;
      }
    }
    child = next;
  }

  position_stack.pop_back();
  return total_characters;
}

void collect_paths(const pugi::xml_node& node,
                   const pugi::xml_node& svg_root,
                   const std::vector<CssRule>& rules,
                   const StyleState& inherited,
                   const std::string& parent_transform,
                   const std::optional<std::string>& font_path,
                   std::vector<PathEntry>& out_paths,
                   double ancestor_opacity,
                   std::size_t expanded_depth,
                   TraversalContext& context) {
  if (node.type() != pugi::node_element) return;
  if (!begin_expanded_node(node, expanded_depth, context)) return;

  const std::string name = node.name();
  if (should_skip_tag(name)) return;

  const StyleState style = resolve_style(node, rules, inherited);
  StyleState effective_style = style;
  effective_style.opacity = format_opacity(
    ancestor_opacity * parse_double_string(style.opacity, 1.0));
  normalize_numeric_style(effective_style);
  const ComputedStyle computed = compute_style(effective_style);
  if (!computed.displayed) return;
  const std::string transform =
    combine_transform(parent_transform, node.attribute("transform").as_string());

  if (name == "use") {
    std::string href = node.attribute("href").as_string();
    if (href.empty()) href = node.attribute("xlink:href").as_string();
    if (!href.empty() && href[0] == '#') {
      const std::string reference_id = href.substr(1);
      if (context.reference_depth >= kMaxUseReferenceDepth) {
        record_traversal_warning(
            context,
            node,
            "use-depth-limit",
            "Use expansion reached the 64-reference safety limit and was truncated.");
        return;
      }
      if (context.active_references.find(reference_id) != context.active_references.end()) {
        record_traversal_warning(
            context,
            node,
            "cyclic-use-reference",
            "A cyclic use reference was suppressed to keep conversion bounded.");
        return;
      }
      const auto target = find_by_id(*context.id_index, reference_id);
      if (target) {
        std::string use_transform = transform;
        const double x = attr_double(node, "x", 0.0);
        const double y = attr_double(node, "y", 0.0);
        if (x != 0.0 || y != 0.0) {
          use_transform = combine_transform(
            use_transform, "translate(" + fmt(x) + " " + fmt(y) + ")");
        }

        context.active_references.insert(reference_id);
        ++context.reference_depth;
        if (std::string(target->name()) == "symbol") {
          if (!begin_expanded_node(*target, expanded_depth + 1, context)) {
            --context.reference_depth;
            context.active_references.erase(reference_id);
            return;
          }
          StyleState symbol_style = resolve_style(*target, rules, style);
          // A symbol's generated instance has an author-agent display value of
          // inline even when display on the definition itself says otherwise.
          symbol_style.display = "inline";
          if (target->attribute("x") || target->attribute("y")) {
            record_traversal_warning(
              context,
              node,
              "unsupported-symbol-position",
              "The referenced symbol sets x or y, whose instance-position semantics are outside the supported symbol contract. Compatible conversion ignores the symbol position and applies only x/y from this use; strict conversion rejects this use instance.");
          }
          const double symbol_opacity = std::clamp(
            parse_double_string(effective_style.opacity, 1.0) *
              parse_double_string(symbol_style.opacity, 1.0),
            0.0,
            1.0);
          const std::optional<SymbolViewportMapping> viewport =
            symbol_viewbox_mapping(node, *target, svg_root);
          if (viewport.has_value()) {
            if (viewport->uses_root_viewport_fallback &&
                context.symbol_instance_depth > 0) {
              record_traversal_warning(
                context,
                node,
                "unsupported-nested-symbol-viewport",
                "A nested symbol use relies on an automatic width or height from its containing symbol viewport. Compatible conversion resolves that automatic dimension against the root SVG viewport because nested viewport state is outside the supported contract; strict conversion rejects this use instance. Set explicit width and height on the use or referenced symbol for deterministic conversion.");
            }
            const std::size_t first_symbol_path = out_paths.size();
            std::string symbol_transform = combine_transform(
              use_transform, target->attribute("transform").as_string());
            symbol_transform = combine_transform(symbol_transform, viewport->transform);
            ++context.symbol_instance_depth;
            for (const pugi::xml_node child : target->children()) {
              collect_paths(
                child,
                svg_root,
                rules,
                symbol_style,
                symbol_transform,
                font_path,
                out_paths,
                symbol_opacity,
                expanded_depth + 2,
                context);
              if (context.resource_budget_exhausted) break;
            }
            --context.symbol_instance_depth;

            const bool overflow_is_visible =
              lower_copy(trim(target->attribute("overflow").as_string())) == "visible";
            if (!overflow_is_visible) {
              const SymbolClipProof clip_proof = prove_symbol_paths_inside_viewport(
                out_paths,
                first_symbol_path,
                combine_transform(
                  use_transform, target->attribute("transform").as_string()),
                *viewport);
              if (clip_proof != SymbolClipProof::Contained) {
                record_traversal_warning(
                  context,
                  node,
                  "unsupported-symbol-viewport-clipping",
                  clip_proof == SymbolClipProof::Outside
                    ? "Referenced symbol geometry extends outside its use viewport. Compatible conversion expands the symbol without the required viewport clip, so overflow remains visible; strict conversion rejects this use instance. Set overflow=\"visible\" only when unclipped overflow is intended."
                    : "Referenced symbol geometry could not be proven to remain inside its use viewport. Compatible conversion expands the symbol without a viewport clip, so overflow may remain visible; strict conversion rejects this use instance. Set overflow=\"visible\" only when unclipped overflow is intended.");
              }
            }
          }
        } else {
          collect_paths(*target,
                        svg_root,
                        rules,
                        style,
                        use_transform,
                        font_path,
                        out_paths,
                        parse_double_string(effective_style.opacity, 1.0),
                        expanded_depth + 1,
                        context);
        }
        --context.reference_depth;
        context.active_references.erase(reference_id);
      }
    }
    return;
  }

  if (name == "text" || name == "tspan") {
    TextCursor cursor;
    TextWhitespaceState whitespace;
    std::vector<TextPositionState*> position_stack;
    collect_text_node(
      node,
      svg_root,
      rules,
      inherited,
      parent_transform,
      font_path,
      out_paths,
      cursor,
      whitespace,
      position_stack,
      false,
      ancestor_opacity,
      expanded_depth,
      context);
    return;
  }

  const bool emit_fill_for_node = computed.visible && computed.has_fill;
  const bool emit_stroke_for_node = computed.visible && computed.has_stroke;
  const std::size_t first_source_entry = out_paths.size();
  if ((name == "circle" || name == "ellipse") &&
      emit_stroke_for_node && computed.stroke_width > 0.0 && !computed.has_dash_pattern) {
    const std::string fill_path =
        name == "circle" ? circle_to_path(node) : ellipse_to_path(node);
    if (emit_fill_for_node && !fill_path.empty() &&
        reserve_output_paths(node, 1, context)) {
      append_path_entry(
        out_paths,
        fill_path,
        transform,
        effective_style.fill,
        effective_style.stroke,
        effective_style,
        true,
        false);
    }

    const StyleState outline_style = stroke_as_fill_style(effective_style);
    const std::string stroke_path =
        name == "circle" ? circle_stroke_to_ring(node, computed.stroke_width)
                         : ellipse_stroke_to_ring(node, computed.stroke_width);
    if (!stroke_path.empty() && reserve_output_paths(node, 1, context)) {
      append_path_entry(
        out_paths,
        stroke_path,
        transform,
        effective_style.stroke,
        effective_style.stroke,
        outline_style,
        true,
        false);
    }
  } else {
    const std::string d = node_to_path(node);
    const bool valid_path_data = name != "path" || path_data_is_valid(d);
    if (!d.empty() && valid_path_data) {
      const bool has_curve_segments = path_has_curve_segments(d);
      const bool keep_live_dashed_stroke = emit_stroke_for_node && computed.has_dash_pattern;
      bool emit_fill = emit_fill_for_node;
      const bool emit_stroke = emit_stroke_for_node;
      if (name == "line") emit_fill = false;

      const bool should_outline_stroke = emit_stroke && !keep_live_dashed_stroke;
      const std::string final_stroke_outline =
        should_outline_stroke
          ? (has_curve_segments
              ? build_curve_fallback_outline(
                  d,
                  computed.stroke_width,
                  to_string(computed.stroke_linecap),
                  to_string(computed.stroke_linejoin),
                  computed.stroke_miterlimit)
              : build_straight_stroke_outline(
                  d,
                  computed.stroke_width,
                  to_string(computed.stroke_linecap),
                  to_string(computed.stroke_linejoin),
                  computed.stroke_miterlimit))
          : "";

      const bool emit_live_stroke =
          final_stroke_outline.empty() && emit_stroke && computed.stroke_width > 0.0 &&
          stroke_path_can_paint(d, to_string(computed.stroke_linecap));
      const std::size_t emitted_live_paths =
          (emit_fill ? 1U : 0U) + (emit_live_stroke ? 1U : 0U);
      if (emitted_live_paths != 0 &&
          reserve_output_paths(node, emitted_live_paths, context)) {
        if (emit_live_stroke) {
          record_live_stroke_retention(
              context,
              node,
              keep_live_dashed_stroke
                  ? "Filled-path conversion cannot yet outline dashed strokes."
                  : "Filled-path conversion could not produce a valid outline for this stroke.");
        }
        append_path_entry(
          out_paths,
          d,
          transform,
          effective_style.fill,
          effective_style.stroke,
          effective_style,
          emit_fill,
          emit_live_stroke);
      }
      if (!final_stroke_outline.empty() &&
          reserve_output_paths(node, 1, context)) {
        const StyleState outline_style = stroke_as_fill_style(effective_style);
        append_path_entry(
          out_paths,
          final_stroke_outline,
          transform,
          effective_style.stroke,
          effective_style.stroke,
          outline_style,
          true,
          false);
      }
    }
  }

  preserve_source_element_opacity(
    out_paths, first_source_entry, computed.opacity, context);

  for (const pugi::xml_node child : node.children()) {
    if (context.resource_budget_exhausted) break;
    collect_paths(child,
                  svg_root,
                  rules,
                  style,
                  transform,
                  font_path,
                  out_paths,
                  parse_double_string(effective_style.opacity, 1.0),
                  expanded_depth + 1,
                  context);
  }
}

}  // namespace

void collect_paths_from_svg(const pugi::xml_node& svg_node,
                            const SvgIdIndex& id_index,
                            const std::vector<CssRule>& rules,
                            const StyleState& root_style,
                            const std::optional<std::string>& font_path,
                            std::vector<PathEntry>& out_paths,
                            bool font_path_is_authoritative,
                            std::vector<std::string>* fonts_used,
                            std::vector<Diagnostic>* diagnostics,
                            std::size_t* missing_glyphs,
                            ConversionPolicy conversion_policy) {
  if (!compute_style(root_style).displayed) return;

  TraversalContext context;
  context.id_index = &id_index;
  context.font_path_is_authoritative = font_path_is_authoritative;
  context.conversion_policy = conversion_policy;
  context.fonts_used = fonts_used;
  context.diagnostics = diagnostics;
  context.missing_glyphs = missing_glyphs;
  const std::string root_transform = svg_node.attribute("transform").as_string();
  const double root_opacity = std::clamp(
    parse_double_string(root_style.opacity, 1.0), 0.0, 1.0);
  for (const pugi::xml_node child : svg_node.children()) {
    collect_paths(
      child, svg_node, rules, root_style, root_transform, font_path, out_paths, root_opacity, 2,
      context);
    if (context.resource_budget_exhausted) break;
  }
}

}  // namespace svg_squisher
