#include "svg_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "svg_computed_style.h"
#include "svg_dom.h"
#include "svg_path.h"
#include "svg_shape.h"
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
  const std::string key = code + "\n" + element + "\n" + message;
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

bool css_property_uses_length(const std::string& property) {
  return property == "stroke-width" || property == "font-size" ||
         property == "letter-spacing";
}

std::string compatible_css_numeric_action(const std::string& property) {
  if (property == "opacity" || property == "fill-opacity" ||
      property == "stroke-opacity") {
    return "Compatible conversion substitutes the default value 1.";
  }
  if (property == "stroke-width") {
    return "Compatible conversion substitutes the default stroke width of 1.";
  }
  if (property == "stroke-miterlimit") {
    return "Compatible conversion substitutes the default miter limit of 4.";
  }
  if (property == "font-size") {
    return "Compatible conversion substitutes the default font size of 16.";
  }
  if (property == "letter-spacing") {
    return "Compatible conversion substitutes the default letter spacing of 0.";
  }
  return "Compatible conversion ignores this declaration.";
}

bool has_external_css_url(const std::string& value) {
  return analyze_css_urls(value).has_unsafe_url;
}

void inspect_css_declarations(const std::string& declarations,
                              const pugi::xml_node& node,
                              std::vector<Diagnostic>& diagnostics,
                              std::set<std::string>& seen) {
  if (has_external_css_url(declarations)) {
    add_warning(diagnostics, seen, "unsupported-external-reference",
                "External CSS url(...) resources are not fetched; compatible conversion replaces external fill/stroke paints with none and ignores other external resource declarations.",
                node);
  }
  for (const std::string& declaration : split(declarations, ';')) {
    const std::size_t colon = declaration.find(':');
    if (colon == std::string::npos) continue;
    const std::string property = lower_copy(trim(declaration.substr(0, colon)));
    const std::string value = trim(declaration.substr(colon + 1));
    if (!property.empty() && !css_property_is_supported(property)) {
      add_warning(diagnostics, seen, "unsupported-css-property",
                  "Compatible conversion ignores the unsupported CSS property '" +
                    property + "'.",
                  node);
      break;
    }
    if (!property.empty() && css_property_uses_length(property) &&
        has_non_px_unit(without_important(value))) {
      add_warning(diagnostics,
                  seen,
                  "unsupported-length-unit",
                  "The CSS property '" + property +
                    "' uses an unsupported relative or physical unit. " +
                    compatible_css_numeric_action(property),
                  node);
      continue;
    }
    if (!property.empty() && !css_numeric_value_is_valid(property, value)) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  "The CSS property '" + property +
                    "' has an invalid, non-finite, or out-of-range numeric value. " +
                    compatible_css_numeric_action(property),
                  node);
    }
  }
}

