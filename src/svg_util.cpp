#include "svg_util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace svg_squisher {
namespace {

thread_local int active_output_precision = 4;

bool number_may_start_with(char ch) {
  return std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
         ch == '+' || ch == '-' || ch == '.';
}

bool is_css_whitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f';
}

bool is_hex_digit(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

unsigned int hex_digit_value(char ch) {
  if (ch >= '0' && ch <= '9') return static_cast<unsigned int>(ch - '0');
  if (ch >= 'a' && ch <= 'f') return static_cast<unsigned int>(ch - 'a' + 10);
  return static_cast<unsigned int>(ch - 'A' + 10);
}

void append_utf8(std::string& output, char32_t codepoint) {
  if (codepoint <= 0x7f) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

std::string decode_css_escapes(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] != '\\') {
      decoded.push_back(value[index++]);
      continue;
    }

    ++index;
    if (index == value.size()) break;
    if (value[index] == '\n' || value[index] == '\f') {
      ++index;
      continue;
    }
    if (value[index] == '\r') {
      ++index;
      if (index < value.size() && value[index] == '\n') ++index;
      continue;
    }
    if (!is_hex_digit(value[index])) {
      decoded.push_back(value[index++]);
      continue;
    }

    char32_t codepoint = 0;
    std::size_t digits = 0;
    while (index < value.size() && digits < 6 && is_hex_digit(value[index])) {
      codepoint = codepoint * 16 + hex_digit_value(value[index]);
      ++index;
      ++digits;
    }
    if (index < value.size() && is_css_whitespace(value[index])) {
      if (value[index] == '\r' && index + 1 < value.size() &&
          value[index + 1] == '\n') {
        ++index;
      }
      ++index;
    }
    if (codepoint == 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      codepoint = 0xfffd;
    }
    append_utf8(decoded, codepoint);
  }
  return decoded;
}

std::string without_css_comments(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    if (index + 1 < value.size() && value[index] == '/' &&
        value[index + 1] == '*') {
      const std::size_t close = value.find("*/", index + 2);
      if (close == std::string::npos) break;
      index = close + 2;
      continue;
    }
    result.push_back(value[index++]);
  }
  return result;
}

bool is_css_identifier_character(unsigned char ch) {
  return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch >= 0x80;
}

void skip_css_whitespace(const std::string& value, std::size_t& position) {
  while (position < value.size() && is_css_whitespace(value[position])) ++position;
}

std::optional<std::vector<double>> parse_comma_wsp_list(
    const std::string& text,
    bool allow_px_lengths,
    bool allow_trailing_decimal_point = true) {
  std::vector<double> values;
  std::size_t position = 0;
  skip_svg_whitespace(text, position);
  if (position == text.size()) return values;
  if (text[position] == ',') return std::nullopt;

  while (true) {
    if (!number_may_start_with(text[position])) return std::nullopt;
    const std::size_t token_start = position;
    double value = 0.0;
    if (!parse_number_token(text, position, value)) return std::nullopt;
    if (!allow_trailing_decimal_point) {
      std::size_t significand_end = position;
      for (std::size_t index = token_start; index < position; ++index) {
        if (text[index] == 'e' || text[index] == 'E') {
          significand_end = index;
          break;
        }
      }
      if (significand_end > token_start && text[significand_end - 1] == '.') {
        return std::nullopt;
      }
    }
    if (allow_px_lengths && position + 2 <= text.size() &&
        lower_copy(text.substr(position, 2)) == "px") {
      position += 2;
    }
    values.push_back(value);

    const std::size_t separator_start = position;
    skip_svg_whitespace(text, position);
    const bool had_whitespace = position != separator_start;
    if (position == text.size()) return values;

    if (text[position] == ',') {
      ++position;
      skip_svg_whitespace(text, position);
      if (position == text.size() || text[position] == ',') return std::nullopt;
    } else if (!had_whitespace) {
      return std::nullopt;
    }
  }
}

bool consume_comma_wsp(const std::string& text, std::size_t& position) {
  const std::size_t separator_start = position;
  skip_svg_whitespace(text, position);
  const bool had_whitespace = position != separator_start;
  if (position < text.size() && text[position] == ',') {
    ++position;
    skip_svg_whitespace(text, position);
    return true;
  }
  return had_whitespace;
}

