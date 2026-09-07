#include "svg_paint.h"

#include "svg_util.h"

namespace svg_squisher {

ParsedPaint parse_paint(const std::string& paint, double opacity) {
  ParsedPaint parsed;
  parsed.raw = paint;
  const std::string trimmed = trim(paint);
  parsed.normalized = lower_copy(trimmed);

  if (parsed.normalized.empty() || parsed.normalized == "none" || opacity <= 1e-6) {
    parsed.kind = PaintKind::None;
    parsed.visible = false;
    return parsed;
  }

  if (parsed.normalized.size() >= 6 && parsed.normalized.rfind("url(", 0) == 0) {
    const std::size_t close = trimmed.find(')', 4);
    if (close != std::string::npos) {
      std::string target = trim(trimmed.substr(4, close - 4));
      if (target.size() >= 2 &&
          ((target.front() == '\'' && target.back() == '\'') ||
           (target.front() == '"' && target.back() == '"'))) {
        target = trim(target.substr(1, target.size() - 2));
      }
      if (target.size() > 1 && target.front() == '#') {
        parsed.kind = PaintKind::Url;
        parsed.url_id = target.substr(1);
        parsed.visible = true;
        return parsed;
      }
    }
  }

  parsed.kind = PaintKind::Value;
  parsed.visible = true;
  return parsed;
}

bool paint_equals(const std::string& lhs, const std::string& rhs) {
  return lower_copy(trim(lhs)) == lower_copy(trim(rhs));
}

std::optional<double> paint_brightness(const std::string& paint) {
  const std::string value = lower_copy(trim(paint));
  if (value == "white") return 255.0;
  if (value == "black") return 0.0;

  std::string hex;
  if (value.size() == 4 && value[0] == '#') {
    hex = {value[1], value[1], value[2], value[2], value[3], value[3]};
  } else if (value.size() == 7 && value[0] == '#') {
    hex = value.substr(1);
  } else {
    return std::nullopt;
  }

  const int r = std::stoi(hex.substr(0, 2), nullptr, 16);
  const int g = std::stoi(hex.substr(2, 2), nullptr, 16);
  const int b = std::stoi(hex.substr(4, 2), nullptr, 16);
  return 0.299 * r + 0.587 * g + 0.114 * b;
}

}  // namespace svg_squisher
