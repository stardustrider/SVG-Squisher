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
#include <vector>

#include "svg_computed_style.h"
#include "svg_diagnostics.h"
#include "svg_dom.h"
#include "svg_path.h"
#include "svg_shape.h"
#include "svg_stroke.h"
#include "svg_text.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace svg_squisher {
namespace {

struct TraversalContext {
  std::unordered_set<std::string> active_references;
  std::size_t reference_depth = 0;
  std::size_t expanded_nodes = 1;
  std::size_t emitted_output_paths = 0;
  bool resource_budget_exhausted = false;
  bool font_path_is_authoritative = false;
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

void collect_normalized_text(const pugi::xml_node& node,
                             bool inherited_preserve,
                             TextWhitespaceState& whitespace,
                             std::string& text) {
  const bool preserve = preserve_whitespace_for_node(node, inherited_preserve);
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
      text += normalize_text_whitespace(child.value(), preserve, whitespace);
    } else if (child.type() == pugi::node_element) {
      const std::string child_name = child.name();
      if (child_name == "tspan" || child_name == "textPath") {
        collect_normalized_text(child, preserve, whitespace, text);
      }
    }
  }
}

StyleState stroke_as_fill_style(const StyleState& style) {
  StyleState outline_style = style;
  outline_style.fill_opacity = style.stroke_opacity;
  return outline_style;
}

std::optional<std::string> symbol_viewbox_transform(const pugi::xml_node& use_node,
                                                    const pugi::xml_node& symbol_node) {
  const std::vector<double> viewbox =
    parse_number_list(symbol_node.attribute("viewBox").as_string());
  if (viewbox.size() < 4) return std::string{};

  const double min_x = viewbox[0];
  const double min_y = viewbox[1];
  const double viewbox_width = viewbox[2];
  const double viewbox_height = viewbox[3];
  if (viewbox_width <= 0.0 || viewbox_height <= 0.0) return std::nullopt;

  double viewport_width = attr_double(use_node, "width", viewbox_width);
  double viewport_height = attr_double(use_node, "height", viewbox_height);
  if (viewport_width <= 0.0 || viewport_height <= 0.0) return std::nullopt;

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

  return "translate(" + fmt(offset_x) + " " + fmt(offset_y) + ") scale(" +
    fmt(scale_x) + " " + fmt(scale_y) + ") translate(" + fmt(-min_x) + " " +
    fmt(-min_y) + ")";
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

  const std::optional<std::string> text_font_path = resolve_text_font_path(
    style, fallback_font_path, context.font_path_is_authoritative);

  const bool establishes_anchor_chunk =
    std::string(node.name()) == "text" || !position_state.x.empty();
  if (establishes_anchor_chunk && text_font_path.has_value() &&
      (computed.text_anchor == TextAnchorMode::Middle || computed.text_anchor == TextAnchorMode::End)) {
    TextWhitespaceState measure_whitespace;
    std::string chunk_text;
    collect_normalized_text(node, preserve, measure_whitespace, chunk_text);
    const double advance = measure_text_advance(
      chunk_text, computed.font_size, *text_font_path, computed.letter_spacing);
    cursor.x = (position_state.x.empty() ? cursor.x : position_state.x.front()) -
      (computed.text_anchor == TextAnchorMode::Middle ? advance / 2.0 : advance);
    if (!position_state.x.empty()) position_state.first_x_override = cursor.x;
  }

  std::size_t total_characters = 0;

  for (const pugi::xml_node child : node.children()) {
    if (context.resource_budget_exhausted) break;
    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
      const std::string text = normalize_text_whitespace(child.value(), preserve, whitespace);
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
      continue;
    }

    if (child.type() != pugi::node_element) continue;
    const std::string child_name = child.name();
    if (child_name != "tspan" && child_name != "textPath") continue;
    if (!begin_expanded_node(child, expanded_depth + 1, context)) continue;

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
      const auto target = find_by_id(svg_root, reference_id);
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
          const StyleState symbol_style = resolve_style(*target, rules, style);
          const ComputedStyle symbol_computed = compute_style(symbol_style);
          const double symbol_opacity = std::clamp(
            parse_double_string(effective_style.opacity, 1.0) *
              parse_double_string(symbol_style.opacity, 1.0),
            0.0,
            1.0);
          const std::optional<std::string> viewport_transform =
            symbol_viewbox_transform(node, *target);
          if (symbol_computed.displayed && viewport_transform.has_value()) {
            std::string symbol_transform = combine_transform(
              use_transform, target->attribute("transform").as_string());
            symbol_transform = combine_transform(symbol_transform, *viewport_transform);
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
    if (emit_fill_for_node && reserve_output_paths(node, 1, context)) {
      append_path_entry(
        out_paths,
        name == "circle" ? circle_to_path(node) : ellipse_to_path(node),
        transform,
        effective_style.fill,
        effective_style.stroke,
        effective_style,
        true,
        false);
    }

    const StyleState outline_style = stroke_as_fill_style(effective_style);
    if (reserve_output_paths(node, 1, context)) {
      append_path_entry(
        out_paths,
        name == "circle" ? circle_stroke_to_ring(node, computed.stroke_width)
                         : ellipse_stroke_to_ring(node, computed.stroke_width),
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

      const bool emit_live_stroke = final_stroke_outline.empty() && emit_stroke;
      const std::size_t emitted_live_paths =
          (emit_fill ? 1U : 0U) + (emit_live_stroke ? 1U : 0U);
      if (emitted_live_paths != 0 &&
          reserve_output_paths(node, emitted_live_paths, context)) {
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
                            const std::vector<CssRule>& rules,
                            const StyleState& root_style,
                            const std::optional<std::string>& font_path,
                            std::vector<PathEntry>& out_paths,
                            bool font_path_is_authoritative,
                            std::vector<std::string>* fonts_used,
                            std::vector<Diagnostic>* diagnostics,
                            std::size_t* missing_glyphs) {
  if (!compute_style(root_style).displayed) return;

  TraversalContext context;
  context.font_path_is_authoritative = font_path_is_authoritative;
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