bool parse_number_at_current(const std::string& text,
                             std::size_t& position,
                             double& value) {
  if (position >= text.size() || !number_may_start_with(text[position])) {
    return false;
  }
  return parse_number_token(text, position, value);
}

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

CssUrlAnalysis analyze_css_urls(const std::string& value) {
  CssUrlAnalysis analysis;
  const std::string decoded = decode_css_escapes(without_css_comments(value));
  const std::string lowered = lower_copy(decoded);
  std::size_t search_from = 0;
  while (search_from < lowered.size()) {
    const std::size_t name = lowered.find("url", search_from);
    if (name == std::string::npos) break;
    if (name > 0 && is_css_identifier_character(
                      static_cast<unsigned char>(lowered[name - 1]))) {
      search_from = name + 3;
      continue;
    }

    std::size_t cursor = name + 3;
    skip_css_whitespace(lowered, cursor);
    if (cursor >= lowered.size() || lowered[cursor] != '(') {
      search_from = name + 3;
      continue;
    }

    analysis.has_url = true;
    ++cursor;
    skip_css_whitespace(decoded, cursor);
    std::string target;
    bool well_formed = true;
    std::size_t next_search = cursor;
    if (cursor < decoded.size() &&
        (decoded[cursor] == '\'' || decoded[cursor] == '"')) {
      const char quote = decoded[cursor++];
      const std::size_t target_start = cursor;
      const std::size_t quote_end = decoded.find(quote, cursor);
      if (quote_end == std::string::npos) {
        well_formed = false;
        next_search = decoded.size();
      } else {
        target = decoded.substr(target_start, quote_end - target_start);
        cursor = quote_end + 1;
        skip_css_whitespace(decoded, cursor);
        if (cursor >= decoded.size() || decoded[cursor] != ')') {
          well_formed = false;
          next_search = cursor;
        } else {
          next_search = cursor + 1;
        }
      }
    } else {
      const std::size_t close = decoded.find(')', cursor);
      if (close == std::string::npos) {
        well_formed = false;
        next_search = decoded.size();
      } else {
        target = trim(decoded.substr(cursor, close - cursor));
        next_search = close + 1;
        if (std::any_of(target.begin(), target.end(), [](unsigned char ch) {
              return is_css_whitespace(static_cast<char>(ch)) || ch == '\'' ||
                     ch == '"' || ch == '(';
            })) {
          well_formed = false;
        }
      }
    }

    if (well_formed && target.size() > 1 && target.front() == '#') {
      analysis.local_fragment_ids.push_back(target.substr(1));
    } else {
      analysis.has_unsafe_url = true;
    }
    if (next_search <= name) next_search = name + 3;
    search_from = next_search;
  }
  return analysis;
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
  return parse_comma_wsp_list(text, false).value_or(std::vector<double>{});
}

std::vector<double> parse_length_list(const std::string& text) {
  return parse_comma_wsp_list(text, true).value_or(std::vector<double>{});
}

std::optional<std::vector<double>> parse_points_list(const std::string& text) {
  std::vector<double> values;
  std::size_t position = 0;
  skip_svg_whitespace(text, position);
  if (position == text.size()) return values;

  while (true) {
    double x = 0.0;
    double y = 0.0;
    if (!parse_number_at_current(text, position, x)) return std::nullopt;

    // The points grammar permits a minus sign to separate the two coordinates
    // in one pair without comma-wsp (for example, "10-20").
    if (position < text.size() && text[position] == '-') {
      if (!parse_number_at_current(text, position, y)) return std::nullopt;
    } else {
      if (!consume_comma_wsp(text, position) ||
          !parse_number_at_current(text, position, y)) {
        return std::nullopt;
      }
    }
    values.push_back(x);
    values.push_back(y);

    std::size_t trailing = position;
    skip_svg_whitespace(text, trailing);
    if (trailing == text.size()) return values;
    if (!consume_comma_wsp(text, position) || position == text.size()) {
      return std::nullopt;
    }
  }
}

std::optional<std::array<double, 4>> parse_viewbox(const std::string& text) {
  const auto values = parse_comma_wsp_list(text, false, false);
  if (!values || values->size() != 4) return std::nullopt;
  return std::array<double, 4>{(*values)[0], (*values)[1], (*values)[2], (*values)[3]};
}

bool is_svg_whitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

void skip_svg_whitespace(const std::string& data, std::size_t& position) {
  while (position < data.size() && is_svg_whitespace(data[position])) ++position;
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
