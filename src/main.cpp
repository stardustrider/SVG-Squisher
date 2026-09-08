#include "svg_squisher.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "svg_diagnostics.h"
#include "svg_report.h"

#ifndef SVG_SQUISHER_VERSION
#define SVG_SQUISHER_VERSION "development"
#endif

namespace fs = std::filesystem;
using svg_squisher::BatchResult;
using svg_squisher::Diagnostic;
using svg_squisher::FileConversionResult;
using svg_squisher::ConversionPolicy;
using svg_squisher::Options;
using svg_squisher::SvgSquisher;

namespace {

void print_usage(std::ostream& out) {
  out
      << "SVG Squisher " << SVG_SQUISHER_VERSION << "\n\n"
      << "Convert icon-oriented SVG content into explicit paths.\n\n"
      << "Usage:\n"
      << "  svg_squisher <input.svg> <output.svg> [options]\n"
      << "  svg_squisher <input-dir> <output-dir> [options]\n\n"
      << "Conversion options:\n"
      << "  --fill <color>          Recolor emitted fills and strokes without removing geometry\n"
      << "  --font <path>           Authoritative font file for text-to-path conversion\n"
      << "  --conversion-policy <preserve-appearance|filled-paths>\n"
      << "                          Select live-stroke preservation or filled paths\n"
      << "  --precision <0-15>      Decimal precision for generated coordinates (default: 4)\n"
      << "  --remove-background     Apply the icon-background removal heuristic\n"
      << "  --strict                Reject input that uses unsupported SVG semantics\n"
      << "  --in-place              Explicitly allow input and output to be the same path\n\n"
      << "Batch options:\n"
      << "  --recursive             Include SVG files in nested input directories\n"
      << "  --fail-fast             Stop after the first failed file\n"
      << "  --no-overwrite          Skip output files that already exist\n"
      << "  --overwrite             Replace existing output files (default)\n"
      << "  --report <path>         Write a machine-readable JSON conversion report\n\n"
      << "Other:\n"
      << "  --                      Treat all following arguments as paths\n"
      << "  -h, --help              Show this help\n"
      << "  --version               Show the version\n";
}

std::string require_value(int argc, char** argv, int& index, const std::string& option) {
  if (index + 1 >= argc) throw std::runtime_error(option + " requires a value");
  return argv[++index];
}

int parse_precision(const std::string& value) {
  std::size_t consumed = 0;
  int precision = 0;
  try {
    precision = std::stoi(value, &consumed);
  } catch (...) {
    throw std::runtime_error("--precision requires an integer from 0 to 15");
  }
  if (consumed != value.size() || precision < 0 || precision > 15) {
    throw std::runtime_error("--precision requires an integer from 0 to 15");
  }
  return precision;
}

ConversionPolicy parse_conversion_policy(const std::string& value) {
  if (value == "preserve-appearance") return ConversionPolicy::PreserveAppearance;
  if (value == "filled-paths") return ConversionPolicy::FilledPaths;
  throw std::runtime_error(
      "--conversion-policy requires preserve-appearance or filled-paths");
}

void print_diagnostics(const std::vector<Diagnostic>& diagnostics,
                       const std::optional<fs::path>& input = std::nullopt) {
  for (const Diagnostic& diagnostic : diagnostics) {
    std::cerr << svg_squisher::diagnostic_severity_name(diagnostic.severity)
              << "[" << diagnostic.code << "]";
    if (input) std::cerr << " " << *input;
    if (!diagnostic.element.empty()) std::cerr << " " << diagnostic.element;
    std::cerr << ": " << diagnostic.message << "\n";
  }
}

void print_file_failure(const FileConversionResult& file) {
  std::cerr << (file.skipped ? "Skipped " : "Failed ") << file.input_path;
  if (!file.error.empty()) std::cerr << ": " << file.error;
  std::cerr << "\n";
}

fs::path normalized_absolute_path(const fs::path& path) {
  std::error_code error;
  const fs::path absolute = fs::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

bool paths_refer_to_same_location(const fs::path& left, const fs::path& right) {
  std::error_code error;
  const bool equivalent = fs::equivalent(left, right, error);
  if (!error && equivalent) return true;
  return normalized_absolute_path(left) == normalized_absolute_path(right);
}

bool path_is_at_or_within(const fs::path& candidate, const fs::path& directory) {
  std::error_code candidate_error;
  std::error_code directory_error;
  fs::path normalized_candidate = fs::weakly_canonical(candidate, candidate_error);
  fs::path normalized_directory = fs::weakly_canonical(directory, directory_error);
  if (candidate_error) normalized_candidate = normalized_absolute_path(candidate);
  if (directory_error) normalized_directory = normalized_absolute_path(directory);

  auto candidate_component = normalized_candidate.begin();
  for (auto directory_component = normalized_directory.begin();
       directory_component != normalized_directory.end();
       ++directory_component, ++candidate_component) {
    if (candidate_component == normalized_candidate.end() ||
        *candidate_component != *directory_component) {
      return false;
    }
  }
  return true;
}

void reject_report_collision(const fs::path& report_path,
                             const FileConversionResult& file) {
  if (paths_refer_to_same_location(report_path, file.input_path) ||
      paths_refer_to_same_location(report_path, file.output_path)) {
    throw std::runtime_error("Report path must not replace an input or converted SVG file");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      print_usage(std::cerr);
      return 1;
    }

    Options options;
    std::optional<fs::path> report_path;
    std::vector<std::string> positional;
    bool allow_in_place = false;
    bool parse_options = true;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (parse_options && arg == "--") {
        parse_options = false;
      } else if (parse_options && arg == "--fill") {
        options.fill_override = require_value(argc, argv, i, arg);
      } else if (parse_options && arg == "--font") {
        options.font_path = require_value(argc, argv, i, arg);
      } else if (parse_options && arg == "--conversion-policy") {
        options.conversion_policy =
            parse_conversion_policy(require_value(argc, argv, i, arg));
      } else if (parse_options && arg == "--precision") {
        options.precision = parse_precision(require_value(argc, argv, i, arg));
      } else if (parse_options && arg == "--report") {
        report_path = require_value(argc, argv, i, arg);
      } else if (parse_options && arg == "--remove-background") {
        options.remove_background = true;
      } else if (parse_options && arg == "--strict") {
        options.strict = true;
      } else if (parse_options && arg == "--recursive") {
        options.recursive = true;
      } else if (parse_options && arg == "--fail-fast") {
        options.continue_on_error = false;
      } else if (parse_options && arg == "--no-overwrite") {
        options.overwrite = false;
      } else if (parse_options && arg == "--overwrite") {
        options.overwrite = true;
      } else if (parse_options && arg == "--in-place") {
        allow_in_place = true;
        options.allow_in_place = true;
      } else if (parse_options && (arg == "--help" || arg == "-h")) {
        print_usage(std::cout);
        return 0;
      } else if (parse_options && arg == "--version") {
        std::cout << "SVG Squisher " << SVG_SQUISHER_VERSION << "\n";
        return 0;
      } else if (parse_options && !arg.empty() && arg.front() == '-') {
        throw std::runtime_error("Unknown option: " + arg);
      } else {
        positional.push_back(arg);
      }
    }

    if (positional.size() != 2) {
      throw std::runtime_error("Expected exactly one input path and one output path");
    }

    const fs::path input = positional[0];
    const fs::path output = positional[1];
    const bool input_is_directory = fs::is_directory(input);
    if (!allow_in_place && paths_refer_to_same_location(input, output)) {
      throw std::runtime_error(
          "Input and output resolve to the same path; pass --in-place to allow replacement");
    }
    if (report_path && (paths_refer_to_same_location(*report_path, input) ||
                        paths_refer_to_same_location(*report_path, output))) {
      throw std::runtime_error("Report path must be distinct from the input and output paths");
    }
    if (report_path && input_is_directory &&
        (path_is_at_or_within(*report_path, input) ||
         path_is_at_or_within(*report_path, output))) {
      throw std::runtime_error(
          "For a directory conversion, the report path must be outside the input and output trees");
    }
    SvgSquisher squisher;

    if (input_is_directory) {
      const BatchResult result = squisher.squish_directory_with_result(input, output, options);
      for (const FileConversionResult& file : result.files) {
        print_diagnostics(file.diagnostics, file.input_path);
        if (!file.success) print_file_failure(file);
      }
      if (report_path) {
        for (const FileConversionResult& file : result.files) {
          reject_report_collision(*report_path, file);
        }
        svg_squisher::write_json_report(*report_path, result, options);
      }

      std::cout << "Converted " << result.converted << ", failed " << result.failed
                << ", skipped " << result.skipped << ", warnings " << result.warnings
                << ". Output: " << output << "\n";
      return (result.failed == 0 && result.skipped == 0) ? 0 : 1;
    }

    const FileConversionResult file = squisher.squish_file_with_result(input, output, options);
    print_diagnostics(file.diagnostics, input);
    const BatchResult report = svg_squisher::batch_result_from_file(file);
    if (report_path) {
      reject_report_collision(*report_path, file);
      svg_squisher::write_json_report(*report_path, report, options);
    }
    if (!file.success) {
      print_file_failure(file);
      return 1;
    }

    std::cout << "Converted " << input << " -> " << output
              << " (" << file.stats.input_bytes << " -> " << file.stats.output_bytes
              << " bytes, " << file.stats.output_paths << " paths)\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    return 1;
  }
}
