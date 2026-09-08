#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "svg_style.h"

namespace svg_squisher {

struct TextCursor {
  double x = 0.0;
  double y = 0.0;
  bool has_x = false;
  bool has_y = false;
};

struct TextLayoutResult {
  std::string d;
  double end_x = 0.0;
  double end_y = 0.0;
  std::size_t glyph_count = 0;
  bool has_right_to_left_run = false;
  std::vector<char32_t> missing_codepoints;
  std::optional<FontIdentity> font_identity;
};

struct TextFontResolution {
  std::optional<std::string> path;
  bool requested_family_unresolved = false;
  bool used_fallback = false;
  bool used_authoritative_font = false;
};

std::string collect_direct_text(const pugi::xml_node& node);
std::vector<char32_t> decode_utf8(const std::string& text);
std::optional<std::string> inherited_attr(const pugi::xml_node& node, const char* name);
std::optional<std::string> discover_default_font();
double parse_svg_length(const std::string& value, double fallback = 0.0);
std::optional<std::string> resolve_text_font_path(const StyleState& style,
                                                  const std::optional<std::string>& fallback_font_path,
                                                  bool fallback_is_authoritative = false);
TextFontResolution resolve_text_font(const StyleState& style,
                                     const std::optional<std::string>& fallback_font_path,
                                     bool fallback_is_authoritative = false);
double first_coord_value(const pugi::xml_node& node, const char* attr_name, double fallback);
std::vector<double> coord_values(const pugi::xml_node& node, const char* attr_name);
TextLayoutResult text_to_path(const std::string& text,
                              double x,
                              double y,
                              double font_size,
                              const std::string& font_path,
                              double letter_spacing,
                              const std::vector<double>& x_values,
                              const std::vector<double>& y_values,
                              const std::vector<double>& dx_values,
                              const std::vector<double>& dy_values);
std::string text_to_path(const std::string& text,
                         double x,
                         double y,
                         double font_size,
                         const std::string& font_path);
std::string text_to_path(const std::string& text,
                         double x,
                         double y,
                         double font_size,
                         const std::string& font_path,
                         double letter_spacing);
double measure_text_advance(const std::string& text, double font_size, const std::string& font_path);
double measure_text_advance(const std::string& text,
                            double font_size,
                            const std::string& font_path,
                            double letter_spacing);

}  // namespace svg_squisher
