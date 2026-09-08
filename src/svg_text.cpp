#include "svg_text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <hb-ft.h>
#include <hb.h>

#include "svg_font_identity.h"
#include "svg_util.h"

namespace fs = std::filesystem;

namespace svg_squisher {
namespace {

struct FontLibrary {
  struct CachedFace {
    std::shared_ptr<const std::vector<unsigned char>> bytes;
    FT_Face face = nullptr;
  };

  FT_Library library = nullptr;
  std::map<std::string, CachedFace> faces;
  std::deque<std::string> face_order;

  FontLibrary() {
    if (FT_Init_FreeType(&library) != 0) {
      throw std::runtime_error("Failed to initialize FreeType");
    }
  }

  ~FontLibrary() {
    for (auto& [digest, cached] : faces) {
      (void)digest;
      if (cached.face) FT_Done_Face(cached.face);
    }
    if (library) FT_Done_FreeType(library);
  }
};

std::string strip_matching_quotes(std::string value) {
  value = trim(value);
  if (value.size() >= 2) {
    const char first = value.front();
    const char last = value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      value = value.substr(1, value.size() - 2);
    }
  }
  return trim(value);
}

std::string normalize_font_token(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    if (std::isalnum(ch)) {
      out.push_back(static_cast<char>(std::tolower(ch)));
    }
  }
  return out;
}

bool font_weight_is_boldish(const std::string& font_weight) {
  const std::string lowered = lower_copy(trim(font_weight));
  if (lowered == "bold" || lowered == "bolder" || lowered == "semibold" || lowered == "demibold") {
    return true;
  }
  const double numeric = parse_double_string(lowered, 400.0);
  return numeric >= 600.0;
}

bool font_style_is_italicish(const std::string& font_style) {
  const std::string lowered = lower_copy(trim(font_style));
  return lowered == "italic" || lowered == "oblique";
}

