#include "svg_squisher.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "svg_computed_style.h"
#include "svg_diagnostics.h"
#include "svg_font_identity.h"
#include "svg_output.h"
#include "svg_postprocess.h"
#include "svg_path.h"
#include "svg_style.h"
#include "svg_text.h"
#include "svg_traversal.h"
#include "svg_util.h"

namespace fs = std::filesystem;

namespace svg_squisher {
namespace {

std::size_t warning_count(const std::vector<Diagnostic>& diagnostics) {
  return static_cast<std::size_t>(std::count_if(
      diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Warning;
      }));
}

std::string strict_error(const std::vector<Diagnostic>& diagnostics) {
  for (const Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == DiagnosticSeverity::Warning) {
      return "Strict conversion rejected " + diagnostic.code + " at " + diagnostic.element +
             ": " + diagnostic.message;
    }
  }
  return "Strict conversion rejected unsupported SVG content";
}

bool has_visible_painted_input(const pugi::xml_node& node,
                               const std::vector<CssRule>& rules,
                               const StyleState& inherited) {
  if (node.type() != pugi::node_element) return false;
  const std::string name = node.name();
  if (name == "defs" || name == "style" || name == "script" || name == "metadata") {
    return false;
  }

  const StyleState style = resolve_style(node, rules, inherited);
  const ComputedStyle computed = compute_style(style);
  if (!computed.displayed) return false;

  const bool geometry = name == "path" || name == "rect" || name == "circle" ||
                        name == "ellipse" || name == "polyline" || name == "polygon" ||
                        name == "text" || name == "use";
  if (computed.visible &&
      ((geometry && (computed.has_fill || computed.has_stroke)) ||
       (name == "line" && computed.has_stroke))) {
    return true;
  }

  for (const pugi::xml_node child : node.children()) {
    if (has_visible_painted_input(child, rules, style)) return true;
  }
  return false;
}

void append_batch_result(BatchResult& batch, FileConversionResult file) {
  batch.warnings += warning_count(file.diagnostics);
  if (file.skipped) ++batch.skipped;
  else if (file.success) ++batch.converted;
  else ++batch.failed;
  batch.files.push_back(std::move(file));
}

bool paths_are_equivalent(const fs::path& left, const fs::path& right) {
  std::error_code error;
  const bool equivalent = fs::equivalent(left, right, error);
  if (!error) return equivalent;

  std::error_code left_error;
  std::error_code right_error;
  const fs::path normalized_left = fs::weakly_canonical(left, left_error);
  const fs::path normalized_right = fs::weakly_canonical(right, right_error);
  return !left_error && !right_error && normalized_left == normalized_right;
}

}  // namespace

ConversionResult SvgSquisher::convert_string(const std::string& svg_text,
                                             const Options& options) const {
  ConversionResult result;
  result.stats.input_bytes = svg_text.size();

  try {
    if (options.precision < 0 || options.precision > 15) {
      throw std::runtime_error("Output precision must be between 0 and 15");
    }

    pugi::xml_document doc;
    const pugi::xml_parse_result parse_result =
        doc.load_buffer(svg_text.data(), svg_text.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!parse_result) {
      throw std::runtime_error("Failed to parse SVG XML at byte " +
                               std::to_string(parse_result.offset) + ": " +
                               parse_result.description());
    }

    const pugi::xml_node svg_node = doc.document_element();
    if (!svg_node || std::string(svg_node.name()) != "svg") {
      throw std::runtime_error("No <svg> root element found");
    }

    validate_svg_structure_depth(svg_node);
    result.stats.input_elements = count_svg_elements(svg_node);
    for (const pugi::xpath_node& path : svg_node.select_nodes(".//path[@d]")) {
      result.stats.input_path_commands +=
        count_path_commands(path.node().attribute("d").as_string());
    }
    result.diagnostics = inspect_svg_capabilities(svg_node);

    const auto css_rules = parse_css_rules(svg_node);
    std::vector<PathEntry> paths;
    const StyleState root_style = resolve_style(svg_node, css_rules, StyleState{});
    const std::optional<std::string> font_path =
        options.font_path.has_value() ? options.font_path : discover_default_font();

    const bool has_text = static_cast<bool>(svg_node.select_node(".//text"));
    if (!font_path && has_text) {
      result.diagnostics.push_back({
          DiagnosticSeverity::Warning,
          "missing-font",
          "Text could not be converted because no usable font was found; provide --font.",
          "<text>",
      });
    } else if (font_path && has_text && !options.font_path.has_value()) {
      result.diagnostics.push_back({
          DiagnosticSeverity::Warning,
          "implicit-font-selection",
          "Text uses system font discovery (initial candidate: '" + *font_path +
            "'); pass --font for reproducible output.",
          "<text>",
      });
    }

    if (options.strict && warning_count(result.diagnostics) != 0) {
      result.error = strict_error(result.diagnostics);
      return result;
    }

    OutputPrecisionScope precision_scope(options.precision);
    collect_paths_from_svg(svg_node,
                           css_rules,
                           root_style,
                           font_path,
                           paths,
                           options.font_path.has_value(),
                           &result.fonts_used,
                           &result.diagnostics,
                           &result.stats.missing_glyphs);

    result.font_identities.reserve(result.fonts_used.size());
    for (const std::string& used_font : result.fonts_used) {
      FontIdentity identity = identify_font_file(used_font);
      if (!identity.error.empty()) {
        result.diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "font-identity-unavailable",
            "Could not capture a stable byte identity for font '" + used_font + "': " +
                identity.error,
            "<text>",
        });
      }
      result.font_identities.push_back(std::move(identity));
    }

    if (paths.empty() && has_visible_painted_input(svg_node, css_rules, root_style)) {
      result.diagnostics.push_back({
          DiagnosticSeverity::Warning,
          "empty-output",
          "The input contains renderable elements but conversion produced no paths.",
          "<svg>",
      });
    }

    // Traversal, text shaping, and font identity can add diagnostics after preflight.
    // Keep the final strict gate immediately before serialization.
    if (options.strict && warning_count(result.diagnostics) != 0) {
      result.error = strict_error(result.diagnostics);
      return result;
    }

    const std::vector<PathEntry> final_paths =
        prepare_output_paths(svg_node, paths, options.remove_background);
    result.svg = render_svg_document(svg_node, final_paths, options.fill_override);
    for (const PathEntry& path : final_paths) {
      const std::size_t emitted_copies =
        (path.emit_fill ? 1U : 0U) + (path.emit_stroke ? 1U : 0U);
      result.stats.output_paths += emitted_copies;
      result.stats.output_path_commands += emitted_copies * count_path_commands(path.d);
    }
    result.stats.output_bytes = result.svg.size();
    result.success = true;
  } catch (const std::exception& ex) {
    result.error = ex.what();
  }

  return result;
}

