#include "svg_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <pugixml.hpp>

#include "svg_dom.h"
#include "svg_path.h"
#include "svg_style.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace svg_squisher {
namespace {

std::string element_label(const pugi::xml_node& node) {
  std::string label = "<" + std::string(node.name());
  if (node.attribute("id")) label += "#" + std::string(node.attribute("id").as_string());
  label += ">";
  return label;
}

void add_warning(std::vector<Diagnostic>& diagnostics,
                 std::set<std::string>& seen,
                 const std::string& code,
                 const std::string& message,
                 const pugi::xml_node& node) {
  const std::string element = element_label(node);
  const std::string key = code + "\n" + element;
  if (!seen.insert(key).second) return;
  diagnostics.push_back({DiagnosticSeverity::Warning, code, message, element});
}

bool has_non_px_unit(const std::string& value) {
  const std::string lowered = lower_copy(trim(value));
  if (lowered.empty() || lowered == "none" || lowered == "normal") return false;
  if (lowered.find('%') != std::string::npos) return true;
  for (std::size_t i = 0; i < lowered.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(lowered[i]);
    if (!std::isalpha(ch)) continue;
    if ((ch == 'e') && i > 0 && i + 1 < lowered.size() &&
        (std::isdigit(static_cast<unsigned char>(lowered[i - 1])) || lowered[i - 1] == '.') &&
        (std::isdigit(static_cast<unsigned char>(lowered[i + 1])) || lowered[i + 1] == '+' || lowered[i + 1] == '-')) {
      continue;
    }
    return lowered.compare(i, 2, "px") != 0;
  }
  return false;
}

bool selector_is_simple(const std::string& selector) {
  const std::string value = trim(selector);
  if (value.empty()) return false;
  return value.find_first_of(" >+~[:") == std::string::npos;
}

bool css_property_is_supported(const std::string& property) {
  static const std::set<std::string> supported = {
    "fill", "fill-opacity", "stroke", "stroke-opacity", "stroke-width",
    "stroke-dasharray", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
    "fill-rule", "opacity", "display", "visibility", "font-size", "font-family",
    "font-weight", "font-style", "text-anchor", "letter-spacing",
  };
  return supported.find(lower_copy(trim(property))) != supported.end();
}

std::string without_important(std::string value) {
  value = trim(value);
  const std::string lowered = lower_copy(value);
  const std::size_t bang = lowered.rfind('!');
  if (bang != std::string::npos && trim(lowered.substr(bang + 1)) == "important") {
    value = trim(value.substr(0, bang));
  }
  return value;
}

bool is_css_wide_keyword(const std::string& value) {
  const std::string normalized = lower_copy(trim(value));
  return normalized == "inherit" || normalized == "initial" || normalized == "unset";
}

bool css_numeric_value_is_valid(const std::string& property,
                                const std::string& raw_value) {
  const std::string value = without_important(raw_value);
  if (is_css_wide_keyword(value)) return true;
  double parsed = 0.0;
  if (property == "opacity" || property == "fill-opacity" ||
      property == "stroke-opacity") {
    return parse_finite_number(value, parsed);
  }
  if (property == "stroke-width" || property == "font-size") {
    return parse_finite_length(value, parsed) && parsed >= 0.0;
  }
  if (property == "stroke-miterlimit") {
    return parse_finite_number(value, parsed) && parsed >= 1.0;
  }
  if (property == "letter-spacing") {
    return lower_copy(value) == "normal" || parse_finite_length(value, parsed);
  }
  return true;
}

std::string css_url_target(const std::string& value,
                           std::size_t open,
                           std::size_t close) {
  std::string target = trim(value.substr(open + 4, close - open - 4));
  if (target.size() >= 2 &&
      ((target.front() == '\'' && target.back() == '\'') ||
       (target.front() == '"' && target.back() == '"'))) {
    target = trim(target.substr(1, target.size() - 2));
  }
  return target;
}

bool has_external_css_url(const std::string& value) {
  const std::string lowered = lower_copy(value);
  std::size_t cursor = 0;
  while ((cursor = lowered.find("url(", cursor)) != std::string::npos) {
    const std::size_t close = lowered.find(')', cursor + 4);
    if (close == std::string::npos) return false;
    const std::string target = css_url_target(value, cursor, close);
    if (!target.empty() && target.front() != '#') return true;
    cursor = close + 1;
  }
  return false;
}

void inspect_css_declarations(const std::string& declarations,
                              const pugi::xml_node& node,
                              std::vector<Diagnostic>& diagnostics,
                              std::set<std::string>& seen) {
  if (has_external_css_url(declarations)) {
    add_warning(diagnostics, seen, "unsupported-external-reference",
                "External references are not fetched during conversion.", node);
  }
  for (const std::string& declaration : split(declarations, ';')) {
    const std::size_t colon = declaration.find(':');
    if (colon == std::string::npos) continue;
    const std::string property = lower_copy(trim(declaration.substr(0, colon)));
    const std::string value = trim(declaration.substr(colon + 1));
    if (!property.empty() && !css_property_is_supported(property)) {
      add_warning(diagnostics, seen, "unsupported-css-property",
                  "The CSS property '" + property + "' is not represented in path output.", node);
      break;
    }
    if (!property.empty() && !css_numeric_value_is_valid(property, value)) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  "The CSS property '" + property +
                    "' has an invalid, non-finite, or out-of-range numeric value.", node);
    }
  }
}

