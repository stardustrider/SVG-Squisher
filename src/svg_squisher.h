#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svg_squisher {

struct Options {
  std::optional<std::string> fill_override;
  std::optional<std::string> font_path;
  bool remove_background = false;
  bool strict = false;
  bool recursive = false;
  bool continue_on_error = true;
  bool overwrite = true;
  bool allow_in_place = false;
  int precision = 4;
};

enum class DiagnosticSeverity {
  Warning,
  Error,
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Warning;
  std::string code;
  std::string message;
  std::string element;
};

struct ConversionStats {
  std::uintmax_t input_bytes = 0;
  std::uintmax_t output_bytes = 0;
  std::size_t input_elements = 0;
  std::size_t input_path_commands = 0;
  std::size_t output_paths = 0;
  std::size_t output_path_commands = 0;
  std::size_t missing_glyphs = 0;
};

struct FontIdentity {
  std::string path;
  std::optional<std::uintmax_t> bytes;
  std::optional<std::string> sha256;
  std::string error;
};

struct ConversionResult {
  bool success = false;
  std::string svg;
  std::string error;
  std::vector<Diagnostic> diagnostics;
  std::vector<std::string> fonts_used;
  std::vector<FontIdentity> font_identities;
  ConversionStats stats;
};

struct FileConversionResult {
  std::filesystem::path input_path;
  std::filesystem::path output_path;
  bool success = false;
  bool skipped = false;
  std::string error;
  std::vector<Diagnostic> diagnostics;
  std::vector<std::string> fonts_used;
  std::vector<FontIdentity> font_identities;
  ConversionStats stats;
};

struct BatchResult {
  std::vector<FileConversionResult> files;
  std::size_t converted = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  std::size_t warnings = 0;
};

struct CssRule {
  std::string selector;
  std::vector<std::pair<std::string, std::string>> declarations;
};

struct PathEntry {
  std::string d;
  std::string transform;
  std::string fill;
  std::string stroke;
  std::string stroke_width;
  std::string stroke_dasharray;
  std::string stroke_linecap;
  std::string stroke_linejoin;
  std::string stroke_miterlimit;
  std::string fill_rule;
  std::string opacity;
  std::string fill_opacity;
  std::string stroke_opacity;
  std::size_t compositing_group = 0;
  bool emit_fill = false;
  bool emit_stroke = false;
};

class SvgSquisher {
public:
  ConversionResult convert_string(const std::string& svg_text,
                                  const Options& options = {}) const;
  std::string squish_string(const std::string& svg_text, const Options& options = {}) const;
  FileConversionResult squish_file_with_result(
      const std::filesystem::path& input_path,
      const std::filesystem::path& output_path,
      const Options& options = {}) const;
  void squish_file(const std::filesystem::path& input_path,
                   const std::filesystem::path& output_path,
                   const Options& options = {}) const;
  BatchResult squish_directory_with_result(
      const std::filesystem::path& input_dir,
      const std::filesystem::path& output_dir,
      const Options& options = {}) const;
  void squish_directory(const std::filesystem::path& input_dir,
                        const std::filesystem::path& output_dir,
                        const Options& options = {}) const;
};

}  // namespace svg_squisher