std::string compatible_length_action(const pugi::xml_node& node,
                                     const std::string& attribute) {
  const std::string name = node.name();
  if ((name == "text" || name == "tspan") &&
      (attribute == "x" || attribute == "y")) {
    return "Compatible conversion ignores this position list and keeps the current text cursor " +
      attribute + " coordinate.";
  }
  if ((name == "text" || name == "tspan") &&
      (attribute == "dx" || attribute == "dy")) {
    return "Compatible conversion ignores this position list, so this attribute applies no text displacement.";
  }
  if (attribute == "textLength") {
    return "Compatible conversion ignores textLength while outlining text.";
  }
  if (attribute == "stroke-width") {
    return "Compatible conversion substitutes the default stroke width of 1.";
  }
  if (attribute == "font-size") {
    return "Compatible conversion substitutes the default font size of 16.";
  }
  if (attribute == "letter-spacing") {
    return "Compatible conversion substitutes the default letter spacing of 0.";
  }
  if (attribute == "x" || attribute == "y" || attribute == "x1" ||
      attribute == "y1" || attribute == "x2" || attribute == "y2" ||
      attribute == "cx" || attribute == "cy") {
    return "Compatible conversion substitutes 0 for this coordinate.";
  }
  if ((attribute == "width" || attribute == "height") && name == "rect") {
    return "Compatible conversion substitutes 0, so this rect geometry is dropped.";
  }
  if (attribute == "r" && name == "circle") {
    return "Compatible conversion substitutes 0, so this circle geometry is dropped.";
  }
  if ((attribute == "rx" || attribute == "ry") && name == "ellipse") {
    return "Compatible conversion substitutes 0, so this ellipse geometry is dropped.";
  }
  if ((attribute == "rx" || attribute == "ry") && name == "rect") {
    return "Compatible conversion treats this radius as 0 before applying the rectangle's paired-radius rules.";
  }
  if ((attribute == "width" || attribute == "height") && name == "use") {
    return "Compatible conversion ignores this value; a referenced symbol instance resolves the automatic dimension from the symbol, then the supported root viewport.";
  }
  if ((attribute == "width" || attribute == "height") && name == "symbol") {
    return "Compatible conversion ignores this value; the automatic symbol dimension resolves from the supported root viewport.";
  }
  if ((attribute == "width" || attribute == "height") && name == "svg") {
    return "Compatible output preserves this root attribute verbatim, but conversion does not use it as a finite viewport length.";
  }
  return "Compatible conversion substitutes 0 for this length.";
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
    if (has_non_px_unit(raw_value)) return;
    double value = 0.0;
    if (!parse_finite_length(raw_value, value) ||
        (non_negative && value < 0.0)) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  std::string(attribute) +
                    " has an invalid, non-finite, or out-of-range numeric value. " +
                    compatible_length_action(node, attribute),
                  node);
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
                    " has an invalid, non-finite, or out-of-range numeric value. " +
                    (std::string(attribute) == "stroke-miterlimit"
                         ? "Compatible conversion substitutes the default miter limit of 4."
                         : "Compatible conversion substitutes its default value."),
                  node);
    }
  };

  const bool text_position = name == "text" || name == "tspan";
  for (const char* attribute : {"x", "y"}) {
    if (!node.attribute(attribute)) continue;
    if (text_position) {
      if (!has_non_px_unit(node.attribute(attribute).as_string()) &&
          parse_length_list(node.attribute(attribute).as_string()).empty()) {
        add_warning(diagnostics, seen, "invalid-numeric-value",
                    std::string(attribute) +
                      " does not contain a valid finite length list. " +
                      compatible_length_action(node, attribute),
                    node);
      }
    } else {
      check_length(attribute, false);
    }
  }
  if (text_position) {
    for (const char* attribute : {"dx", "dy"}) {
      if (node.attribute(attribute) &&
          !has_non_px_unit(node.attribute(attribute).as_string()) &&
          parse_length_list(node.attribute(attribute).as_string()).empty()) {
        add_warning(diagnostics, seen, "invalid-numeric-value",
                    std::string(attribute) +
                      " does not contain a valid finite length list. " +
                      compatible_length_action(node, attribute),
                    node);
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
                  std::string(attribute) +
                    " does not contain a finite number. Compatible conversion substitutes the default value 1.",
                  node);
    }
  }
  check_number("stroke-miterlimit", 1.0);

  if (node.attribute("transform") &&
      !transform_is_valid(node.attribute("transform").as_string())) {
    add_warning(diagnostics, seen, "invalid-numeric-value",
                "transform is malformed or contains a non-finite value. Compatible conversion ignores the entire local transform.",
                node);
  }
  if (node.attribute("viewBox")) {
    const auto values = parse_viewbox(node.attribute("viewBox").as_string());
    if (!values || (*values)[2] < 0.0 || (*values)[3] < 0.0) {
      const bool is_root_svg = name == "svg" &&
        node.parent().type() == pugi::node_document;
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  "viewBox must contain four finite SVG numbers with non-negative width and height; a zero dimension disables rendering. " +
                    std::string(is_root_svg
                      ? "Compatible output preserves the invalid root viewBox verbatim and leaves viewport handling to the renderer."
                      : "Compatible conversion ignores this viewBox and processes supported descendants in the current user coordinate system."),
                  node);
    }
  }
  if ((name == "polyline" || name == "polygon") && node.attribute("points")) {
    const auto values = parse_points_list(node.attribute("points").as_string());
    if (!values) {
      add_warning(diagnostics, seen, "invalid-numeric-value",
                  "points must contain a finite sequence of coordinate pairs. Compatible conversion drops this polyline or polygon geometry.",
                  node);
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
};

struct ExpansionInspection {
  ExpansionProblem problem = ExpansionProblem::None;
  pugi::xml_node problem_node;
  std::size_t expanded_nodes = 1;
};

void inspect_expanded_tree(const pugi::xml_node& node,
                           const SvgIdIndex& id_index,
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
      if (const auto target = find_by_id(id_index, id)) {
        active.insert(id);
        inspect_expanded_tree(*target,
                              id_index,
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
                          id_index,
                          active,
                          reference_depth,
                          combined_depth + 1,
                          false,
                          inspection);
    if (inspection.problem != ExpansionProblem::None) return;
  }
}

bool expansion_may_paint_multiple_layers(const pugi::xml_node& node,
                                         const SvgIdIndex& id_index,
                                         const std::vector<CssRule>& rules,
                                         const StyleState& inherited,
                                         std::set<std::string>& active,
                                         std::size_t reference_depth,
                                         std::size_t combined_depth,
                                         bool referenced_root,
                                         std::size_t& visited_nodes,
                                         std::size_t& painted_layers) {
  if (node.type() != pugi::node_element) return false;
  if (combined_depth > kMaxExpandedTraversalDepth ||
      reference_depth > kMaxUseReferenceDepth ||
      visited_nodes >= kMaxExpandedNodeCount) {
    return true;
  }
  ++visited_nodes;

  const std::string name = node.name();
  if (should_skip_tag(name) && !(referenced_root && name == "symbol")) return false;
  StyleState style = resolve_style(node, rules, inherited);
  if (referenced_root && name == "symbol") style.display = "inline";
  const ComputedStyle computed = compute_style(style);
  if (!computed.displayed) return false;

  if (name == "path" || name == "rect" || name == "circle" || name == "ellipse" ||
      name == "line" || name == "polyline" || name == "polygon") {
    const std::string path_data = node_to_path(node);
    const bool valid_geometry = !path_data.empty() &&
      (name != "path" || path_data_is_valid(path_data));
    const bool may_paint = computed.visible && valid_geometry &&
      (name == "line" ? computed.has_stroke : computed.has_fill || computed.has_stroke);
    if (may_paint && ++painted_layers >= 2) return true;
  }

  if (name == "text" || name == "tspan" || name == "textPath") {
    const bool text_may_paint = computed.visible && computed.font_size > 0.0 &&
      (computed.has_fill || computed.has_stroke);
    for (const pugi::xml_node child : node.children()) {
      if (text_may_paint &&
          (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) &&
          !trim(child.value()).empty() && ++painted_layers >= 2) {
        return true;
      }
      if (child.type() != pugi::node_element) continue;
      const std::string child_name = child.name();
      if (child_name != "tspan" && child_name != "textPath") continue;
      if (expansion_may_paint_multiple_layers(child,
                                              id_index,
                                              rules,
                                              style,
                                              active,
                                              reference_depth,
                                              combined_depth + 1,
                                              false,
                                              visited_nodes,
                                              painted_layers)) {
        return true;
      }
    }
    return false;
  }

  if (name == "use") {
    std::string href = node.attribute("href").as_string();
    if (href.empty()) href = node.attribute("xlink:href").as_string();
    if (href.size() <= 1 || href.front() != '#') return false;
    const std::string id = href.substr(1);
    if (active.find(id) != active.end() || reference_depth >= kMaxUseReferenceDepth) {
      return true;
    }
    const auto target = find_by_id(id_index, id);
    if (!target) return false;
    active.insert(id);
    const bool multiple = expansion_may_paint_multiple_layers(
        *target,
        id_index,
        rules,
        style,
        active,
        reference_depth + 1,
        combined_depth + 1,
        true,
        visited_nodes,
        painted_layers);
    active.erase(id);
    return multiple;
  }

  for (const pugi::xml_node child : node.children()) {
    if (expansion_may_paint_multiple_layers(child,
                                            id_index,
                                            rules,
                                            style,
                                            active,
                                            reference_depth,
                                            combined_depth + 1,
                                            false,
                                            visited_nodes,
                                            painted_layers)) {
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
        const std::string unsupported_selector = trim(selector);
        add_warning(diagnostics, seen, "unsupported-css-selector",
                    "The CSS rule for selector \"" + unsupported_selector +
                        "\" is ignored during compatible conversion because combinators, "
                        "attribute selectors, and pseudo-classes are unsupported.",
                    node);
        break;
      }
    }
    const std::string declarations = css.substr(open + 1, close - open - 1);
    inspect_css_declarations(declarations, node, diagnostics, seen);
    cursor = close + 1;
  }
}