void inspect_numeric_attributes(const pugi::xml_node& node,
                                std::vector<Diagnostic>& diagnostics,
                                std::set<std::string>& seen) {
  const std::string name = node.name();
  const auto check_length = [&](const char* attribute, bool non_negative) {
    if (!node.attribute(attribute)) return;
    const std::string raw_value = node.attribute(attribute).as_string();
    const std::string property = attribute;
    if ((property == "stroke-width" || property == "font-size" ||
         property == "letter-spacing") && is_css_wide_keyword(raw_value)) {
      return;
    }
    if (property == "letter-spacing" && lower_copy(trim(raw_value)) == "normal") return;
    double value = 0.0;
    if (!parse_finite_length(raw_value, value) ||
        (non_negative && value < 0.0)) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  std::string(attribute) +
                    " has an invalid, non-finite, or out-of-range numeric value.", node);
    }
  };
  const auto check_number = [&](const char* attribute, double minimum) {
    if (!node.attribute(attribute)) return;
    const std::string raw_value = node.attribute(attribute).as_string();
    if (is_css_wide_keyword(raw_value)) return;
    double value = 0.0;
    if (!parse_finite_number(raw_value, value) || value < minimum) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  std::string(attribute) +
                    " has an invalid, non-finite, or out-of-range numeric value.", node);
    }
  };

  const bool text_position = name == "text" || name == "tspan";
  for (const char* attribute : {"x", "y"}) {
    if (!node.attribute(attribute)) continue;
    if (text_position) {
      if (parse_length_list(node.attribute(attribute).as_string()).empty()) {
        add_warning(diagnostics, seen, "invalid-numeric-value",
                    std::string(attribute) + " does not contain a valid finite length list.", node);
      }
    } else {
      check_length(attribute, false);
    }
  }
  if (text_position) {
    for (const char* attribute : {"dx", "dy"}) {
      if (node.attribute(attribute) &&
          parse_length_list(node.attribute(attribute).as_string()).empty()) {
        add_warning(diagnostics, seen, "invalid-numeric-value",
                    std::string(attribute) + " does not contain a valid finite length list.", node);
      }
    }
  }

  for (const char* attribute : {"x1", "y1", "x2", "y2", "cx", "cy"}) {
    check_length(attribute, false);
  }
  for (const char* attribute : {"width", "height", "r", "rx", "ry",
                                "stroke-width", "font-size"}) {
    check_length(attribute, true);
  }
  check_length("letter-spacing", false);
  for (const char* attribute : {"opacity", "fill-opacity", "stroke-opacity"}) {
    if (!node.attribute(attribute)) continue;
    if (is_css_wide_keyword(node.attribute(attribute).as_string())) continue;
    double value = 0.0;
    if (!parse_finite_number(node.attribute(attribute).as_string(), value)) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  std::string(attribute) + " does not contain a finite number.", node);
    }
  }
  check_number("stroke-miterlimit", 1.0);

  if (node.attribute("transform") &&
      !transform_is_valid(node.attribute("transform").as_string())) {
    add_warning(diagnostics, seen, "invalid-numeric-value",
                "transform is malformed or contains a non-finite value.", node);
  }
  if (node.attribute("viewBox")) {
    const std::vector<double> values = parse_number_list(node.attribute("viewBox").as_string());
    if (values.size() != 4 || values[2] <= 0.0 || values[3] <= 0.0) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  "viewBox must contain four finite numbers with positive width and height.", node);
    }
  }
  if ((name == "polyline" || name == "polygon") && node.attribute("points")) {
    const std::vector<double> values = parse_number_list(node.attribute("points").as_string());
    if (values.size() < 2 || values.size() % 2 != 0) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  "points must contain a finite sequence of coordinate pairs.", node);
    }
  }
}