std::optional<std::string> discover_font_for_family(const std::string& font_family,
                                                    const std::string& font_weight,
                                                    const std::string& font_style) {
  const std::vector<std::string> families = split(font_family, ',');
  const bool want_bold = font_weight_is_boldish(font_weight);
  const bool want_italic = font_style_is_italicish(font_style);
  static const std::unordered_map<std::string, std::vector<std::string>> known_fonts = {
    {"arial", {"C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/arialmt.ttf"}},
    {"arial_bold", {"C:/Windows/Fonts/arialbd.ttf"}},
    {"arial_italic", {"C:/Windows/Fonts/ariali.ttf"}},
    {"arial_bolditalic", {"C:/Windows/Fonts/arialbi.ttf"}},
    {"segoeui", {"C:/Windows/Fonts/segoeui.ttf"}},
    {"segoeui_bold", {"C:/Windows/Fonts/seguib.ttf"}},
    {"segoeui_italic", {"C:/Windows/Fonts/segoeuii.ttf"}},
    {"segoeui_bolditalic", {"C:/Windows/Fonts/seguisbi.ttf", "C:/Windows/Fonts/seguibli.ttf"}},
    {"calibri", {"C:/Windows/Fonts/calibri.ttf"}},
    {"calibri_bold", {"C:/Windows/Fonts/calibrib.ttf"}},
    {"calibri_italic", {"C:/Windows/Fonts/calibrii.ttf"}},
    {"calibri_bolditalic", {"C:/Windows/Fonts/calibriz.ttf"}},
    {"tahoma", {"C:/Windows/Fonts/tahoma.ttf"}},
    {"tahoma_bold", {"C:/Windows/Fonts/tahomabd.ttf"}},
    {"verdana", {"C:/Windows/Fonts/verdana.ttf"}},
    {"verdana_bold", {"C:/Windows/Fonts/verdanab.ttf"}},
    {"verdana_italic", {"C:/Windows/Fonts/verdanai.ttf"}},
    {"verdana_bolditalic", {"C:/Windows/Fonts/verdanaz.ttf"}},
    {"consolas", {"C:/Windows/Fonts/consola.ttf"}},
    {"consolas_bold", {"C:/Windows/Fonts/consolab.ttf"}},
    {"consolas_italic", {"C:/Windows/Fonts/consolai.ttf"}},
    {"consolas_bolditalic", {"C:/Windows/Fonts/consolaz.ttf"}},
    {"couriernew", {"C:/Windows/Fonts/cour.ttf"}},
    {"couriernew_bold", {"C:/Windows/Fonts/courbd.ttf"}},
    {"couriernew_italic", {"C:/Windows/Fonts/couri.ttf"}},
    {"couriernew_bolditalic", {"C:/Windows/Fonts/courbi.ttf"}},
    {"timesnewroman", {"C:/Windows/Fonts/times.ttf"}},
    {"timesnewroman_bold", {"C:/Windows/Fonts/timesbd.ttf"}},
    {"timesnewroman_italic", {"C:/Windows/Fonts/timesi.ttf"}},
    {"timesnewroman_bolditalic", {"C:/Windows/Fonts/timesbi.ttf"}},
    {"dejavusans", {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"}},
    {"dejavusans_bold", {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"}},
    {"dejavusans_italic", {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf", "/usr/share/fonts/TTF/DejaVuSans-Oblique.ttf"}},
    {"dejavusans_bolditalic", {"/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf", "/usr/share/fonts/TTF/DejaVuSans-BoldOblique.ttf"}},
    {"helvetica", {"/System/Library/Fonts/Helvetica.ttc"}},
  };

  std::vector<std::string> normalized_families;
  normalized_families.reserve(families.size());
  for (const std::string& raw_family : families) {
    const std::string family = normalize_font_token(strip_matching_quotes(raw_family));
    if (!family.empty()) normalized_families.push_back(family);
  }

  for (const std::string& family : normalized_families) {
    std::vector<std::string> lookup_keys;
    if (want_bold && want_italic) lookup_keys.push_back(family + "_bolditalic");
    if (want_bold) lookup_keys.push_back(family + "_bold");
    if (want_italic) lookup_keys.push_back(family + "_italic");
    lookup_keys.push_back(family);

    for (const std::string& lookup_key : lookup_keys) {
      const auto it = known_fonts.find(lookup_key);
      if (it == known_fonts.end()) continue;
      for (const std::string& candidate : it->second) {
        if (fs::exists(candidate)) return candidate;
      }
    }
  }

  const std::vector<fs::path> search_roots = {
    fs::path("C:/Windows/Fonts"),
    fs::path("/usr/share/fonts/truetype"),
    fs::path("/usr/share/fonts/TTF"),
    fs::path("/System/Library/Fonts"),
  };

  for (const fs::path& root : search_roots) {
    if (!fs::exists(root)) continue;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
      if (!it->is_regular_file()) continue;
      const std::string normalized_name = normalize_font_token(it->path().stem().string());
      for (const std::string& family : normalized_families) {
        if (family.empty() || normalized_name.find(family) == std::string::npos) continue;
        if (want_bold && normalized_name.find("bold") == std::string::npos &&
            normalized_name.find("semibold") == std::string::npos &&
            normalized_name.find("demibold") == std::string::npos) {
          continue;
        }
        if (want_italic && normalized_name.find("italic") == std::string::npos &&
            normalized_name.find("oblique") == std::string::npos) {
          continue;
        }
        if (!want_italic && normalized_name.find("italic") != std::string::npos) continue;
        if (!want_bold && normalized_name.find("bold") != std::string::npos) continue;
        return it->path().string();
      }
    }
  }

  return std::nullopt;
}

FontLibrary& font_library() {
  // FT_Face carries mutable size and glyph-slot state. Keeping the cache per
  // thread lets independent core API calls shape at different sizes safely.
  static thread_local FontLibrary library;
  return library;
}

struct LoadedFontFace {
  FT_Face face = nullptr;
  FontIdentity identity;
};

LoadedFontFace load_font_face(const std::string& font_path) {
  FontData font = load_font_data(font_path);
  if (!font.bytes || !font.identity.sha256 || !font.identity.error.empty()) {
    throw std::runtime_error(
      font.identity.error.empty() ? "Failed to read font: " + font_path
                                  : font.identity.error);
  }
  if (font.bytes->size() > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max())) {
    throw std::runtime_error("Font is too large for FreeType: " + font_path);
  }

  FontLibrary& library = font_library();
  auto it = library.faces.find(*font.identity.sha256);
  if (it != library.faces.end()) return {it->second.face, std::move(font.identity)};

  FT_Face face = nullptr;
  if (FT_New_Memory_Face(
        library.library,
        reinterpret_cast<const FT_Byte*>(font.bytes->data()),
        static_cast<FT_Long>(font.bytes->size()),
        0,
        &face) != 0) {
    throw std::runtime_error("Failed to load font: " + font_path);
  }
  constexpr std::size_t max_cached_faces = 128;
  if (library.faces.size() >= max_cached_faces) {
    const std::string oldest_digest = library.face_order.front();
    library.face_order.pop_front();
    const auto oldest = library.faces.find(oldest_digest);
    if (oldest != library.faces.end()) {
      if (oldest->second.face) FT_Done_Face(oldest->second.face);
      library.faces.erase(oldest);
    }
  }
  try {
    library.face_order.push_back(*font.identity.sha256);
  } catch (...) {
    FT_Done_Face(face);
    throw;
  }
  try {
    // Keep the local shared_ptr alive until insertion completes. FreeType may
    // read the memory during FT_Done_Face, so an allocation failure must not
    // destroy the face's sole backing buffer before cleanup.
    const auto [inserted_face, inserted] = library.faces.emplace(
      *font.identity.sha256, FontLibrary::CachedFace{font.bytes, face});
    if (!inserted) {
      library.face_order.pop_back();
      FT_Done_Face(face);
      return {inserted_face->second.face, std::move(font.identity)};
    }
  } catch (...) {
    library.face_order.pop_back();
    FT_Done_Face(face);
    throw;
  }
  return {face, std::move(font.identity)};
}

struct OutlineBuilder {
  std::string d;
  double offset_x = 0.0;
  double offset_y = 0.0;
  bool contour_open = false;
};

using HbBufferPtr = std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)>;
using HbFontPtr = std::unique_ptr<hb_font_t, decltype(&hb_font_destroy)>;

struct ShapedRun {
  std::vector<hb_glyph_info_t> glyphs;
  std::vector<hb_glyph_position_t> positions;
  hb_direction_t direction = HB_DIRECTION_INVALID;
};

bool is_neutral_script(hb_script_t script) {
  return script == HB_SCRIPT_INVALID || script == HB_SCRIPT_UNKNOWN ||
         script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED;
}

std::vector<hb_script_t> resolve_scripts(const std::vector<char32_t>& codepoints) {
  hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
  std::vector<hb_script_t> scripts;
  scripts.reserve(codepoints.size());
  for (const char32_t codepoint : codepoints) {
    scripts.push_back(hb_unicode_script(unicode, static_cast<hb_codepoint_t>(codepoint)));
  }

  hb_script_t preceding = HB_SCRIPT_INVALID;
  for (hb_script_t& script : scripts) {
    if (is_neutral_script(script)) {
      if (!is_neutral_script(preceding)) script = preceding;
    } else {
      preceding = script;
    }
  }

  hb_script_t following = HB_SCRIPT_INVALID;
  for (auto it = scripts.rbegin(); it != scripts.rend(); ++it) {
    if (is_neutral_script(*it)) {
      if (!is_neutral_script(following)) *it = following;
    } else {
      following = *it;
    }
  }
  return scripts;
}

bool has_coordinate_value(const std::vector<double>& values, const std::size_t index) {
  return index < values.size() && std::isfinite(values[index]);
}

bool has_character_position(const std::size_t index,
                            const std::vector<double>& x_values,
                            const std::vector<double>& y_values,
                            const std::vector<double>& dx_values,
                            const std::vector<double>& dy_values) {
  return has_coordinate_value(x_values, index) || has_coordinate_value(y_values, index) ||
         has_coordinate_value(dx_values, index) || has_coordinate_value(dy_values, index);
}

ShapedRun shape_run(hb_font_t* font,
                    const std::vector<char32_t>& codepoints,
                    const std::size_t begin,
                    const std::size_t end,
                    const hb_script_t script,
                    const bool disable_ligatures) {
  HbBufferPtr buffer(hb_buffer_create(), &hb_buffer_destroy);
  if (!buffer || hb_buffer_allocation_successful(buffer.get()) == 0) {
    throw std::runtime_error("Failed to allocate a HarfBuzz shaping buffer");
  }

  std::vector<hb_codepoint_t> shaping_context;
  shaping_context.reserve(codepoints.size());
  for (const char32_t codepoint : codepoints) {
    shaping_context.push_back(static_cast<hb_codepoint_t>(codepoint));
  }
  hb_buffer_add_codepoints(
    buffer.get(), shaping_context.data(), static_cast<int>(shaping_context.size()),
    static_cast<unsigned int>(begin), static_cast<int>(end - begin));
  hb_buffer_set_cluster_level(buffer.get(), HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
  if (!is_neutral_script(script)) hb_buffer_set_script(buffer.get(), script);
  hb_buffer_guess_segment_properties(buffer.get());

  std::vector<hb_feature_t> features;
  if (disable_ligatures) {
    features.push_back({HB_TAG('l', 'i', 'g', 'a'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
    features.push_back({HB_TAG('c', 'l', 'i', 'g'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
  }
  hb_shape(font, buffer.get(), features.data(), static_cast<unsigned int>(features.size()));

  unsigned int glyph_count = 0;
  const hb_glyph_info_t* glyphs = hb_buffer_get_glyph_infos(buffer.get(), &glyph_count);
  const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer.get(), nullptr);
  ShapedRun result;
  result.direction = hb_buffer_get_direction(buffer.get());
  if (glyph_count != 0 && glyphs && positions) {
    result.glyphs.assign(glyphs, glyphs + glyph_count);
    result.positions.assign(positions, positions + glyph_count);
  }
  return result;
}

int ft_move_to_cb(const FT_Vector* to, void* user) {
  auto* builder = static_cast<OutlineBuilder*>(user);
  if (builder->contour_open) {
    builder->d += "Z";
    builder->contour_open = false;
  }
  builder->d += "M" + fmt(builder->offset_x + to->x / 64.0) + "," + fmt(builder->offset_y - to->y / 64.0);
  builder->contour_open = true;
  return 0;
}

int ft_line_to_cb(const FT_Vector* to, void* user) {
  auto* builder = static_cast<OutlineBuilder*>(user);
  builder->d += "L" + fmt(builder->offset_x + to->x / 64.0) + "," + fmt(builder->offset_y - to->y / 64.0);
  builder->contour_open = true;
  return 0;
}

int ft_conic_to_cb(const FT_Vector* control, const FT_Vector* to, void* user) {
  auto* builder = static_cast<OutlineBuilder*>(user);
  builder->d += "Q" +
    fmt(builder->offset_x + control->x / 64.0) + "," + fmt(builder->offset_y - control->y / 64.0) + " " +
    fmt(builder->offset_x + to->x / 64.0) + "," + fmt(builder->offset_y - to->y / 64.0);
  builder->contour_open = true;
  return 0;
}

int ft_cubic_to_cb(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user) {
  auto* builder = static_cast<OutlineBuilder*>(user);
  builder->d += "C" +
    fmt(builder->offset_x + control1->x / 64.0) + "," + fmt(builder->offset_y - control1->y / 64.0) + " " +
    fmt(builder->offset_x + control2->x / 64.0) + "," + fmt(builder->offset_y - control2->y / 64.0) + " " +
    fmt(builder->offset_x + to->x / 64.0) + "," + fmt(builder->offset_y - to->y / 64.0);
  builder->contour_open = true;
  return 0;
}

}  // namespace

std::vector<char32_t> decode_utf8(const std::string& text) {
  std::vector<char32_t> codepoints;
  codepoints.reserve(text.size());

  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto byte = [&text](std::size_t index) {
      return static_cast<unsigned char>(text[index]);
    };
    const unsigned char lead = byte(cursor);
    if (lead <= 0x7f) {
      codepoints.push_back(static_cast<char32_t>(lead));
      ++cursor;
      continue;
    }

    std::size_t length = 0;
    char32_t codepoint = 0;
    if (lead >= 0xc2 && lead <= 0xdf) {
      length = 2;
      codepoint = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      length = 3;
      codepoint = lead & 0x0f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      length = 4;
      codepoint = lead & 0x07;
    } else {
      codepoints.push_back(0xfffd);
      ++cursor;
      continue;
    }

    if (cursor + length > text.size()) {
      codepoints.push_back(0xfffd);
      ++cursor;
      continue;
    }

    bool valid = true;
    for (std::size_t offset = 1; offset < length; ++offset) {
      if ((byte(cursor + offset) & 0xc0) != 0x80) {
        valid = false;
        break;
      }
    }
    if (valid && length == 3) {
      const unsigned char second = byte(cursor + 1);
      valid = (lead != 0xe0 || second >= 0xa0) && (lead != 0xed || second <= 0x9f);
    }
    if (valid && length == 4) {
      const unsigned char second = byte(cursor + 1);
      valid = (lead != 0xf0 || second >= 0x90) && (lead != 0xf4 || second <= 0x8f);
    }
    if (!valid) {
      codepoints.push_back(0xfffd);
      ++cursor;
      continue;
    }

    for (std::size_t offset = 1; offset < length; ++offset) {
      codepoint = (codepoint << 6) | (byte(cursor + offset) & 0x3f);
    }
    codepoints.push_back(codepoint);
    cursor += length;
  }

  return codepoints;
}

std::string collect_direct_text(const pugi::xml_node& node) {
  std::string text;
  for (pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
      text += child.value();
    }
  }
  return trim(text);
}

std::optional<std::string> inherited_attr(const pugi::xml_node& node, const char* name) {
  for (pugi::xml_node current = node; current; current = current.parent()) {
    if (current.attribute(name)) {
      return std::string(current.attribute(name).as_string());
    }
  }
  return std::nullopt;
}

std::optional<std::string> discover_default_font() {
  static const std::vector<std::string> candidates = {
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/calibri.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/SFNSText.ttf",
  };

  for (const std::string& candidate : candidates) {
    if (fs::exists(candidate)) return candidate;
  }
  return std::nullopt;
}

double parse_svg_length(const std::string& value, double fallback) {
  double parsed = 0.0;
  return parse_finite_length(value, parsed) ? parsed : fallback;
}

TextLayoutResult text_to_path(const std::string& text,
                              double x,
                              double y,
                              double font_size,
                              const std::string& font_path,
                              double letter_spacing,
                              const std::vector<double>& x_values,
                              const std::vector<double>& y_values,
                              const std::vector<double>& dx_values,
                              const std::vector<double>& dy_values) {
  TextLayoutResult result;
  result.end_x = x;
  result.end_y = y;
  if (text.empty()) return result;

  LoadedFontFace loaded_font = load_font_face(font_path);
  FT_Face face = loaded_font.face;
  result.font_identity = std::move(loaded_font.identity);
  if (FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(std::llround(font_size * 64.0)), 72, 72) != 0) {
    throw std::runtime_error("Failed to set font size for: " + font_path);
  }
  HbFontPtr shaping_font(hb_ft_font_create_referenced(face), &hb_font_destroy);
  if (!shaping_font) {
    throw std::runtime_error("Failed to create a HarfBuzz font for: " + font_path);
  }
  hb_ft_font_set_load_flags(shaping_font.get(), FT_LOAD_NO_BITMAP);

  OutlineBuilder builder;
  FT_Outline_Funcs funcs{};
  funcs.move_to = ft_move_to_cb;
  funcs.line_to = ft_line_to_cb;
  funcs.conic_to = ft_conic_to_cb;
  funcs.cubic_to = ft_cubic_to_cb;
  funcs.shift = 0;
  funcs.delta = 0;

  double pen_x = x;
  double pen_y = y;

  const std::vector<char32_t> codepoints = decode_utf8(text);
  if (codepoints.empty()) return result;
  const std::vector<hb_script_t> scripts = resolve_scripts(codepoints);

  std::size_t run_begin = 0;
  while (run_begin < codepoints.size()) {
    if (has_coordinate_value(x_values, run_begin)) pen_x = x_values[run_begin];
    if (has_coordinate_value(y_values, run_begin)) pen_y = y_values[run_begin];
    if (has_coordinate_value(dx_values, run_begin)) pen_x += dx_values[run_begin];
    if (has_coordinate_value(dy_values, run_begin)) pen_y += dy_values[run_begin];

    std::size_t run_end = run_begin + 1;
    while (run_end < codepoints.size() &&
           !has_character_position(run_end, x_values, y_values, dx_values, dy_values) &&
           scripts[run_end] == scripts[run_begin]) {
      ++run_end;
    }

    // Letter spacing addresses typographic character boundaries. Turning off
    // standard ligatures in that case keeps those boundaries addressable.
    const ShapedRun shaped = shape_run(
      shaping_font.get(), codepoints, run_begin, run_end, scripts[run_begin],
      std::abs(letter_spacing) > 1e-12);
    result.glyph_count += shaped.glyphs.size();
    result.has_right_to_left_run =
      result.has_right_to_left_run || HB_DIRECTION_IS_BACKWARD(shaped.direction);
    for (const hb_glyph_info_t& glyph : shaped.glyphs) {
      if (glyph.codepoint != 0 || codepoints.empty()) continue;
      const std::size_t character_index = std::min<std::size_t>(
        static_cast<std::size_t>(glyph.cluster), codepoints.size() - 1);
      const char32_t missing = codepoints[character_index];
      if (std::find(result.missing_codepoints.begin(), result.missing_codepoints.end(), missing) ==
          result.missing_codepoints.end()) {
        result.missing_codepoints.push_back(missing);
      }
    }
    std::size_t glyph_index = 0;
    while (glyph_index < shaped.glyphs.size()) {
      const std::uint32_t cluster = shaped.glyphs[glyph_index].cluster;
      std::size_t cluster_end = glyph_index + 1;
      while (cluster_end < shaped.glyphs.size() &&
             shaped.glyphs[cluster_end].cluster == cluster) {
        ++cluster_end;
      }

      for (std::size_t index = glyph_index; index < cluster_end; ++index) {
        const hb_glyph_info_t& glyph = shaped.glyphs[index];
        const hb_glyph_position_t& position = shaped.positions[index];
        if (FT_Load_Glyph(face, glyph.codepoint, FT_LOAD_NO_BITMAP) == 0 &&
            face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
          builder.offset_x = pen_x + position.x_offset / 64.0;
          builder.offset_y = pen_y - position.y_offset / 64.0;
          FT_Outline_Decompose(&face->glyph->outline, &funcs, &builder);
          if (builder.contour_open) {
            builder.d += "Z";
            builder.contour_open = false;
          }
        }
        pen_x += position.x_advance / 64.0;
        pen_y -= position.y_advance / 64.0;
      }

      const bool has_following_character =
        cluster_end < shaped.glyphs.size() || run_end < codepoints.size();
      if (has_following_character) pen_x += letter_spacing;
      glyph_index = cluster_end;
    }
    run_begin = run_end;
  }

  result.d = builder.d;
  result.end_x = pen_x;
  result.end_y = pen_y;
  return result;
}

std::string text_to_path(const std::string& text,
                         double x,
                         double y,
                         double font_size,
                         const std::string& font_path) {
  return text_to_path(text, x, y, font_size, font_path, 0.0, {}, {}, {}, {}).d;
}

std::string text_to_path(const std::string& text,
                         double x,
                         double y,
                         double font_size,
                         const std::string& font_path,
                         double letter_spacing) {
  return text_to_path(text, x, y, font_size, font_path, letter_spacing, {}, {}, {}, {}).d;
}

double measure_text_advance(const std::string& text, double font_size, const std::string& font_path) {
  return measure_text_advance(text, font_size, font_path, 0.0);
}

double measure_text_advance(const std::string& text,
                            double font_size,
                            const std::string& font_path,
                            double letter_spacing) {
  if (text.empty()) return 0.0;
  return text_to_path(text, 0.0, 0.0, font_size, font_path, letter_spacing, {}, {}, {}, {}).end_x;
}

std::optional<std::string> resolve_text_font_path(const StyleState& style,
                                                  const std::optional<std::string>& fallback_font_path,
                                                  bool fallback_is_authoritative) {
  return resolve_text_font(style, fallback_font_path, fallback_is_authoritative).path;
}

TextFontResolution resolve_text_font(const StyleState& style,
                                     const std::optional<std::string>& fallback_font_path,
                                     bool fallback_is_authoritative) {
  TextFontResolution result;
  if (fallback_is_authoritative && fallback_font_path.has_value()) {
    result.path = fallback_font_path;
    result.used_authoritative_font = true;
    return result;
  }
  if (!style.font_family.empty()) {
    if (const auto family_font = discover_font_for_family(style.font_family, style.font_weight, style.font_style)) {
      result.path = family_font;
      return result;
    }
    result.requested_family_unresolved = true;
  }
  result.path = fallback_font_path;
  result.used_fallback = result.requested_family_unresolved && fallback_font_path.has_value();
  return result;
}

double first_coord_value(const pugi::xml_node& node, const char* attr_name, double fallback) {
  if (!node.attribute(attr_name)) return fallback;
  const std::vector<double> values = parse_length_list(node.attribute(attr_name).as_string());
  if (!values.empty()) return values.front();
  return fallback;
}

std::vector<double> coord_values(const pugi::xml_node& node, const char* attr_name) {
  if (!node.attribute(attr_name)) return {};
  return parse_length_list(node.attribute(attr_name).as_string());
}

}  // namespace svg_squisher
