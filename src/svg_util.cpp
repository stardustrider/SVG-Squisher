#include "svg_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace svg_squisher {
namespace {

thread_local int active_output_precision = 4;

}  // namespace

OutputPrecisionScope::OutputPrecisionScope(int precision)
    : previous_precision_(active_output_precision) {
  active_output_precision = std::max(0, std::min(15, precision));
}

OutputPrecisionScope::~OutputPrecisionScope() {
  active_output_precision = previous_precision_;
}

int output_precision() {
  return active_output_precision;
}

std::string trim(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::vector<std::string> split(const std::string& text, char delim) {
  std::vector<std::string> parts;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, delim)) {
    parts.push_back(item);
  }
  return parts;
}

std::vector<double> parse_number_list(const std::string& text) {
  std::vector<double> values;
  std::size_t position = 0;
  double value = 0.0;
  while (true) {
    skip_separators(text, position);
    if (position == text.size()) return values;
    if (!parse_number_token(text, position, value)) return {};
    values.push_back(value);
  }
}

std::vector<double> parse_length_list(const std::string& text) {
  std::vector<double> values;
  std::size_t position = 0;
  while (true) {
    skip_separators(text, position);
    if (position == text.size()) return values;

    double value = 0.0;
    if (!parse_number_token(text, position, value)) return {};
    if (position + 2 <= text.size() &&
        lower_copy(text.substr(position, 2)) == "px") {
      position += 2;
    }
    if (position < text.size()) {
      const unsigned char next = static_cast<unsigned char>(text[position]);
      if (!std::isspace(next) && text[position] != ',' &&
          text[position] != '+' && text[position] != '-') {
        return {};
      }
    }
    values.push_back(value);
  }
}

void skip_separators(const std::string& data, std::size_t& position) {
  while (position < data.size()) {
    const char ch = data[position];
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',') {
      ++position;
    } else {
      break;
    }
  }
}

bool parse_number_token(const std::string& data,
                        std::size_t& position,
                        double& value) {
  std::size_t cursor = position;
  skip_separators(data, cursor);
  if (cursor >= data.size()) return false;

  const std::size_t start = cursor;
  if (data[cursor] == '+' || data[cursor] == '-') ++cursor;
  if (cursor + 1 < data.size() && data[cursor] == '0' &&
      (data[cursor + 1] == 'x' || data[cursor + 1] == 'X')) {
    return false;
  }

  bool has_digit = false;
  while (cursor < data.size() && std::isdigit(static_cast<unsigned char>(data[cursor]))) {
    has_digit = true;
    ++cursor;
  }
  if (cursor < data.size() && data[cursor] == '.') {
    ++cursor;
    while (cursor < data.size() && std::isdigit(static_cast<unsigned char>(data[cursor]))) {
      has_digit = true;
      ++cursor;
    }
  }
  if (!has_digit) return false;

  if (cursor < data.size() && (data[cursor] == 'e' || data[cursor] == 'E')) {
    ++cursor;
    if (cursor < data.size() && (data[cursor] == '+' || data[cursor] == '-')) ++cursor;
    const std::size_t exponent_start = cursor;
    while (cursor < data.size() && std::isdigit(static_cast<unsigned char>(data[cursor]))) {
      ++cursor;
    }
    if (cursor == exponent_start) return false;
  }

  const std::string token = data.substr(start, cursor - start);
  const char* token_start = token.c_str();
  char* end = nullptr;
  value = std::strtod(token_start, &end);
  if (end != token_start + token.size() || !std::isfinite(value)) return false;
  position = cursor;
  return true;
}

bool parse_arc_flag(const std::string& data,
                    std::size_t& position,
                    int& value) {
  skip_separators(data, position);
  if (position >= data.size() || (data[position] != '0' && data[position] != '1')) {
    return false;
  }
  value = data[position] - '0';
  ++position;
  return true;
}

std::string fmt(double value) {
  if (!std::isfinite(value)) {
    throw std::runtime_error("Conversion produced a non-finite coordinate");
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(active_output_precision) << value;
  std::string s = out.str();
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s.empty() || s == "-0") s = "0";
  return s;
}

bool parse_finite_number(const std::string& text, double& value) {
  const std::string trimmed = trim(text);
  if (trimmed.empty()) return false;
  char* end = nullptr;
  const double parsed = std::strtod(trimmed.c_str(), &end);
  if (end == trimmed.c_str() || end != trimmed.c_str() + trimmed.size() ||
      !std::isfinite(parsed)) {
    return false;
  }
  value = parsed;
  return true;
}

bool parse_finite_length(const std::string& text, double& value) {
  std::string normalized = trim(text);
  if (normalized.size() >= 2 &&
      lower_copy(normalized.substr(normalized.size() - 2)) == "px") {
    normalized = trim(normalized.substr(0, normalized.size() - 2));
  }
  return parse_finite_number(normalized, value);
}

double parse_double_string(const std::string& text, double fallback) {
  double value = 0.0;
  return parse_finite_number(text, value) ? value : fallback;
}

bool valid_svg_arc_parameters(double radius_x,
                              double radius_y,
                              double large_arc_flag,
                              double sweep_flag) {
  return std::isfinite(radius_x) && std::isfinite(radius_y) && radius_x >= 0.0 &&
         radius_y >= 0.0 && (large_arc_flag == 0.0 || large_arc_flag == 1.0) &&
         (sweep_flag == 0.0 || sweep_flag == 1.0);
}

double combined_opacity(const std::string& a, const std::string& b) {
  return parse_double_string(a, 1.0) * parse_double_string(b, 1.0);
}

bool paint_is_visible(const std::string& paint, double opacity) {
  return lower_copy(trim(paint)) != "none" && opacity > 1e-6;
}

}  // namespace svg_squisher