bool element_is_known(const std::string& name) {
  static const std::set<std::string> known = {
    "svg", "g", "defs", "style", "script", "title", "desc", "metadata",
    "path", "rect", "circle", "ellipse", "line", "polyline", "polygon",
    "use", "symbol", "text", "tspan", "textPath", "image", "foreignObject",
    "switch", "clipPath", "mask", "filter", "linearGradient", "radialGradient",
    "pattern", "stop", "marker", "animate", "animateMotion", "animateTransform",
    "set", "view", "a",
  };
  if (known.find(name) != known.end()) return true;
  return name.size() > 2 && name[0] == 'f' && name[1] == 'e' &&
         std::isupper(static_cast<unsigned char>(name[2]));
}

enum class ExpansionProblem {
  None,
  Cycle,
  ReferenceDepth,
  CombinedDepth,
  NodeBudget,
  OutputBudget,
};

struct ExpansionInspection {
  ExpansionProblem problem = ExpansionProblem::None;
  pugi::xml_node problem_node;
  std::size_t expanded_nodes = 1;
  std::size_t estimated_output_paths = 0;
};

std::size_t maximum_output_paths_for_node(const pugi::xml_node& node) {
  const std::string name = node.name();
  if (name == "path" || name == "rect" || name == "circle" || name == "ellipse" ||
      name == "line" || name == "polyline" || name == "polygon") {
    return 2;
  }
  if (name != "text" && name != "tspan" && name != "textPath") return 0;

  std::size_t text_chunks = 0;
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) ++text_chunks;
  }
  return text_chunks * 2;
}

void inspect_expanded_tree(const pugi::xml_node& node,
                           const pugi::xml_node& root,
                           std::set<std::string>& active,
                           std::size_t reference_depth,
                           std::size_t combined_depth,
                           bool referenced_root,
                           ExpansionInspection& inspection) {
  if (inspection.problem != ExpansionProblem::None ||
      node.type() != pugi::node_element) {
    return;
  }
  if (combined_depth > kMaxExpandedTraversalDepth) {
    inspection.problem = ExpansionProblem::CombinedDepth;
    inspection.problem_node = node;
    return;
  }
  if (inspection.expanded_nodes >= kMaxExpandedNodeCount) {
    inspection.problem = ExpansionProblem::NodeBudget;
    inspection.problem_node = node;
    return;
  }
  ++inspection.expanded_nodes;

  const std::size_t added_paths = maximum_output_paths_for_node(node);
  if (added_paths > kMaxOutputPathCount - inspection.estimated_output_paths) {
    inspection.problem = ExpansionProblem::OutputBudget;
    inspection.problem_node = node;
    return;
  }
  inspection.estimated_output_paths += added_paths;

  const std::string name = node.name();
  if (should_skip_tag(name) && !(referenced_root && name == "symbol")) return;

  if (name == "use") {
    std::string href = node.attribute("href").as_string();
    if (href.empty()) href = node.attribute("xlink:href").as_string();
    if (href.size() > 1 && href.front() == '#') {
      const std::string id = href.substr(1);
      if (active.find(id) != active.end()) {
        inspection.problem = ExpansionProblem::Cycle;
        inspection.problem_node = node;
        return;
      }
      if (reference_depth >= kMaxUseReferenceDepth) {
        inspection.problem = ExpansionProblem::ReferenceDepth;
        inspection.problem_node = node;
        return;
      }
      if (const auto target = find_by_id(root, id)) {
        active.insert(id);
        inspect_expanded_tree(*target,
                              root,
                              active,
                              reference_depth + 1,
                              combined_depth + 1,
                              true,
                              inspection);
        active.erase(id);
      }
    }
    return;
  }

  for (const pugi::xml_node child : node.children()) {
    inspect_expanded_tree(child,
                          root,
                          active,
                          reference_depth,
                          combined_depth + 1,
                          false,
                          inspection);
    if (inspection.problem != ExpansionProblem::None) return;
  }
}

