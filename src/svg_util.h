#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace svg_squisher {

class OutputPrecisionScope {
public:
  explicit OutputPrecisionScope(int precision);
  ~OutputPrecisionScope();

  OutputPrecisionScope(const OutputPrecisionScope&) = delete;
  OutputPrecisionScope& operator=(const OutputPrecisionScope&) = delete;

private:
  int previous_precision_ = 4;
};

struct CssUrlAnalysis {
  bool has_url = false;
  bool has_unsafe_url = false;
  std::vector<std::string> local_fragment_ids;
};

std::string trim(std::string value);
std::string lower_copy(std::string value);
CssUrlAnalysis analyze_css_urls(const std::string& value);
std::vector<std::string> split(const std::string& text, char delim);
std::vector<double> parse_number_list(const std::string& text);
std::vector<double> parse_length_list(const std::string& text);
std::optional<std::vector<double>> parse_points_list(const std::string& text);
std::optional<std::array<double, 4>> parse_viewbox(const std::string& text);
bool is_svg_whitespace(char ch);
void skip_svg_whitespace(const std::string& data, std::size_t& position);
void skip_separators(const std::string& data, std::size_t& position);
bool parse_number_token(const std::string& data,
                        std::size_t& position,
                        double& value);
bool parse_arc_flag(const std::string& data,
                    std::size_t& position,
                    int& value);
std::string fmt(double value);
int output_precision();
bool parse_finite_number(const std::string& text, double& value);
bool parse_finite_length(const std::string& text, double& value);
double parse_double_string(const std::string& text, double fallback = 0.0);
bool valid_svg_arc_parameters(double radius_x,
                              double radius_y,
                              double large_arc_flag,
                              double sweep_flag);
double combined_opacity(const std::string& a, const std::string& b);
bool paint_is_visible(const std::string& paint, double opacity);

}  // namespace svg_squisher