bool has_complex_text_anchor_positioning(const pugi::xml_node& node) {
  const auto coordinate_count = [](const pugi::xml_node& element,
                                   const char* attribute_name) {
    const pugi::xml_attribute attribute = element.attribute(attribute_name);
    return attribute ? parse_length_list(attribute.as_string()).size()
                     : std::size_t{0};
  };

  if (std::max(coordinate_count(node, "x"),
               coordinate_count(node, "y")) > 1) {
    return true;
  }

  std::vector<pugi::xml_node> pending;
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_element) pending.push_back(child);
  }
  while (!pending.empty()) {
    const pugi::xml_node descendant = pending.back();
    pending.pop_back();
    const std::string name = descendant.name();
    if (name != "tspan" && name != "textPath") continue;
    if (descendant.attribute("x") || descendant.attribute("y")) {
      return true;
    }
    for (const pugi::xml_node child : descendant.children()) {
      if (child.type() == pugi::node_element) pending.push_back(child);
    }
  }
  return false;
}

void inspect_node(const pugi::xml_node& node,
                  const pugi::xml_node& root,
                  const SvgIdIndex& id_index,
                  const std::vector<CssRule>& rules,
                  std::vector<Diagnostic>& diagnostics,
                  std::set<std::string>& seen,
                  std::size_t source_depth,
                  const StyleState& inherited) {
  if (node.type() != pugi::node_element) return;
  const std::string name = node.name();
  const StyleState resolved_style = resolve_style(node, rules, inherited);

  inspect_numeric_attributes(node, diagnostics, seen);

  if ((name == "text" || name == "tspan") &&
      compute_style(resolved_style).text_anchor != TextAnchorMode::Start &&
      has_complex_text_anchor_positioning(node)) {
    add_warning(
      diagnostics,
      seen,
      "unsupported-text-anchor-chunk-positioning",
      "Middle/end text anchoring with a multi-value x/y list or descendant absolute x/y creates multiple text chunks whose cross-element membership is not fully represented. Compatible conversion preserves logical x/y/dx/dy list consumption and applies node-local anchor shifts, so text following a positioned descendant may be offset differently; strict conversion rejects this text.",
      node);
  }

  if (!element_is_known(name)) {
    add_warning(diagnostics, seen, "unsupported-element",
                "This SVG element is not converted or preserved.", node);
  }

  if (name == "style") inspect_css(node, diagnostics, seen);
  if (node.attribute("style")) {
    inspect_css_declarations(node.attribute("style").as_string(), node, diagnostics, seen);
  }
  for (const pugi::xml_attribute attribute : node.attributes()) {
    const std::string attribute_name = attribute.name();
    if (attribute_name == "style" || attribute_name == "href" ||
        attribute_name == "xlink:href") {
      continue;
    }
    if (has_external_css_url(attribute.as_string())) {
      add_warning(diagnostics, seen, "unsupported-external-reference",
                  (attribute_name == "fill" || attribute_name == "stroke")
                      ? "External paint URLs are not fetched; compatible conversion replaces this paint with none."
                      : "External URL resources are not fetched; compatible conversion ignores this resource-valued attribute.",
                  node);
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
                "Text-on-path positioning is not implemented; compatible conversion uses "
                "normal-cursor text layout instead.",
                node);
  } else if (name == "switch") {
    add_warning(diagnostics, seen, "unsupported-switch",
                "Conditional switch selection is not evaluated; compatible conversion "
                "traverses all branches in document order.",
                node);
  } else if (name == "marker") {
    add_warning(diagnostics, seen, "unsupported-marker",
                "Markers are not expanded into output geometry.", node);
  } else if (name == "animate" || name == "animateMotion" || name == "animateTransform" ||
             name == "set") {
    add_warning(diagnostics, seen, "unsupported-animation",
                "Animated SVG state is not represented in static path output.", node);
  } else if (name == "svg" && node != root) {
    add_warning(diagnostics, seen, "unsupported-nested-viewport",
                "Nested SVG viewport and preserveAspectRatio semantics are not resolved; "
                "compatible conversion ignores x, y, width, height, viewBox, and "
                "preserveAspectRatio on this nested viewport, then traverses its children "
                "in the current user coordinate system with any transform still applied.",
                node);
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

  std::string href = node.attribute("href").as_string();
  if (href.empty()) href = node.attribute("xlink:href").as_string();
  if (!href.empty() && href.front() != '#') {
    add_warning(diagnostics, seen, "unsupported-external-reference",
                name == "use"
                    ? "External use references are not fetched; compatible conversion emits no geometry for this use instance."
                    : "External references are not fetched; compatible conversion removes the external resource link from retained output.",
                node);
  }

  const double local_opacity = parse_double_string(resolved_style.opacity, 1.0);
  if (node_has_local_property(node, rules, "opacity") &&
      local_opacity > 0.0 && local_opacity < 1.0) {
    std::set<std::string> active;
    std::size_t visited_nodes = 0;
    std::size_t painted_layers = 0;
    const bool symbol_root = name == "symbol";
    if (expansion_may_paint_multiple_layers(node,
                                            id_index,
                                            rules,
                                            inherited,
                                            active,
                                            0,
                                            source_depth,
                                            symbol_root,
                                            visited_nodes,
                                            painted_layers)) {
      const bool use_instance = name == "use";
      add_warning(
          diagnostics,
          seen,
          use_instance ? "use-opacity-flattened" : "group-opacity-flattened",
          use_instance
              ? "Opacity on this use instance is distributed across multiple referenced layers; overlapping content may composite differently."
              : "Opacity on this element is distributed across multiple painted layers; overlapping content may composite differently.",
          node);
    }
  }

  const std::string inline_style = lower_copy(node.attribute("style").as_string());
  for (const char* attr : {"fill", "stroke", "color"}) {
    const std::string value = lower_copy(node.attribute(attr).as_string());
    if (value == "currentcolor" || inline_style.find("currentcolor") != std::string::npos ||
        lower_copy(resolved_style.fill) == "currentcolor" ||
        lower_copy(resolved_style.stroke) == "currentcolor") {
      add_warning(diagnostics, seen, "unsupported-current-color",
                  "Compatible output preserves the literal currentColor paint but does not preserve the source color cascade; the output renderer resolves it.",
                  node);
      break;
    }
  }

  for (const char* attr : {"direction", "unicode-bidi", "writing-mode", "text-orientation",
                           "dominant-baseline", "alignment-baseline", "baseline-shift", "word-spacing",
                           "textLength", "lengthAdjust", "rotate", "font-variant",
                           "font-feature-settings"}) {
    if (node.attribute(attr) || inline_style.find(lower_copy(attr) + ":") != std::string::npos) {
      add_warning(diagnostics, seen, "unsupported-text-layout",
                  "Compatible conversion ignores " + std::string(attr) +
                    " while outlining text.",
                  node);
      break;
    }
  }

  for (const char* attr : {"x", "y", "dx", "dy", "x1", "y1", "x2", "y2", "cx", "cy",
                           "r", "rx", "ry", "width", "height", "stroke-width", "font-size",
                           "letter-spacing", "textLength"}) {
    const pugi::xml_attribute attribute = node.attribute(attr);
    const bool automatic_symbol_dimension =
      (name == "use" || name == "symbol") &&
      (std::string(attr) == "width" || std::string(attr) == "height") &&
      lower_copy(trim(attribute.as_string())) == "auto";
    if (attribute && !automatic_symbol_dimension &&
        has_non_px_unit(attribute.as_string())) {
      add_warning(diagnostics, seen, "unsupported-length-unit",
                  std::string(attr) +
                    " uses an unsupported relative or physical unit. " +
                    compatible_length_action(node, attr),
                  node);
    }
  }

  for (pugi::xml_node child : node.children()) {
    inspect_node(child,
                 root,
                 id_index,
                 rules,
                 diagnostics,
                 seen,
                 source_depth + 1,
                 resolved_style);
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

std::vector<Diagnostic> inspect_svg_capabilities(const pugi::xml_node& svg_root,
                                                 const SvgIdIndex& id_index) {
  std::vector<Diagnostic> diagnostics;
  std::set<std::string> seen;
  const std::vector<CssRule> rules = parse_css_rules(svg_root);
  inspect_node(svg_root, svg_root, id_index, rules, diagnostics, seen, 1, StyleState{});

  ExpansionInspection expansion;
  std::set<std::string> active;
  for (const pugi::xml_node child : svg_root.children()) {
    inspect_expanded_tree(child, id_index, active, 0, 2, false, expansion);
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