bool expansion_may_paint_multiple_elements(const pugi::xml_node& node,
                                           const pugi::xml_node& root,
                                           std::set<std::string>& active,
                                           std::size_t reference_depth,
                                           std::size_t combined_depth,
                                           bool referenced_root,
                                           std::size_t& visited_nodes,
                                           std::size_t& painted_elements) {
  if (node.type() != pugi::node_element) return false;
  if (combined_depth > kMaxExpandedTraversalDepth ||
      reference_depth > kMaxUseReferenceDepth ||
      visited_nodes >= kMaxExpandedNodeCount) {
    return true;
  }
  ++visited_nodes;

  const std::string name = node.name();
  if (should_skip_tag(name) && !(referenced_root && name == "symbol")) return false;
  if (name == "path" || name == "rect" || name == "circle" || name == "ellipse" ||
      name == "line" || name == "polyline" || name == "polygon") {
    if (++painted_elements >= 2) return true;
  }
  if (name == "text" || name == "tspan" || name == "textPath") {
    for (const pugi::xml_node child : node.children()) {
      if ((child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) &&
          !trim(child.value()).empty() && ++painted_elements >= 2) {
        return true;
      }
    }
  }

  if (name == "use") {
    std::string href = node.attribute("href").as_string();
    if (href.empty()) href = node.attribute("xlink:href").as_string();
    if (href.size() <= 1 || href.front() != '#') return false;
    const std::string id = href.substr(1);
    if (active.find(id) != active.end() || reference_depth >= kMaxUseReferenceDepth) {
      return true;
    }
    const auto target = find_by_id(root, id);
    if (!target) return false;
    active.insert(id);
    const bool multiple = expansion_may_paint_multiple_elements(
        *target, root, active, reference_depth + 1, combined_depth + 1, true,
        visited_nodes, painted_elements);
    active.erase(id);
    return multiple;
  }

  for (const pugi::xml_node child : node.children()) {
    if (expansion_may_paint_multiple_elements(child,
                                              root,
                                              active,
                                              reference_depth,
                                              combined_depth + 1,
                                              false,
                                              visited_nodes,
                                              painted_elements)) {
      return true;
    }
  }
  return false;
}

void inspect_css(const pugi::xml_node& node,
                 std::vector<Diagnostic>& diagnostics,
                 std::set<std::string>& seen) {
  const std::string css = node.text().as_string();
  std::size_t cursor = 0;
  while (cursor < css.size()) {
    const std::size_t open = css.find('{', cursor);
    if (open == std::string::npos) break;
    const std::size_t close = css.find('}', open + 1);
    if (close == std::string::npos) break;
    const std::string selector_list = css.substr(cursor, open - cursor);
    for (const std::string& selector : split(selector_list, ',')) {
      if (!selector_is_simple(selector)) {
        add_warning(diagnostics, seen, "unsupported-css-selector",
                    "Combinators, attribute selectors, and pseudo-classes are not resolved; this rule may be ignored.", node);
        break;
      }
    }
    const std::string declarations = css.substr(open + 1, close - open - 1);
    inspect_css_declarations(declarations, node, diagnostics, seen);
    cursor = close + 1;
  }
}