std::string SvgSquisher::squish_string(const std::string& svg_text,
                                       const Options& options) const {
  ConversionResult result = convert_string(svg_text, options);
  if (!result.success) {
    throw std::runtime_error(result.error.empty() ? "SVG conversion failed" : result.error);
  }
  return std::move(result.svg);
}

FileConversionResult SvgSquisher::squish_file_with_result(
    const fs::path& input_path,
    const fs::path& output_path,
    const Options& options) const {
  FileConversionResult file;
  file.input_path = input_path;
  file.output_path = output_path;

  try {
    if (!options.allow_in_place && paths_are_equivalent(input_path, output_path)) {
      file.error =
        "Input and output resolve to the same path; enable allow_in_place explicitly";
      return file;
    }
    if (!options.overwrite && fs::exists(output_path)) {
      file.skipped = true;
      file.error = "Output already exists (use --overwrite to replace it)";
      return file;
    }

    const std::string input = read_file(input_path);
    ConversionResult conversion = convert_string(input, options);
    file.diagnostics = std::move(conversion.diagnostics);
    file.fonts_used = std::move(conversion.fonts_used);
    file.font_identities = std::move(conversion.font_identities);
    file.stats = conversion.stats;
    if (!conversion.success) {
      file.error = std::move(conversion.error);
      return file;
    }

    write_file(output_path, conversion.svg);
    file.success = true;
  } catch (const std::exception& ex) {
    file.error = ex.what();
  }
  return file;
}

void SvgSquisher::squish_file(const fs::path& input_path,
                              const fs::path& output_path,
                              const Options& options) const {
  const FileConversionResult result = squish_file_with_result(input_path, output_path, options);
  if (!result.success) {
    throw std::runtime_error(result.error.empty() ? "SVG conversion failed" : result.error);
  }
}

BatchResult SvgSquisher::squish_directory_with_result(
    const fs::path& input_dir,
    const fs::path& output_dir,
    const Options& options) const {
  BatchResult batch;
  if (!fs::is_directory(input_dir)) {
    FileConversionResult error;
    error.input_path = input_dir;
    error.output_path = output_dir;
    error.error = "Input directory does not exist or is not a directory";
    append_batch_result(batch, std::move(error));
    return batch;
  }

  std::vector<fs::path> inputs;
  std::error_code iteration_error;
  if (options.recursive) {
    for (fs::recursive_directory_iterator it(input_dir, iteration_error), end;
         it != end && !iteration_error;
         it.increment(iteration_error)) {
      std::error_code type_error;
      if (it->is_directory(type_error)) {
        if (paths_are_equivalent(it->path(), output_dir)) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (type_error) {
        iteration_error = type_error;
        break;
      }
      if (it->is_regular_file(type_error) &&
          lower_copy(it->path().extension().string()) == ".svg") {
        inputs.push_back(it->path());
      }
      if (type_error) {
        iteration_error = type_error;
        break;
      }
    }
  } else {
    for (fs::directory_iterator it(input_dir, iteration_error), end;
         it != end && !iteration_error;
         it.increment(iteration_error)) {
      if (it->is_regular_file() && lower_copy(it->path().extension().string()) == ".svg") {
        inputs.push_back(it->path());
      }
    }
  }

  if (iteration_error) {
    FileConversionResult error;
    error.input_path = input_dir;
    error.output_path = output_dir;
    error.error = "Unable to enumerate input directory: " + iteration_error.message();
    append_batch_result(batch, std::move(error));
    return batch;
  }

  std::sort(inputs.begin(), inputs.end(), [](const fs::path& left, const fs::path& right) {
    return left.generic_string() < right.generic_string();
  });

  for (const fs::path& input : inputs) {
    std::error_code relative_error;
    fs::path relative = options.recursive ? fs::relative(input, input_dir, relative_error)
                                          : input.filename();
    if (relative_error) relative = input.filename();
    FileConversionResult file =
        squish_file_with_result(input, output_dir / relative, options);
    const bool failed = !file.success && !file.skipped;
    append_batch_result(batch, std::move(file));
    if (failed && !options.continue_on_error) break;
  }

  return batch;
}

void SvgSquisher::squish_directory(const fs::path& input_dir,
                                   const fs::path& output_dir,
                                   const Options& options) const {
  const BatchResult result = squish_directory_with_result(input_dir, output_dir, options);
  if (result.failed != 0) {
    for (const FileConversionResult& file : result.files) {
      if (!file.success && !file.skipped) {
        throw std::runtime_error(file.error.empty() ? "Directory conversion failed" : file.error);
      }
    }
  }
}

}  // namespace svg_squisher
