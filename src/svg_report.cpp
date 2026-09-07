#include "svg_report.h"

#include <algorithm>
#include <sstream>
#include <string>

#include "svg_diagnostics.h"
#include "svg_output.h"

#ifndef SVG_SQUISHER_VERSION
#define SVG_SQUISHER_VERSION "0.0.0+unknown"
#endif

namespace fs = std::filesystem;

namespace svg_squisher {
namespace {

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          static const char hex[] = "0123456789abcdef";
          out << "\\u00" << hex[(ch >> 4) & 0xf] << hex[ch & 0xf];
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

void render_diagnostics(std::ostringstream& out, const std::vector<Diagnostic>& diagnostics) {
  out << "[";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0) out << ",";
    const Diagnostic& diagnostic = diagnostics[i];
    out << "{\"severity\":\"" << diagnostic_severity_name(diagnostic.severity) << "\""
        << ",\"code\":\"" << json_escape(diagnostic.code) << "\""
        << ",\"message\":\"" << json_escape(diagnostic.message) << "\""
        << ",\"element\":\"" << json_escape(diagnostic.element) << "\"}";
  }
  out << "]";
}

void render_strings(std::ostringstream& out, const std::vector<std::string>& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ",";
    out << "\"" << json_escape(values[i]) << "\"";
  }
  out << "]";
}

void render_font_identities(std::ostringstream& out,
                            const std::vector<FontIdentity>& identities) {
  out << "[";
  for (std::size_t i = 0; i < identities.size(); ++i) {
    if (i != 0) out << ",";
    const FontIdentity& identity = identities[i];
    out << "{\"path\":\"" << json_escape(identity.path) << "\",\"bytes\":";
    if (identity.bytes) out << *identity.bytes;
    else out << "null";
    out << ",\"sha256\":";
    if (identity.sha256) out << "\"" << json_escape(*identity.sha256) << "\"";
    else out << "null";
    out << ",\"error\":";
    if (identity.error.empty()) out << "null";
    else out << "\"" << json_escape(identity.error) << "\"";
    out << "}";
  }
  out << "]";
}

std::string report_path(const fs::path& value,
                        const std::optional<fs::path>& report_parent) {
  if (!report_parent) return value.generic_u8string();

  const fs::path absolute_value = value.is_absolute()
    ? value.lexically_normal()
    : fs::absolute(value).lexically_normal();
  const fs::path relative = absolute_value.lexically_relative(*report_parent);
  return relative.empty() ? absolute_value.generic_u8string() : relative.generic_u8string();
}

std::string render_json_report_impl(const BatchResult& result,
                                    const Options& options,
                                    const std::optional<fs::path>& report_parent) {
  std::ostringstream out;
  out << "{\n"
      << "  \"schemaVersion\": 1,\n"
      << "  \"generator\": \"SVG Squisher\",\n"
      << "  \"generatorVersion\": \"" << json_escape(SVG_SQUISHER_VERSION) << "\",\n"
      << "  \"pathBase\": \"page\",\n"
      << "  \"options\": {"
      << "\"strict\":" << (options.strict ? "true" : "false")
      << ",\"recursive\":" << (options.recursive ? "true" : "false")
      << ",\"continueOnError\":" << (options.continue_on_error ? "true" : "false")
      << ",\"removeBackground\":" << (options.remove_background ? "true" : "false")
      << ",\"overwrite\":" << (options.overwrite ? "true" : "false")
      << ",\"allowInPlace\":" << (options.allow_in_place ? "true" : "false")
      << ",\"precision\":" << options.precision
      << ",\"fill\":";
  if (options.fill_override) out << "\"" << json_escape(*options.fill_override) << "\"";
  else out << "null";
  out << ",\"font\":";
  if (options.font_path) out << "\"" << json_escape(*options.font_path) << "\"";
  else out << "null";
  out << "},\n"
      << "  \"summary\": {\"converted\":" << result.converted
      << ",\"failed\":" << result.failed
      << ",\"skipped\":" << result.skipped
      << ",\"warnings\":" << result.warnings << "},\n"
      << "  \"files\": [\n";

  for (std::size_t i = 0; i < result.files.size(); ++i) {
    const FileConversionResult& file = result.files[i];
    out << "    {\"input\":\"" << json_escape(report_path(file.input_path, report_parent)) << "\""
        << ",\"output\":\"" << json_escape(report_path(file.output_path, report_parent)) << "\""
        << ",\"status\":\"" << (file.skipped ? "skipped" : (file.success ? "converted" : "failed")) << "\""
        << ",\"error\":";
    if (file.error.empty()) out << "null";
    else out << "\"" << json_escape(file.error) << "\"";
    out << ",\"inputBytes\":" << file.stats.input_bytes
        << ",\"outputBytes\":" << file.stats.output_bytes
        << ",\"inputElements\":" << file.stats.input_elements
        << ",\"inputPathCommands\":" << file.stats.input_path_commands
        << ",\"outputPaths\":" << file.stats.output_paths
        << ",\"outputPathCommands\":" << file.stats.output_path_commands
        << ",\"missingGlyphs\":" << file.stats.missing_glyphs
        << ",\"fonts\":";
    render_strings(out, file.fonts_used);
    out << ",\"fontIdentities\":";
    render_font_identities(out, file.font_identities);
    out << ",\"diagnostics\":";
    render_diagnostics(out, file.diagnostics);
    out << "}" << (i + 1 == result.files.size() ? "" : ",") << "\n";
  }

  out << "  ]\n}\n";
  return out.str();
}

}  // namespace

std::string render_json_report(const BatchResult& result, const Options& options) {
  return render_json_report_impl(result, options, std::nullopt);
}

void write_json_report(const std::filesystem::path& path,
                       const BatchResult& result,
                       const Options& options) {
  const fs::path report_parent = fs::absolute(path).lexically_normal().parent_path();
  write_file(path, render_json_report_impl(result, options, report_parent));
}

BatchResult batch_result_from_file(const FileConversionResult& file) {
  BatchResult result;
  result.files.push_back(file);
  if (file.skipped) ++result.skipped;
  else if (file.success) ++result.converted;
  else ++result.failed;
  result.warnings = static_cast<std::size_t>(std::count_if(
    file.diagnostics.begin(), file.diagnostics.end(), [](const Diagnostic& diagnostic) {
      return diagnostic.severity == DiagnosticSeverity::Warning;
    }));
  return result;
}

}  // namespace svg_squisher