void inspect_node(const pugi::xml_node& node,
                  const pugi::xml_node& root,
                  const std::vector<CssRule>& rules,
                  std::vector<Diagnostic>& diagnostics,
                  std::set<std::string>& seen,
                  std::size_t source_depth) {
  if (node.type() != pugi::node_element) return;
  const std::string name = node.name();

  inspect_numeric_attributes(node, diagnostics, seen);

  if (!element_is_known(name)) {
    add_warning(diagnostics, seen, "unsupported-element",
                "This SVG element is not converted or preserved.", node);
  }

  if (name == "style") inspect_css(node, diagnostics, seen);
  if (node.attribute("style")) {
    inspect_css_declarations(node.attribute("style").as_string(), node, diagnostics, seen);
  }
  for (const pugi::xml_attribute attribute : node.attributes()) {
    if (has_external_css_url(attribute.as_string())) {
      add_warning(diagnostics, seen, "unsupported-external-reference",
                  "External references are not fetched during conversion.", node);
      break;
    }
  }

  if (name == "path" && node.attribute("d") &&
      !path_data_is_valid(node.attribute("d").as_string())) {
    add_warning(diagnostics, seen, "invalid-path-data",
                "Path data is malformed and will be skipped.", node);
  }

  if (name == "script") {
    add_warning(diagnostics, seen, "unsupported-script",
                "Script and other active document behavior are not retained.", node);
  } else if (name == "image" || name == "foreignObject") {
    add_warning(diagnostics, seen, "unsupported-raster-content",
                "Embedded or external raster/HTML content is not converted to paths.", node);
  } else if (name == "textPath") {
    add_warning(diagnostics, seen, "unsupported-text-path",
                "Text-on-path positioning is not implemented; glyph placement may change.", node);
  } else if (name == "switch") {
    add_warning(diagnostics, seen, "unsupported-switch",
                "Conditional switch selection is not evaluated.", node);
  } else if (name == "marker") {
    add_warning(diagnostics, seen, "unsupported-marker",
                "Markers are not expanded into output geometry.", node);
  } else if (name == "animate" || name == "animateMotion" || name == "animateTransform" ||
             name == "set") {
    add_warning(diagnostics, seen, "unsupported-animation",
                "Animated SVG state is not represented in static path output.", node);
  } else if (name == "svg" && node != root) {
    add_warning(diagnostics, seen, "unsupported-nested-viewport",
                "Nested SVG viewport and preserveAspectRatio semantics are not fully resolved.", node);
  }

  for (const char* attr : {"clip-path", "mask", "filter"}) {
    if (node.attribute(attr) && lower_copy(node.attribute(attr).as_string()) != "none") {
      add_warning(diagnostics, seen, std::string("unsupported-") + attr,
                  std::string(attr) + " is not preserved by path conversion.", node);
    }
  }

  for (const char* attr : {"marker-start", "marker-mid", "marker-end", "vector-effect", "paint-order",
                           "stroke-dashoffset"}) {
    if (node.attribute(attr) && lower_copy(node.attribute(attr).as_string()) != "none") {
      add_warning(diagnostics, seen, "unsupported-paint-semantics",
                  std::string(attr) + " is not represented in the output path model.", node);
    }
  }

  const StyleState resolved_style = resolve_style(node, rules, StyleState{});
  if ((name == "g" || name == "svg" || name == "symbol") &&
      node_has_local_property(node, rules, "opacity") &&
      parse_double_string(resolved_style.opacity, 1.0) < 1.0) {
    add_warning(diagnostics, seen, "group-opacity-flattened",
                "Compatible conversion multiplies group opacity into descendants; overlapping children may composite differently.", node);
  }

  std::string href = node.attribute("href").as_string();
  if (href.empty()) href = node.attribute("xlink:href").as_string();
  if (!href.empty() && href.front() != '#') {
    add_warning(diagnostics, seen, "unsupported-external-reference",
                "External references are not fetched during conversion.", node);
  } else if (name == "use" && href.size() > 1 &&
             node_has_local_property(node, rules, "opacity") &&
             parse_double_string(resolve_style(node, rules, StyleState{}).opacity, 1.0) < 1.0) {
    const auto target = find_by_id(root, href.substr(1));
    if (target) {
      std::set<std::string> active{href.substr(1)};
      std::size_t visited_nodes = 0;
      std::size_t painted_elements = 0;
      if (expansion_may_paint_multiple_elements(*target,
                                                root,
                                                active,
                                                1,
                                                source_depth + 1,
                                                true,
                                                visited_nodes,
                                                painted_elements)) {
        add_warning(
            diagnostics,
            seen,
            "use-opacity-flattened",
            "Opacity on this use instance is distributed across multiple referenced elements; overlapping content may composite differently.",
            node);
      }
    }
  }

  const std::string inline_style = lower_copy(node.attribute("style").as_string());
  for (const char* attr : {"fill", "stroke", "color"}) {
    const std::string value = lower_copy(node.attribute(attr).as_string());
    if (value == "currentcolor" || inline_style.find("currentcolor") != std::string::npos ||
        lower_copy(resolved_style.fill) == "currentcolor" ||
        lower_copy(resolved_style.stroke) == "currentcolor") {
      add_warning(diagnostics, seen, "unsupported-current-color",
                  "currentColor is not fully resolved into an explicit paint.", node);
      break;
    }
  }

  for (const char* attr : {"direction", "unicode-bidi", "writing-mode", "text-orientation",
                           "dominant-baseline", "alignment-baseline", "baseline-shift", "word-spacing",
                           "textLength", "lengthAdjust", "rotate", "font-variant",
                           "font-feature-settings"}) {
    if (node.attribute(attr) || inline_style.find(lower_copy(attr) + ":") != std::string::npos) {
      add_warning(diagnostics, seen, "unsupported-text-layout",
                  std::string(attr) + " is not fully represented by text-to-path conversion.", node);
      break;
    }
  }

  for (const char* attr : {"x", "y", "x1", "y1", "x2", "y2", "cx", "cy", "r", "rx", "ry",
                           "width", "height", "stroke-width", "font-size", "letter-spacing"}) {
    if (node.attribute(attr) && has_non_px_unit(node.attribute(attr).as_string())) {
      add_warning(diagnostics, seen, "unsupported-length-unit",
                  std::string(attr) + " uses a relative or physical unit that is not fully resolved.", node);
    }
  }

  for (pugi::xml_node child : node.children()) {
    inspect_node(child, root, rules, diagnostics, seen, source_depth + 1);
  }
}

}  // namespace

void validate_svg_structure_depth(const pugi::xml_node& svg_root) {
  std::vector<std::pair<pugi::xml_node, std::size_t>> pending;
  pending.emplace_back(svg_root, 1);

  while (!pending.empty()) {
    const auto [node, depth] = pending.back();
    pending.pop_back();
    if (depth > kMaxSvgStructuralDepth) {
      throw std::runtime_error(
          "SVG element nesting depth exceeds safety limit of " +
          std::to_string(kMaxSvgStructuralDepth));
    }

    for (const pugi::xml_node child : node.children()) {
      if (child.type() == pugi::node_element) {
        pending.emplace_back(child, depth + 1);
      }
    }
  }
}

std::vector<Diagnostic> inspect_svg_capabilities(const pugi::xml_node& svg_root) {
  std::vector<Diagnostic> diagnostics;
  std::set<std::string> seen;
  const std::vector<CssRule> rules = parse_css_rules(svg_root);
  inspect_node(svg_root, svg_root, rules, diagnostics, seen, 1);

  ExpansionInspection expansion;
  std::set<std::string> active;
  for (const pugi::xml_node child : svg_root.children()) {
    inspect_expanded_tree(child, svg_root, active, 0, 2, false, expansion);
    if (expansion.problem != ExpansionProblem::None) break;
  }
  switch (expansion.problem) {
    case ExpansionProblem::None:
      break;
    case ExpansionProblem::Cycle:
      add_warning(diagnostics, seen, "cyclic-use-reference",
                  "A cyclic use reference was suppressed to keep conversion bounded.",
                  expansion.problem_node);
      break;
    case ExpansionProblem::ReferenceDepth:
      add_warning(diagnostics, seen, "use-depth-limit",
                  "Use expansion reached the 64-reference safety limit and was truncated.",
                  expansion.problem_node);
      break;
    case ExpansionProblem::CombinedDepth:
      add_warning(diagnostics, seen, "expanded-traversal-depth-limit",
                  "Expanded use traversal exceeds the combined 256-element safety depth; the reference branch was truncated.",
                  expansion.problem_node);
      break;
    case ExpansionProblem::NodeBudget:
      add_warning(diagnostics, seen, "expanded-node-limit",
                  "Expanded use traversal exceeds the 16384-element visit limit; remaining referenced content was skipped.",
                  expansion.problem_node);
      break;
    case ExpansionProblem::OutputBudget:
      add_warning(diagnostics, seen, "output-path-limit",
                  "Expanded content may exceed the 8192-output-path limit; remaining content was skipped.",
                  expansion.problem_node);
      break;
  }
  return diagnostics;
}

std::size_t count_svg_elements(const pugi::xml_node& svg_root) {
  std::size_t count = 0;
  for (const pugi::xpath_node& match : svg_root.select_nodes("descendant-or-self::*")) {
    (void)match;
    ++count;
  }
  return count;
}

const char* diagnostic_severity_name(DiagnosticSeverity severity) {
  return severity == DiagnosticSeverity::Error ? "error" : "warning";
}

}  // namespace svg_squisher
