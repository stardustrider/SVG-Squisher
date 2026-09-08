#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "svg_diagnostics.h"
#include "svg_output.h"
#include "svg_report.h"
#include "svg_squisher.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool contains_diagnostic(const svg_squisher::ConversionResult& result,
                         const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool diagnostic_message_contains(const svg_squisher::ConversionResult& result,
                                 const std::string& code,
                                 const std::string& text) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code && diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool is_sha256(const std::optional<std::string>& value) {
  return value && value->size() == 64 &&
         std::all_of(value->begin(), value->end(), [](const char digit) {
           return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
         });
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  std::size_t cursor = 0;
  while ((cursor = text.find(needle, cursor)) != std::string::npos) {
    ++count;
    cursor += needle.size();
  }
  return count;
}

void write_text(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string root(const std::string& body) {
  return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" "
         "viewBox=\"0 0 100 100\">" + body + "</svg>";
}

std::string nested_svg_at_depth(std::size_t depth) {
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">";
  const std::size_t group_count = depth > 1 ? depth - 2 : 0;
  for (std::size_t i = 0; i < group_count; ++i) svg += "<g>";
  svg += "<rect width=\"1\" height=\"1\"/>";
  for (std::size_t i = 0; i < group_count; ++i) svg += "</g>";
  svg += "</svg>";
  return svg;
}

std::string expanded_depth_attack(std::size_t reference_count,
                                  std::size_t nested_group_count) {
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"><defs>";
  for (std::size_t reference = 0; reference < reference_count; ++reference) {
    svg += "<g id=\"chain-" + std::to_string(reference) + "\">";
    for (std::size_t group = 0; group < nested_group_count; ++group) svg += "<g>";
    if (reference + 1 < reference_count) {
      svg += "<use href=\"#chain-" + std::to_string(reference + 1) + "\"/>";
    } else {
      svg += "<rect width=\"1\" height=\"1\"/>";
    }
    for (std::size_t group = 0; group < nested_group_count; ++group) svg += "</g>";
    svg += "</g>";
  }
  svg += "</defs><use href=\"#chain-0\"/></svg>";
  return svg;
}

std::string binary_use_tree(std::size_t depth) {
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"><defs>";
  for (std::size_t level = 0; level < depth; ++level) {
    svg += "<g id=\"binary-" + std::to_string(level) + "\">";
    if (level + 1 < depth) {
      const std::string target = "#binary-" + std::to_string(level + 1);
      svg += "<use href=\"" + target + "\"/><use href=\"" + target + "\"/>";
    } else {
      svg += "<rect width=\"1\" height=\"1\"/>";
    }
    svg += "</g>";
  }
  svg += "</defs><use href=\"#binary-0\"/></svg>";
  return svg;
}

std::string output_path_budget_attack(std::size_t painted_elements) {
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">";
  for (std::size_t element = 0; element < painted_elements; ++element) {
    svg += "<circle cx=\"5\" cy=\"5\" r=\"4\" fill=\"red\" stroke=\"black\" "
           "stroke-width=\"1\"/>";
  }
  svg += "</svg>";
  return svg;
}

std::string fill_only_elements(std::size_t painted_elements, bool hidden) {
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">";
  if (hidden) svg += "<g display=\"none\">";
  for (std::size_t element = 0; element < painted_elements; ++element) {
    svg += "<rect width=\"1\" height=\"1\" fill=\"red\"/>";
  }
  if (hidden) svg += "</g>";
  svg += "</svg>";
  return svg;
}

std::string zero_radius_circles_with_valid_rect(std::size_t element_count) {
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">";
  for (std::size_t element = 0; element < element_count; ++element) {
    svg += "<circle cx=\"5\" cy=\"5\" r=\"0\" fill=\"red\" "
           "stroke=\"black\" stroke-width=\"1\"/>";
  }
  svg += "<rect width=\"1\" height=\"1\" fill=\"red\"/>";
  svg += "</svg>";
  return svg;
}

std::string repeated_use_instances_before_target(std::size_t instance_count) {
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\">";
  for (std::size_t instance = 0; instance < instance_count; ++instance) {
    svg += "<use href=\"#late-target\"/>";
  }
  svg +=
      "<defs><path id=\"late-target\" d=\"M0 0L1 1\" fill=\"none\" "
      "stroke=\"black\" stroke-dasharray=\"1 1\"/></defs></svg>";
  return svg;
}

void test_expanded_traversal_bounds() {
  svg_squisher::SvgSquisher squisher;

  constexpr std::size_t repeated_use_count = 3000;
  const auto repeated_uses = squisher.convert_string(
      repeated_use_instances_before_target(repeated_use_count));
  expect(repeated_uses.success &&
             repeated_uses.stats.output_paths == repeated_use_count &&
             !contains_diagnostic(repeated_uses, "expanded-node-limit") &&
             !contains_diagnostic(repeated_uses, "output-path-limit"),
         "many use instances resolve a late target within traversal budgets");

  const std::string deep_chain = expanded_depth_attack(64, 251);
  const auto compatible_chain = squisher.convert_string(deep_chain);
  expect(compatible_chain.success &&
             contains_diagnostic(compatible_chain, "expanded-traversal-depth-limit") &&
             compatible_chain.stats.output_paths <= svg_squisher::kMaxOutputPathCount,
         "combined DOM and use expansion depth is diagnosed and truncated safely");

  svg_squisher::Options strict;
  strict.strict = true;
  const auto strict_chain = squisher.convert_string(deep_chain, strict);
  expect(!strict_chain.success && strict_chain.svg.empty() &&
             strict_chain.error.find("expanded-traversal-depth-limit") != std::string::npos,
         "strict mode rejects combined expanded traversal depth before conversion");

  const std::string binary_tree = binary_use_tree(20);
  const auto compatible_tree = squisher.convert_string(binary_tree);
  expect(compatible_tree.success &&
             (contains_diagnostic(compatible_tree, "expanded-node-limit") ||
              contains_diagnostic(compatible_tree, "output-path-limit")) &&
             compatible_tree.stats.output_paths <= svg_squisher::kMaxOutputPathCount,
         "acyclic exponential use expansion stays within node and output budgets");
  const auto strict_tree = squisher.convert_string(binary_tree, strict);
  expect(!strict_tree.success && strict_tree.svg.empty() &&
             (strict_tree.error.find("expanded-node-limit") != std::string::npos ||
              strict_tree.error.find("output-path-limit") != std::string::npos),
         "strict mode rejects exponential use expansion during preflight");

  const std::string excessive_output =
      output_path_budget_attack(svg_squisher::kMaxOutputPathCount / 2 + 1);
  const auto compatible_output = squisher.convert_string(excessive_output);
  expect(compatible_output.success &&
             contains_diagnostic(compatible_output, "output-path-limit") &&
             compatible_output.stats.output_paths == svg_squisher::kMaxOutputPathCount,
         "compatible mode caps the total number of emitted path entries");
  const auto strict_output = squisher.convert_string(excessive_output, strict);
  expect(!strict_output.success && strict_output.svg.empty() &&
             strict_output.error.find("output-path-limit") != std::string::npos,
         "strict mode rejects an exact runtime output-path budget overflow");

  constexpr std::size_t fill_only_count = svg_squisher::kMaxOutputPathCount / 2 + 1;
  const auto strict_fill_only =
      squisher.convert_string(fill_only_elements(fill_only_count, false), strict);
  expect(strict_fill_only.success &&
             strict_fill_only.stats.output_paths == fill_only_count &&
             !contains_diagnostic(strict_fill_only, "output-path-limit"),
         "strict output budgeting counts one emitted path for fill-only geometry");

  const auto strict_hidden =
      squisher.convert_string(fill_only_elements(fill_only_count, true), strict);
  expect(strict_hidden.success && strict_hidden.stats.output_paths == 0 &&
             !contains_diagnostic(strict_hidden, "output-path-limit"),
         "strict output budgeting ignores geometry suppressed by display none");

  const auto strict_degenerate = squisher.convert_string(
      zero_radius_circles_with_valid_rect(
          svg_squisher::kMaxOutputPathCount / 2 + 1), strict);
  expect(strict_degenerate.success && strict_degenerate.stats.output_paths == 1 &&
             !contains_diagnostic(strict_degenerate, "output-path-limit"),
         "strict output budgeting ignores zero-radius geometry that emits no paths");

  const std::string overlapping_use = root(
      "<defs><g id=\"overlap\"><rect width=\"8\" height=\"8\"/>"
      "<rect x=\"2\" y=\"2\" width=\"8\" height=\"8\"/></g></defs>"
      "<use href=\"#overlap\" opacity=\".5\"/>");
  const auto compatible_opacity = squisher.convert_string(overlapping_use);
  expect(compatible_opacity.success &&
             contains_diagnostic(compatible_opacity, "use-opacity-flattened"),
         "use opacity over multiple referenced elements is diagnosed");
  const auto strict_opacity = squisher.convert_string(overlapping_use, strict);
  expect(!strict_opacity.success &&
             strict_opacity.error.find("use-opacity-flattened") != std::string::npos,
         "strict mode rejects distributed opacity across a use instance");
}

void test_symbol_viewport_contract() {
  svg_squisher::SvgSquisher squisher;
  svg_squisher::Options strict;
  strict.strict = true;

  const std::string symbol_body =
    "<defs><symbol id=\"s\" viewBox=\"0 0 20 10\" overflow=\"visible\">"
    "<rect width=\"20\" height=\"10\"/></symbol></defs>";
  const auto automatic_root_size = squisher.convert_string(
    root(symbol_body + "<use href=\"#s\" x=\"10\" y=\"15\"/>"), strict);
  const auto explicit_root_size = squisher.convert_string(
    root(symbol_body +
         "<use href=\"#s\" x=\"10\" y=\"15\" width=\"100\" height=\"100\"/>"),
    strict);
  expect(automatic_root_size.success && explicit_root_size.success &&
             automatic_root_size.svg == explicit_root_size.svg,
         "automatic use dimensions resolve against the root user-space viewport");

  const auto explicit_auto_size = squisher.convert_string(root(
    "<defs><symbol id=\"s\" width=\"auto\" height=\"auto\" "
    "viewBox=\"0 0 20 10\" overflow=\"visible\">"
    "<rect width=\"20\" height=\"10\"/></symbol></defs>"
    "<use href=\"#s\" x=\"10\" y=\"15\" width=\"auto\" height=\"auto\"/>"),
    strict);
  expect(explicit_auto_size.success && explicit_auto_size.diagnostics.empty() &&
             explicit_auto_size.svg == automatic_root_size.svg,
         "explicit auto use and symbol dimensions are equivalent to omission");

  const std::string invalid_root_viewbox_prefix =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" height=\"60\" "
    "viewBox=\"0 0 -10 100\"><defs><symbol id=\"s\" viewBox=\"0 0 8 6\" "
    "overflow=\"visible\"><rect width=\"8\" height=\"6\"/></symbol></defs>";
  const auto invalid_root_auto = squisher.convert_string(
    invalid_root_viewbox_prefix + "<use href=\"#s\"/></svg>");
  const auto invalid_root_explicit = squisher.convert_string(
    invalid_root_viewbox_prefix +
      "<use href=\"#s\" width=\"80\" height=\"60\"/></svg>");
  expect(invalid_root_auto.success && invalid_root_explicit.success &&
             invalid_root_auto.stats.output_paths == 1 &&
             invalid_root_auto.svg == invalid_root_explicit.svg,
         "an invalid negative root viewBox falls back to finite root dimensions for automatic symbol sizing");

  const std::string sized_symbol_body =
    "<defs><symbol id=\"s\" width=\"40\" height=\"30\" "
    "viewBox=\"0 0 20 10\" overflow=\"visible\">"
    "<rect width=\"20\" height=\"10\"/></symbol></defs>";
  const auto inherited_symbol_size = squisher.convert_string(
    root(sized_symbol_body + "<use href=\"#s\" width=\"60\"/>"), strict);
  const auto explicit_symbol_size = squisher.convert_string(
    root(sized_symbol_body +
         "<use href=\"#s\" width=\"60\" height=\"30\"/>"),
    strict);
  expect(inherited_symbol_size.success && explicit_symbol_size.success &&
             inherited_symbol_size.svg == explicit_symbol_size.svg,
         "use dimensions override explicit symbol dimensions one axis at a time");

  const auto display_on_definition = squisher.convert_string(root(
    "<defs><symbol id=\"displayed\" viewBox=\"0 0 10 10\" display=\"none\" "
    "overflow=\"visible\"><rect width=\"10\" height=\"10\"/></symbol></defs>"
    "<use href=\"#displayed\" width=\"10\" height=\"10\"/>"), strict);
  expect(display_on_definition.success &&
             display_on_definition.stats.output_paths == 1,
         "a generated symbol instance uses display inline regardless of display on its definition");

  const auto displayed_symbol_layers = squisher.convert_string(root(
    "<defs><symbol id=\"layers\" viewBox=\"0 0 10 10\" display=\"none\" "
    "overflow=\"visible\"><rect width=\"8\" height=\"8\"/>"
    "<rect x=\"2\" y=\"2\" width=\"8\" height=\"8\"/></symbol></defs>"
    "<use href=\"#layers\" width=\"10\" height=\"10\" opacity=\".5\"/>"));
  expect(displayed_symbol_layers.success &&
             contains_diagnostic(displayed_symbol_layers, "use-opacity-flattened"),
         "symbol instance opacity diagnostics also honor the generated display-inline value");

  const std::string contained_symbol = root(
    "<defs><symbol id=\"inside\" viewBox=\"0 0 20 10\">"
    "<rect width=\"20\" height=\"10\"/></symbol></defs>"
    "<use href=\"#inside\" width=\"60\" height=\"70\"/>");
  const auto contained = squisher.convert_string(contained_symbol, strict);
  expect(contained.success &&
             !contains_diagnostic(contained, "unsupported-symbol-viewport-clipping"),
         "strict conversion accepts default symbol clipping when generated geometry is proven contained");

  const std::string clipped_symbol = root(
    "<defs><symbol id=\"clipped\" viewBox=\"0 0 20 10\" "
    "overflow=\"hidden\" preserveAspectRatio=\"xMaxYMin slice\">"
    "<rect width=\"20\" height=\"10\"/><circle cx=\"10\" cy=\"5\" r=\"7\"/>"
    "</symbol></defs><use href=\"#clipped\" x=\"10\" y=\"15\" "
    "width=\"60\" height=\"70\"/>");
  const auto compatible_clip = squisher.convert_string(clipped_symbol);
  expect(compatible_clip.success &&
             contains_diagnostic(
               compatible_clip, "unsupported-symbol-viewport-clipping") &&
             diagnostic_message_contains(
               compatible_clip,
               "unsupported-symbol-viewport-clipping",
               "without the required viewport clip") &&
             compatible_clip.stats.output_paths == 2,
         "compatible conversion identifies unclipped symbol overflow and states its fallback");
  const auto strict_clip = squisher.convert_string(clipped_symbol, strict);
  expect(!strict_clip.success && strict_clip.svg.empty() &&
             strict_clip.error.find("unsupported-symbol-viewport-clipping") !=
               std::string::npos,
         "strict conversion rejects symbol content that requires viewport clipping");

  const std::string transformed_clip = root(
    "<defs><symbol id=\"moved\" width=\"10\" height=\"10\" "
    "transform=\"translate(-100 0)\"><rect x=\"100\" width=\"10\" "
    "height=\"10\"/></symbol></defs><use href=\"#moved\" width=\"10\" "
    "height=\"10\"/>");
  const auto transformed_clip_compatible =
    squisher.convert_string(transformed_clip);
  const auto transformed_clip_strict =
    squisher.convert_string(transformed_clip, strict);
  expect(transformed_clip_compatible.success &&
             contains_diagnostic(
               transformed_clip_compatible,
               "unsupported-symbol-viewport-clipping") &&
             !transformed_clip_strict.success,
         "symbol transforms participate in conservative viewport containment checks");

  std::string visible_overflow = clipped_symbol;
  const std::size_t hidden = visible_overflow.find("overflow=\"hidden\"");
  visible_overflow.replace(hidden, std::string("overflow=\"hidden\"").size(),
                           "overflow=\"visible\"");
  const auto explicit_visible = squisher.convert_string(visible_overflow, strict);
  expect(explicit_visible.success &&
             !contains_diagnostic(
               explicit_visible, "unsupported-symbol-viewport-clipping"),
         "explicit visible symbol overflow opts into the converter's unclipped output");

  const std::string styled_overflow = root(
    "<defs><symbol id=\"styled\" viewBox=\"0 0 10 10\" "
    "style=\"overflow:visible\"><rect width=\"12\" height=\"10\"/></symbol></defs>"
    "<use href=\"#styled\" width=\"10\" height=\"10\"/>");
  const auto diagnosed_overflow_style = squisher.convert_string(styled_overflow);
  const auto rejected_overflow_style = squisher.convert_string(styled_overflow, strict);
  expect(diagnosed_overflow_style.success &&
             contains_diagnostic(
               diagnosed_overflow_style, "unsupported-css-property") &&
             !rejected_overflow_style.success &&
             rejected_overflow_style.error.find("unsupported-css-property") !=
               std::string::npos,
         "CSS overflow on a symbol is diagnosed while the supported visible attribute stays explicit");

  const std::string positioned_symbol = root(
    "<defs><symbol id=\"positioned\" x=\"2\" y=\"3\" width=\"10\" "
    "height=\"10\" overflow=\"visible\"><rect width=\"10\" "
    "height=\"10\"/></symbol></defs><use href=\"#positioned\"/>");
  const auto positioned_compatible = squisher.convert_string(positioned_symbol);
  const auto positioned_strict = squisher.convert_string(positioned_symbol, strict);
  expect(positioned_compatible.success &&
             diagnostic_message_contains(
               positioned_compatible,
               "unsupported-symbol-position",
               "applies only x/y from this use") &&
             !positioned_strict.success &&
             positioned_strict.error.find("unsupported-symbol-position") !=
               std::string::npos,
         "referenced symbol x/y has an explicit compatible fallback and strict rejection");

  const std::string unprovable_stroke = root(
    "<defs><symbol id=\"live\" viewBox=\"0 0 20 10\">"
    "<path d=\"M2 5H18\" fill=\"none\" stroke=\"black\" "
    "stroke-dasharray=\"1 1\"/></symbol></defs>"
    "<use href=\"#live\" width=\"20\" height=\"10\"/>");
  const auto unprovable = squisher.convert_string(unprovable_stroke);
  expect(unprovable.success &&
             diagnostic_message_contains(
               unprovable,
               "unsupported-symbol-viewport-clipping",
               "could not be proven"),
         "live-stroked symbol geometry is diagnosed when viewport containment cannot be proven");

  const std::string nested_automatic = root(
    "<defs>"
    "<symbol id=\"inner\" viewBox=\"0 0 10 10\" overflow=\"visible\">"
    "<rect width=\"10\" height=\"10\"/></symbol>"
    "<symbol id=\"outer\" viewBox=\"0 0 20 20\" overflow=\"visible\">"
    "<use href=\"#inner\"/></symbol>"
    "</defs><use href=\"#outer\" width=\"20\" height=\"20\"/>");
  const auto nested = squisher.convert_string(nested_automatic);
  expect(nested.success &&
             diagnostic_message_contains(
               nested,
               "unsupported-nested-symbol-viewport",
               "resolves that automatic dimension against the root SVG viewport"),
         "nested automatic symbol dimensions receive a deterministic root-viewport diagnostic");

  const std::string indirectly_nested_automatic = root(
    "<defs>"
    "<symbol id=\"inner-indirect\" viewBox=\"0 0 10 10\" overflow=\"visible\">"
    "<rect width=\"10\" height=\"10\"/></symbol>"
    "<g id=\"bridge\"><use href=\"#inner-indirect\"/></g>"
    "<symbol id=\"outer-indirect\" viewBox=\"0 0 20 20\" overflow=\"visible\">"
    "<use href=\"#bridge\"/></symbol>"
    "</defs><use href=\"#outer-indirect\" width=\"20\" height=\"20\"/>");
  const auto indirectly_nested =
    squisher.convert_string(indirectly_nested_automatic);
  expect(indirectly_nested.success &&
             contains_diagnostic(
               indirectly_nested, "unsupported-nested-symbol-viewport"),
         "active symbol-instance context diagnoses nested auto sizing through a referenced group");
}

void test_exclusive_atomic_file_writes() {
  const fs::path temporary = fs::temp_directory_path() / "svg-squisher-output-safety-tests";
  std::error_code ignored;
  fs::remove_all(temporary, ignored);
  fs::create_directories(temporary);

#if !defined(_WIN32)
  const fs::path output = temporary / "symlink-result.svg";
  const fs::path dangling_target = temporary / "must-not-be-created.svg";
  const fs::path stale_temporary =
      temporary / (".symlink-result.svg.svg-squisher-" +
                   std::to_string(static_cast<unsigned long long>(::getpid())) +
                   "-0.tmp");
  std::error_code symlink_error;
  fs::create_symlink(dangling_target, stale_temporary, symlink_error);
  expect(!symlink_error, "POSIX output safety fixture creates a dangling temporary symlink");
  if (!symlink_error) {
    bool write_succeeded = false;
    try {
      svg_squisher::write_file(output, "exclusive output");
      write_succeeded = true;
    } catch (const std::exception& error) {
      std::cerr << "FAIL: exclusive write beside dangling symlink threw: "
                << error.what() << '\n';
      ++failures;
    }
    expect(write_succeeded && fs::is_regular_file(output) &&
               svg_squisher::read_file(output) == "exclusive output",
           "exclusive creation skips a stale dangling temporary symlink");
    expect(!fs::exists(dangling_target),
           "exclusive creation never follows a dangling temporary symlink");
    expect(fs::is_symlink(stale_temporary) &&
               fs::read_symlink(stale_temporary) == dangling_target,
           "exclusive creation leaves the colliding symlink untouched");
  }
#endif

  constexpr std::size_t writer_count = 8;
  const fs::path concurrent_output = temporary / "concurrent.svg";
  std::vector<std::string> payloads;
  payloads.reserve(writer_count);
  for (std::size_t index = 0; index < writer_count; ++index) {
    payloads.push_back("writer-" + std::to_string(index) + "\n" +
                       std::string(256 * 1024, static_cast<char>('A' + index)));
  }

  std::atomic<bool> start{false};
  std::vector<std::string> errors(writer_count);
  std::vector<std::thread> writers;
  writers.reserve(writer_count);
  for (std::size_t index = 0; index < writer_count; ++index) {
    writers.emplace_back([&, index] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      try {
        svg_squisher::write_file(concurrent_output, payloads[index]);
      } catch (const std::exception& error) {
        errors[index] = error.what();
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread& writer : writers) writer.join();

  for (std::size_t index = 0; index < writer_count; ++index) {
    expect(errors[index].empty(),
           "concurrent writer " + std::to_string(index) + " succeeds: " + errors[index]);
  }
  if (fs::is_regular_file(concurrent_output)) {
    const std::string final_payload = svg_squisher::read_file(concurrent_output);
    expect(std::find(payloads.begin(), payloads.end(), final_payload) != payloads.end(),
           "concurrent atomic replacement leaves one complete writer payload");
  } else {
    expect(false, "concurrent atomic replacement creates the destination file");
  }

  bool leaked_temporary = false;
  for (const fs::directory_entry& entry : fs::directory_iterator(temporary)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(".concurrent.svg.svg-squisher-", 0) == 0) {
      leaked_temporary = true;
    }
  }
  expect(!leaked_temporary, "concurrent atomic replacement leaves no temporary files");

  fs::remove_all(temporary, ignored);
}

void test_conversion_contract() {
  svg_squisher::SvgSquisher squisher;
  const auto basic = squisher.convert_string(root("<rect width=\"20\" height=\"10\"/>"));
  expect(basic.success, "basic rectangle conversion succeeds");
  expect(basic.stats.input_elements == 2, "input element count includes root and rectangle");
  expect(basic.stats.output_paths == 1, "basic rectangle produces one path entry");
  expect(basic.stats.output_path_commands > 0,
         "conversion reports generated path-command complexity");
  expect(basic.stats.output_bytes == basic.svg.size(), "reported output byte count is exact");
  expect(basic.svg.find("<path") != std::string::npos, "basic output contains a path");
  const auto repeated = squisher.convert_string(root("<rect width=\"20\" height=\"10\"/>"));
  expect(repeated.success && repeated.svg == basic.svg,
         "identical input and options produce deterministic output");
  const auto idempotent = squisher.convert_string(basic.svg);
  expect(idempotent.success && idempotent.svg == basic.svg,
         "converting generated path output is idempotent");
  expect(basic.svg.find("nan") == std::string::npos && basic.svg.find("inf") == std::string::npos,
         "generated geometry contains only finite coordinate tokens");

  const auto text = squisher.convert_string(root("<text x=\"5\" y=\"40\">Office</text>"));
  expect(!text.fonts_used.empty() || contains_diagnostic(text, "missing-font"),
         "text conversion reports the actual font or an actionable missing-font warning");
  if (!text.fonts_used.empty()) {
    expect(contains_diagnostic(text, "implicit-font-selection"),
           "implicit system font selection is identified as non-reproducible");
    expect(text.font_identities.size() == text.fonts_used.size() &&
               text.font_identities.front().path == text.fonts_used.front() &&
               text.font_identities.front().bytes.has_value() &&
               *text.font_identities.front().bytes > 0 &&
               is_sha256(text.font_identities.front().sha256) &&
               text.font_identities.front().error.empty(),
           "text conversion reports a byte-level identity for the exact font path used");
  }

  const auto invalid = squisher.convert_string("<svg><path></svg>");
  expect(!invalid.success && invalid.error.find("Failed to parse") != std::string::npos,
         "invalid XML returns an actionable failure");

  const auto bounded =
      squisher.convert_string(nested_svg_at_depth(svg_squisher::kMaxSvgStructuralDepth));
  expect(bounded.success, "SVG nesting at the documented structural limit succeeds");
  const auto too_deep =
      squisher.convert_string(nested_svg_at_depth(svg_squisher::kMaxSvgStructuralDepth + 1));
  expect(!too_deep.success && too_deep.svg.empty() &&
             too_deep.error.find(std::to_string(svg_squisher::kMaxSvgStructuralDepth)) !=
                 std::string::npos,
         "SVG nesting above the structural limit fails cleanly before conversion");
  const auto adversarial_depth =
      squisher.convert_string(nested_svg_at_depth(svg_squisher::kMaxSvgStructuralDepth * 16));
  expect(!adversarial_depth.success && adversarial_depth.svg.empty(),
         "adversarial nesting fails without entering recursive conversion passes");

  const std::string clipped = root(
      "<defs><clipPath id=\"c\"><rect width=\"10\" height=\"10\"/></clipPath></defs>"
      "<rect width=\"20\" height=\"20\" clip-path=\"url(#c)\"/>");
  const auto compatible = squisher.convert_string(clipped);
  expect(compatible.success, "compatible profile emits best-effort output");
  expect(contains_diagnostic(compatible, "unsupported-clip-path"),
         "compatible profile reports unsupported clipping");

  svg_squisher::Options strict;
  strict.strict = true;
  const auto rejected = squisher.convert_string(clipped, strict);
  expect(!rejected.success && rejected.svg.empty(), "strict profile writes no degraded output");
  expect(rejected.error.find("unsupported-clip-path") != std::string::npos,
         "strict failure identifies its capability code");
}

void test_explicit_conversion_policies() {
  svg_squisher::SvgSquisher squisher;
  const std::string two_shapes = root(
      "<rect width=\"100\" height=\"100\" fill=\"black\" fill-opacity=\".5\"/>"
      "<rect x=\"40\" y=\"40\" width=\"20\" height=\"20\" fill=\"red\"/>");

  svg_squisher::Options recolor;
  recolor.fill_override = "white";
  const auto recolored = squisher.convert_string(two_shapes, recolor);
  expect(recolored.success && recolored.stats.output_paths == 2,
         "recolor preserves all geometry by default");
  expect(recolored.svg.find("fill-opacity=\".5\"") != std::string::npos ||
             recolored.svg.find("fill-opacity=\"0.5\"") != std::string::npos,
         "recolor preserves fill opacity");

  recolor.remove_background = true;
  const auto cleaned = squisher.convert_string(two_shapes, recolor);
  expect(cleaned.success && cleaned.stats.output_paths == 1,
         "background removal runs only when explicitly requested");

  const std::string transformed_gradient = root(
      "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"black\"/>"
      "<stop offset=\"1\" stop-color=\"gray\"/></linearGradient></defs>"
      "<path d=\"M0 0H100V100H0Z\" transform=\"scale(.1)\" fill=\"url(#g)\"/>"
      "<rect x=\"50\" y=\"50\" width=\"10\" height=\"10\" fill=\"red\"/>");
  svg_squisher::Options transformed_cleanup;
  transformed_cleanup.remove_background = true;
  const auto transformed_cleaned =
      squisher.convert_string(transformed_gradient, transformed_cleanup);
  expect(transformed_cleaned.success &&
             transformed_cleaned.stats.output_paths == 2 &&
             transformed_cleaned.svg.find("url(#g)") != std::string::npos,
         "background removal classifies retained geometry in output coordinates");

  svg_squisher::Options coarse;
  coarse.precision = 1;
  const auto rounded = squisher.convert_string(
      root("<circle cx=\"10.1234\" cy=\"10.5678\" r=\"3.9876\"/>"), coarse);
  expect(rounded.success && rounded.svg.find("10.1234") == std::string::npos,
         "coordinate precision is configurable");
}

void test_preserved_document_semantics() {
  svg_squisher::SvgSquisher squisher;

  const auto painted_element = squisher.convert_string(root(
      "<rect x=\"10\" y=\"10\" width=\"70\" height=\"70\" fill=\"red\" "
      "stroke=\"blue\" stroke-width=\"20\" opacity=\"0.5\"/>"));
  expect(painted_element.success &&
           painted_element.svg.find("<g opacity=\"0.5\">") != std::string::npos &&
           count_occurrences(painted_element.svg, "opacity=\"0.5\"") == 1 &&
           count_occurrences(painted_element.svg, "<path ") == 2,
         "one painted element applies opacity once around its fill and stroke paths");
  expect(!contains_diagnostic(painted_element, "group-opacity-flattened"),
         "painted-element compositing is distinct from unsupported ancestor group opacity");

  const auto live_fill_and_stroke = squisher.convert_string(root(
      "<path d=\"M10 10H90V90H10Z\" fill=\"red\" stroke=\"blue\" "
      "stroke-width=\"4\" stroke-dasharray=\"6 4\"/>"));
  expect(live_fill_and_stroke.success &&
           live_fill_and_stroke.stats.output_paths ==
             count_occurrences(live_fill_and_stroke.svg, "<path ") &&
           live_fill_and_stroke.stats.output_paths == 2,
         "output path count matches serialized fill and live-stroke path elements");
  expect(live_fill_and_stroke.stats.output_path_commands == 10,
         "output command count includes both serialized copies of shared path data");

  const std::string grouped = root(
      "<style>.faded{opacity:.5}</style><g class=\"faded\">"
      "<rect width=\"70\" height=\"70\" fill=\"red\"/>"
      "<rect x=\"30\" y=\"30\" width=\"70\" height=\"70\" fill=\"blue\"/>"
      "</g>");
  const auto compatible_group = squisher.convert_string(grouped);
  expect(compatible_group.success && contains_diagnostic(compatible_group, "group-opacity-flattened"),
         "CSS group opacity is diagnosed in compatible mode");
  const std::size_t first_opacity = compatible_group.svg.find("opacity=\"0.5\"");
  expect(first_opacity != std::string::npos &&
           compatible_group.svg.find("opacity=\"0.5\"", first_opacity + 1) != std::string::npos,
         "compatible mode applies ancestor opacity to each emitted descendant");

  svg_squisher::Options strict;
  strict.strict = true;
  const auto strict_group = squisher.convert_string(grouped, strict);
  expect(!strict_group.success && strict_group.error.find("group-opacity-flattened") != std::string::npos,
         "strict mode rejects group-compositing changes before output");

  const std::string single_layer_group = root(
      "<g opacity=\".5\"><rect width=\"70\" height=\"70\" fill=\"red\" "
      "stroke=\"blue\" stroke-width=\"4\"/></g>");
  const auto strict_single_layer = squisher.convert_string(single_layer_group, strict);
  expect(strict_single_layer.success &&
             !contains_diagnostic(strict_single_layer, "group-opacity-flattened"),
         "container opacity around one painted element remains supported in strict mode");

  const std::string linked_layers = root(
      "<a opacity=\".5\"><rect width=\"70\" height=\"70\"/>"
      "<rect x=\"30\" y=\"30\" width=\"70\" height=\"70\"/></a>");
  const auto compatible_link = squisher.convert_string(linked_layers);
  expect(compatible_link.success &&
             contains_diagnostic(compatible_link, "group-opacity-flattened"),
         "opacity on a link container with multiple painted layers is diagnosed");
  const auto strict_link = squisher.convert_string(linked_layers, strict);
  expect(!strict_link.success &&
             strict_link.error.find("group-opacity-flattened") != std::string::npos,
         "strict mode rejects distributed opacity on a link container");

  const std::string text_layers = root(
      "<text x=\"4\" y=\"40\" opacity=\".5\">first<tspan x=\"4\" "
      "y=\"60\">second</tspan></text>");
  const auto compatible_text_layers = squisher.convert_string(text_layers);
  expect(compatible_text_layers.success &&
             contains_diagnostic(compatible_text_layers, "group-opacity-flattened"),
         "opacity across text and tspan chunks is diagnosed");
  const auto strict_text_layers = squisher.convert_string(text_layers, strict);
  expect(!strict_text_layers.success &&
             strict_text_layers.error.find("group-opacity-flattened") != std::string::npos,
         "strict mode rejects distributed opacity across text chunks");

  const std::string nested_tspan_layers = root(
      "<text x=\"4\" y=\"40\"><tspan opacity=\".5\">first"
      "<tspan x=\"4\" y=\"60\">second</tspan></tspan></text>");
  const auto compatible_tspan_layers = squisher.convert_string(nested_tspan_layers);
  expect(compatible_tspan_layers.success &&
             contains_diagnostic(compatible_tspan_layers, "group-opacity-flattened"),
         "opacity across nested tspan chunks is diagnosed");
  const auto strict_tspan_layers = squisher.convert_string(nested_tspan_layers, strict);
  expect(!strict_tspan_layers.success &&
             strict_tspan_layers.error.find("group-opacity-flattened") != std::string::npos,
         "strict mode rejects distributed opacity across nested tspan chunks");

  const auto viewport = squisher.convert_string(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-10 -20 100 100\" "
      "id=\"mark\" role=\"img\" aria-label=\"Accessible mark\" "
      "aria-labelledby=\"name details\" aria-describedby=\"details\" aria-hidden=\"false\" "
      "focusable=\"false\" lang=\"en\" xml:lang=\"en-US\">"
      "<title id=\"name\" lang=\"en\" xml:lang=\"en-US\">Mark</title>"
      "<desc id=\"details\" lang=\"en\">Description</desc>"
      "<rect x=\"-10\" y=\"-20\" width=\"10\" height=\"10\"/>"
      "</svg>");
  expect(viewport.success && viewport.svg.find("viewBox=\"-10 -20 100 100\"") != std::string::npos,
         "non-zero viewBox origins are preserved instead of rewriting paint coordinates");
  expect(viewport.svg.find("id=\"mark\"") != std::string::npos &&
             viewport.svg.find("role=\"img\"") != std::string::npos &&
             viewport.svg.find("aria-label=\"Accessible mark\"") != std::string::npos &&
             viewport.svg.find("aria-labelledby=\"name details\"") != std::string::npos &&
             viewport.svg.find("aria-describedby=\"details\"") != std::string::npos &&
             viewport.svg.find("aria-hidden=\"false\"") != std::string::npos &&
             viewport.svg.find("focusable=\"false\"") != std::string::npos &&
             viewport.svg.find("lang=\"en\"") != std::string::npos &&
             viewport.svg.find("xml:lang=\"en-US\"") != std::string::npos &&
             viewport.svg.find("<title id=\"name\" lang=\"en\" xml:lang=\"en-US\">Mark</title>") !=
                 std::string::npos &&
             viewport.svg.find("<desc id=\"details\" lang=\"en\">Description</desc>") !=
                 std::string::npos,
         "the documented accessible root and direct metadata attributes are preserved");

  const auto definitions = squisher.convert_string(root(
      "<defs><linearGradient id=\"unused\"/><linearGradient id=\"base\"/>"
      "<linearGradient id=\"derived\" href=\"#base\"/></defs>"
      "<rect width=\"50\" height=\"50\" fill=\"url(#derived)\"/>"));
  expect(definitions.success && definitions.svg.find("id=\"derived\"") != std::string::npos &&
           definitions.svg.find("id=\"base\"") != std::string::npos &&
           definitions.svg.find("id=\"unused\"") == std::string::npos,
         "only referenced definitions and their dependencies are retained");

  const auto sanitized_definition = squisher.convert_string(root(
      "<defs><pattern id=\"safe-pattern\" width=\"10\" height=\"10\" "
      "patternUnits=\"userSpaceOnUse\" onload=\"javascript:alert(1)\">"
      "<style>.tile{fill:#00ff00}</style>"
      "<script>javascript:alert(2)</script>"
      "<rect class=\"tile\" width=\"10\" height=\"10\" onclick=\"javascript:alert(3)\" "
      "style=\"fill:#00ff00;filter:url(https://evil.invalid/filter.svg#f)\"/>"
      "<image href=\"https://evil.invalid/tile.png\"/>"
      "<foreignObject><div>unsupported</div></foreignObject>"
      "<animate attributeName=\"opacity\" values=\"0;1\"/>"
      "<use href=\"javascript:alert(4)\"/>"
      "</pattern></defs>"
      "<rect width=\"50\" height=\"50\" fill=\"url(#safe-pattern)\"/>"));
  expect(sanitized_definition.success &&
             sanitized_definition.svg.find("id=\"safe-pattern\"") != std::string::npos &&
             sanitized_definition.svg.find("fill=\"#00ff00\"") != std::string::npos,
         "supported definition styles are materialized before sanitization");
  expect(sanitized_definition.svg.find("<style") == std::string::npos &&
             sanitized_definition.svg.find("<script") == std::string::npos &&
             sanitized_definition.svg.find("<image") == std::string::npos &&
             sanitized_definition.svg.find("<foreignObject") == std::string::npos &&
             sanitized_definition.svg.find("<animate") == std::string::npos &&
             sanitized_definition.svg.find("onload") == std::string::npos &&
             sanitized_definition.svg.find("onclick") == std::string::npos &&
             sanitized_definition.svg.find("javascript:") == std::string::npos &&
             sanitized_definition.svg.find("evil.invalid") == std::string::npos,
         "retained definitions exclude active nodes, handlers, and external resources");

  const auto referenced_script_definition = squisher.convert_string(root(
      "<defs><script id=\"script-paint\">javascript:alert(1)</script></defs>"
      "<rect width=\"10\" height=\"10\" fill=\"url(#script-paint)\"/>"));
  expect(referenced_script_definition.success &&
             contains_diagnostic(referenced_script_definition, "unsupported-script") &&
             referenced_script_definition.svg.find("<script") == std::string::npos &&
             referenced_script_definition.svg.find("javascript:") == std::string::npos,
         "an unsafe directly referenced definition root is not serialized");

  const auto rotated_arc = squisher.convert_string(root(
      "<path d=\"M10 50A30 12 45 0 1 90 50Z\" transform=\"scale(2 1)\"/>"));
  expect(rotated_arc.success && rotated_arc.svg.find("transform=\"scale(2 1)\"") != std::string::npos,
         "non-uniform transforms on rotated arcs remain live for renderer-correct geometry");
}

void test_strict_capability_coverage() {
  svg_squisher::SvgSquisher squisher;
  svg_squisher::Options strict;
  strict.strict = true;

  const auto compact_points = squisher.convert_string(root(
      "<polyline points=\"0-10 20-30\" fill=\"none\" stroke=\"black\"/>"),
      strict);
  expect(compact_points.success &&
             !contains_diagnostic(compact_points, "invalid-numeric-value"),
         "strict mode accepts compact negative coordinates in points lists");

  const auto zero_viewbox = squisher.convert_string(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 0 10\">"
      "<rect width=\"10\" height=\"10\"/></svg>",
      strict);
  expect(zero_viewbox.success &&
             !contains_diagnostic(zero_viewbox, "invalid-numeric-value"),
         "strict mode accepts a zero viewBox dimension that disables rendering");

  const auto intentionally_empty = squisher.convert_string(root(
      "<defs><g id=\"empty-definition\"/></defs>"
      "<circle r=\"0\"/><ellipse rx=\"0\" ry=\"4\"/>"
      "<rect width=\"0\" height=\"10\"/><path d=\"\"/>"
      "<polyline points=\"\"/><polygon points=\"\"/>"
      "<use href=\"#empty-definition\"/>"), strict);
  expect(intentionally_empty.success && intentionally_empty.stats.output_paths == 0 &&
             !contains_diagnostic(intentionally_empty, "empty-output") &&
             !contains_diagnostic(intentionally_empty, "invalid-numeric-value"),
         "strict mode accepts valid intentionally empty geometry without empty-output");

  const auto dashed = squisher.convert_string(root(
      "<path d=\"M0 10H100\" fill=\"none\" stroke=\"black\" "
      "stroke-dasharray=\"4 4\" stroke-dashoffset=\"2\"/>"), strict);
  expect(!dashed.success && dashed.error.find("unsupported-paint-semantics") != std::string::npos,
         "strict mode rejects unrepresented dash offsets");

  const auto css = squisher.convert_string(root(
      "<style>rect{mix-blend-mode:multiply}</style><rect width=\"10\" height=\"10\"/>"), strict);
  expect(!css.success && css.error.find("unsupported-css-property") != std::string::npos,
         "strict mode rejects unsupported CSS declarations");

  const std::string inline_css_source =
      root("<rect width=\"10\" height=\"10\" style=\"fill:red;filter:url(#f)\"/>");
  const auto inline_css = squisher.convert_string(inline_css_source);
  expect(inline_css.success && contains_diagnostic(inline_css, "unsupported-css-property") &&
             diagnostic_message_contains(
                 inline_css, "unsupported-css-property", "Compatible conversion ignores"),
         "compatible mode diagnoses unsupported inline CSS declarations");
  const auto strict_inline_css = squisher.convert_string(inline_css_source, strict);
  expect(!strict_inline_css.success &&
             strict_inline_css.error.find("unsupported-css-property") != std::string::npos,
         "strict mode rejects unsupported inline CSS declarations");

  const auto unsupported_selector = squisher.convert_string(root(
      "<style>g > rect{fill:red}</style><g><rect width=\"10\" height=\"10\"/></g>"));
  expect(unsupported_selector.success &&
             diagnostic_message_contains(
                 unsupported_selector, "unsupported-css-selector", "g > rect") &&
             diagnostic_message_contains(
                 unsupported_selector, "unsupported-css-selector", "is ignored"),
         "unsupported selector diagnostics identify the selector and ignored rule");

  const auto text_path_fallback = squisher.convert_string(root(
      "<defs><path id=\"baseline\" d=\"M0 20H100\"/></defs>"
      "<text><textPath href=\"#baseline\">text</textPath></text>"));
  expect(text_path_fallback.success &&
             diagnostic_message_contains(
                 text_path_fallback, "unsupported-text-path", "normal-cursor text layout"),
         "textPath diagnostics state the compatible normal-cursor layout");

  const auto switch_fallback = squisher.convert_string(root(
      "<switch><rect width=\"10\" height=\"10\"/>"
      "<circle cx=\"20\" cy=\"20\" r=\"5\"/></switch>"));
  expect(switch_fallback.success &&
             diagnostic_message_contains(
                 switch_fallback, "unsupported-switch", "traverses all branches"),
         "switch diagnostics state that compatible conversion traverses every branch");

  const auto nested_viewport_fallback = squisher.convert_string(root(
      "<svg x=\"10\" y=\"20\" width=\"50\" height=\"50\" viewBox=\"0 0 10 10\" "
      "preserveAspectRatio=\"xMidYMid meet\" transform=\"translate(2 3)\">"
      "<rect width=\"10\" height=\"10\"/></svg>"));
  expect(nested_viewport_fallback.success &&
             diagnostic_message_contains(
                 nested_viewport_fallback,
                 "unsupported-nested-viewport",
                 "ignores x, y, width, height, viewBox, and preserveAspectRatio") &&
             diagnostic_message_contains(
                 nested_viewport_fallback,
                 "unsupported-nested-viewport",
                 "current user coordinate system with any transform still applied"),
         "nested viewport diagnostics state the deterministic compatible fallback");

  const std::string external_paint_source = root(
      "<rect width=\"10\" height=\"10\" "
      "fill=\"url(https://evil.invalid/paint.svg#paint)\"/>");
  const auto compatible_external_paint = squisher.convert_string(external_paint_source);
  expect(compatible_external_paint.success &&
             contains_diagnostic(compatible_external_paint, "unsupported-external-reference") &&
             diagnostic_message_contains(
                 compatible_external_paint,
                 "unsupported-external-reference",
                 "replaces this paint with none") &&
             compatible_external_paint.svg.find("evil.invalid") == std::string::npos,
         "compatible mode diagnoses and removes external presentation-attribute paints");
  const auto strict_external_paint = squisher.convert_string(external_paint_source, strict);
  expect(!strict_external_paint.success &&
             strict_external_paint.error.find("unsupported-external-reference") != std::string::npos,
         "strict mode rejects external presentation-attribute paints");

  const std::string escaped_external_paint_source = root(R"SVG(
    <rect width="10" height="10"
      fill="\75\72\6c(http://127.0.0.1:47931/leak.svg)"/>
  )SVG");
  const auto compatible_escaped_external =
    squisher.convert_string(escaped_external_paint_source);
  const auto strict_escaped_external =
    squisher.convert_string(escaped_external_paint_source, strict);
  expect(compatible_escaped_external.success &&
             contains_diagnostic(
               compatible_escaped_external, "unsupported-external-reference") &&
             compatible_escaped_external.svg.find("127.0.0.1") ==
               std::string::npos &&
             compatible_escaped_external.svg.find("fill=\"none\"") !=
               std::string::npos,
         "compatible output removes an external URL with an escaped CSS function name");
  expect(!strict_escaped_external.success &&
             strict_escaped_external.svg.empty() &&
             strict_escaped_external.error.find("unsupported-external-reference") !=
               std::string::npos,
         "strict mode rejects an external URL with an escaped CSS function name");

  const std::string escaped_local_paint_source = root(R"SVG(
    <defs><linearGradient id="safe-local"><stop offset="0" stop-color="red"/>
      <stop offset="1" stop-color="blue"/></linearGradient></defs>
    <rect width="10" height="10" fill="\75\72\6c(\23 safe-local)"/>
  )SVG");
  const auto escaped_local_paint =
    squisher.convert_string(escaped_local_paint_source, strict);
  expect(escaped_local_paint.success && escaped_local_paint.diagnostics.empty() &&
             escaped_local_paint.svg.find("id=\"safe-local\"") !=
               std::string::npos &&
             escaped_local_paint.svg.find("safe-local") != std::string::npos,
         "escaped local-fragment paint URLs remain supported and retain their definition");

  const std::string external_inline_paint_source = root(
      "<path d=\"M0 0H10\" style=\"fill:none;stroke:url('https://evil.invalid/stroke.svg#s')\"/>");
  const auto strict_external_inline =
      squisher.convert_string(external_inline_paint_source, strict);
  expect(!strict_external_inline.success &&
             strict_external_inline.error.find("unsupported-external-reference") !=
                 std::string::npos,
         "strict mode rejects external URLs in inline CSS paints");

  const std::string escaped_inline_paint_source = root(R"SVG(
    <path d="M0 0H10V10Z"
      style="fill:\55\52\4c ( 'https://evil.invalid/escaped.svg' )"/>
  )SVG");
  const auto compatible_escaped_inline =
    squisher.convert_string(escaped_inline_paint_source);
  const auto strict_escaped_inline =
    squisher.convert_string(escaped_inline_paint_source, strict);
  expect(compatible_escaped_inline.success &&
             contains_diagnostic(
               compatible_escaped_inline, "unsupported-external-reference") &&
             compatible_escaped_inline.svg.find("evil.invalid") ==
               std::string::npos &&
             !strict_escaped_inline.success,
         "escaped mixed-case URL functions in inline CSS are diagnosed and removed");

  const auto unreferenced_script = squisher.convert_string(
      root("<defs><script>console.log('active')</script></defs>"
           "<rect width=\"10\" height=\"10\"/>"),
      strict);
  expect(!unreferenced_script.success &&
             unreferenced_script.error.find("unsupported-script") != std::string::npos,
         "strict mode rejects script content even when it is unreferenced");

  const auto unknown = squisher.convert_string(root(
      "<meshgradient id=\"m\"/><rect width=\"10\" height=\"10\"/>"), strict);
  expect(!unknown.success && unknown.error.find("unsupported-element") != std::string::npos,
         "strict mode rejects unknown SVG elements");

  for (const std::string& invalid_numeric : std::vector<std::string>{
         root("<rect x=\"1e999\" width=\"10\" height=\"10\"/>"),
         root("<rect width=\"10\" height=\"10\" opacity=\"nan\"/>"),
         root("<rect width=\"10\" height=\"10\" opacity=\"0.5junk\"/>"),
         root("<path d=\"M0 0H10\" fill=\"none\" stroke=\"black\" stroke-width=\"-4\"/>"),
         root("<rect width=\"10\" height=\"10\" transform=\"scale(1e999)\"/>"),
         "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 inf 10\">"
         "<rect width=\"10\" height=\"10\"/></svg>",
         "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0. 0 10 10\">"
         "<rect width=\"10\" height=\"10\"/></svg>"}) {
    const auto rejected_numeric = squisher.convert_string(invalid_numeric, strict);
    expect(!rejected_numeric.success && rejected_numeric.svg.empty() &&
               rejected_numeric.error.find("invalid-numeric-value") != std::string::npos,
           "strict mode rejects malformed, non-finite, and out-of-range numeric values");
  }

  const std::vector<std::string> malformed_comma_wsp{
    root("<rect width=\"10\" height=\"10\" transform=\"translate(,5 6)\"/>"),
    root("<rect width=\"10\" height=\"10\" transform=\"translate(5,,6)\"/>"),
    root("<rect width=\"10\" height=\"10\" transform=\"translate(5 6,)\"/>"),
    root("<rect width=\"10\" height=\"10\" transform=\",translate(5 6)\"/>"),
    root("<rect width=\"10\" height=\"10\" transform=\"translate(5 6),,scale(2)\"/>"),
    root("<rect width=\"10\" height=\"10\" transform=\"translate(5 6),\"/>"),
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\",0 0 10 10\">"
      "<rect width=\"10\" height=\"10\"/></svg>",
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0,,0 10 10\">"
      "<rect width=\"10\" height=\"10\"/></svg>",
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10,\">"
      "<rect width=\"10\" height=\"10\"/></svg>",
    root("<polyline points=\",0 0 10 10\"/>"),
    root("<polyline points=\"0,,0 10 10\"/>"),
    root("<polyline points=\"0 0 10 10,\"/>"),
  };
  for (const std::string& malformed : malformed_comma_wsp) {
    const auto rejected = squisher.convert_string(malformed, strict);
    expect(!rejected.success && rejected.svg.empty() &&
               rejected.error.find("invalid-numeric-value") != std::string::npos,
           "strict mode rejects leading, doubled, and trailing comma-wsp errors");
  }

  for (const std::string& malformed_path : {
         std::string("M,0 0L10 10"),
         std::string("M0,,0L10 10"),
         std::string("M0 0L10 10,")}) {
    const auto rejected = squisher.convert_string(
        root("<path d=\"" + malformed_path + "\"/>"), strict);
    expect(!rejected.success && rejected.svg.empty() &&
               rejected.error.find("invalid-path-data") != std::string::npos,
           "strict mode rejects malformed path comma-wsp: " + malformed_path);
  }

  const auto compatible_non_finite = squisher.convert_string(
      root("<rect x=\"1e999\" width=\"10\" height=\"10\" opacity=\"nan\"/>"));
  expect(compatible_non_finite.success &&
             diagnostic_message_contains(
                 compatible_non_finite, "invalid-numeric-value", "substitutes 0") &&
             diagnostic_message_contains(
                 compatible_non_finite, "invalid-numeric-value", "default value 1") &&
             compatible_non_finite.svg.find("inf") == std::string::npos &&
             compatible_non_finite.svg.find("nan") == std::string::npos,
         "compatible numeric diagnostics name coordinate and opacity fallbacks");

  const auto relative_coordinate = squisher.convert_string(
      root("<rect x=\"1in\" width=\"10\" height=\"10\"/>"));
  expect(relative_coordinate.success &&
             diagnostic_message_contains(
                 relative_coordinate, "unsupported-length-unit", "substitutes 0") &&
             relative_coordinate.svg.find("1in") == std::string::npos,
         "relative geometry-coordinate diagnostics state the zero fallback");

  const auto relative_extent = squisher.convert_string(
      root("<rect width=\"1in\" height=\"10\"/>"));
  expect(relative_extent.success &&
             diagnostic_message_contains(
                 relative_extent, "unsupported-length-unit", "rect geometry is dropped") &&
             relative_extent.svg.find("<path ") == std::string::npos,
         "relative geometry-extent diagnostics state that geometry is dropped");

  const auto text_position_fallback = squisher.convert_string(root(
      "<text x=\"12\" y=\"20\">first<tspan x=\"1in\">second</tspan></text>"));
  expect(text_position_fallback.success &&
             diagnostic_message_contains(
                 text_position_fallback,
                 "unsupported-length-unit",
                 "keeps the current text cursor x coordinate"),
         "unsupported text coordinate lists state that current cursor positioning is retained");

  const auto css_numeric_fallback = squisher.convert_string(root(
      "<path d=\"M0 0H10\" fill=\"none\" stroke=\"black\" "
      "style=\"stroke-width:-4\"/>"));
  expect(css_numeric_fallback.success &&
             diagnostic_message_contains(
                 css_numeric_fallback,
                 "invalid-numeric-value",
                 "default stroke width of 1"),
         "invalid CSS numeric diagnostics state the substituted default");

  const auto css_unit_fallback = squisher.convert_string(root(
      "<text x=\"0\" y=\"20\" style=\"font-size:1in\">text</text>"));
  expect(css_unit_fallback.success &&
             diagnostic_message_contains(
                 css_unit_fallback,
                 "unsupported-length-unit",
                 "default font size of 16"),
         "unsupported CSS units state the substituted text default");

  const auto current_color = squisher.convert_string(
      root("<rect width=\"10\" height=\"10\" color=\"red\" fill=\"currentColor\"/>"));
  expect(current_color.success &&
             diagnostic_message_contains(
                 current_color,
                 "unsupported-current-color",
                 "output renderer resolves it"),
         "currentColor diagnostics state how compatible output handles the paint");

  const auto text_layout = squisher.convert_string(
      root("<text x=\"0\" y=\"20\" direction=\"rtl\">text</text>"));
  expect(text_layout.success &&
             diagnostic_message_contains(
                 text_layout, "unsupported-text-layout", "ignores direction"),
         "unsupported text-layout diagnostics state that the property is ignored");

  for (const std::string& invisible : {
         root("<rect width=\"10\" height=\"10\" display=\"none\"/>"),
         root("<g visibility=\"hidden\"><rect width=\"10\" height=\"10\"/></g>"),
         root("<rect width=\"10\" height=\"10\" fill=\"none\" stroke=\"none\"/>")}) {
    const auto accepted_invisible = squisher.convert_string(invisible, strict);
    expect(accepted_invisible.success && accepted_invisible.svg.find("<path ") == std::string::npos,
           "strict mode accepts intentionally invisible supported content");
  }
}

void test_safe_file_and_batch_workflow() {
  const fs::path temporary = fs::temp_directory_path() / "svg-squisher-core-tests";
  std::error_code ignored;
  fs::remove_all(temporary, ignored);
  fs::create_directories(temporary / "input/nested");

  const std::string sample = root("<rect width=\"20\" height=\"10\"/>");
  write_text(temporary / "input/b.svg", sample);
  write_text(temporary / "input/a.svg", sample);
  write_text(temporary / "input/nested/c.svg", sample);
  write_text(temporary / "input/bad.svg", "<svg><path></svg>");

  svg_squisher::SvgSquisher squisher;
  const fs::path prior_cwd = fs::current_path();
  fs::current_path(temporary);
  const auto basename = squisher.squish_file_with_result("input/a.svg", "result.svg");
  fs::current_path(prior_cwd);
  expect(basename.success && fs::exists(temporary / "result.svg"),
         "basename output works without an empty-parent filesystem error");

  const fs::path in_place_path = temporary / "in-place.svg";
  write_text(in_place_path, sample);
  const auto rejected_in_place =
      squisher.squish_file_with_result(in_place_path, in_place_path);
  expect(!rejected_in_place.success &&
             rejected_in_place.error.find("allow_in_place") != std::string::npos &&
             svg_squisher::read_file(in_place_path) == sample,
         "core file API rejects in-place conversion by default without changing input");
  svg_squisher::Options allow_in_place;
  allow_in_place.allow_in_place = true;
  const auto accepted_in_place =
      squisher.squish_file_with_result(in_place_path, in_place_path, allow_in_place);
  expect(accepted_in_place.success &&
             svg_squisher::read_file(in_place_path).find("<path ") != std::string::npos,
         "core file API permits checked in-place replacement only when opted in");

  svg_squisher::Options recursive;
  recursive.recursive = true;
  const auto batch = squisher.squish_directory_with_result(
      temporary / "input", temporary / "output", recursive);
  expect(batch.converted == 3 && batch.failed == 1,
         "recursive batch continues and reports per-file failures");
  expect(fs::exists(temporary / "output/nested/c.svg"),
         "recursive batch preserves relative directories");
  expect(batch.files.size() == 4 && batch.files[0].input_path.filename() == "a.svg",
         "batch processing order is deterministic");

  const fs::path symlink_input = temporary / "symlink-input";
  const fs::path symlink_source = temporary / "symlink-source/source.svg";
  const fs::path symlink_output = temporary / "other-root/output";
  const fs::path escaped_destination = temporary / "other-root/symlink-source/source.svg";
  fs::create_directories(symlink_input);
  write_text(symlink_source, sample);
  write_text(escaped_destination, "sentinel");
  std::error_code symlink_error;
  fs::create_symlink(symlink_source, symlink_input / "linked.svg", symlink_error);
  if (!symlink_error) {
    const auto symlink_batch = squisher.squish_directory_with_result(
        symlink_input, symlink_output, recursive);
    expect(symlink_batch.converted == 0 && symlink_batch.failed == 0 &&
               symlink_batch.skipped == 1 && symlink_batch.files.size() == 1 &&
               symlink_batch.files.front().output_path == symlink_output / "linked.svg",
           "directory batches report SVG symlinks as contained skipped entries");
    expect(svg_squisher::read_file(escaped_destination) == "sentinel" &&
               !fs::exists(symlink_output / "linked.svg"),
           "recursive symlink inputs cannot write outside the output directory");
  }

  const fs::path linked_output_input = temporary / "linked-output-input/sub";
  const fs::path linked_output_root = temporary / "linked-output-root";
  const fs::path linked_output_outside = temporary / "linked-output-outside";
  const fs::path linked_output_sentinel = linked_output_outside / "a.svg";
  fs::create_directories(linked_output_input);
  fs::create_directories(linked_output_root);
  write_text(linked_output_input / "a.svg", sample);
  write_text(linked_output_sentinel, "outside sentinel");
  std::error_code output_link_error;
  fs::create_directory_symlink(
      linked_output_outside, linked_output_root / "sub", output_link_error);
#if !defined(_WIN32)
  expect(!output_link_error, "linked-output fixture creates a directory symlink");
#endif
  if (!output_link_error) {
    const auto linked_output_batch = squisher.squish_directory_with_result(
        temporary / "linked-output-input", linked_output_root, recursive);
    expect(linked_output_batch.converted == 0 &&
               linked_output_batch.failed == 1 &&
               linked_output_batch.skipped == 0 &&
               linked_output_batch.files.size() == 1 &&
               linked_output_batch.files.front().error.find(
                   "symbolic link or reparse point") != std::string::npos,
           "recursive batches report a linked destination parent as a failure");
    expect(svg_squisher::read_file(linked_output_sentinel) == "outside sentinel",
           "a linked destination parent cannot overwrite a file outside the output root");
  }

  fs::create_directories(temporary / "contained/generated/previous");
  write_text(temporary / "contained/source.svg", sample);
  write_text(temporary / "contained/generated/prior.svg", sample);
  write_text(temporary / "contained/generated/previous/older.svg", sample);
  const auto contained_output = squisher.squish_directory_with_result(
      temporary / "contained", temporary / "contained/generated", recursive);
  expect(contained_output.converted == 1 && contained_output.failed == 0 &&
             contained_output.files.size() == 1 &&
             contained_output.files.front().input_path.filename() == "source.svg",
         "recursive batch excludes an existing output subtree nested under input");
  expect(fs::exists(temporary / "contained/generated/source.svg"),
         "nested output exclusion still converts source files outside the output subtree");
  const auto repeated_contained_output = squisher.squish_directory_with_result(
      temporary / "contained", temporary / "contained/generated", recursive);
  expect(repeated_contained_output.files.size() == 1 &&
             repeated_contained_output.files.front().input_path.filename() == "source.svg",
         "nested output exclusion remains deterministic after generated files exist");

  svg_squisher::Options no_overwrite;
  no_overwrite.overwrite = false;
  const auto skipped = squisher.squish_file_with_result(
      temporary / "input/a.svg", temporary / "result.svg", no_overwrite);
  expect(skipped.skipped && !skipped.success, "no-overwrite reports a skipped conversion");

  auto report_batch = batch;
  report_batch.files.front().fonts_used.push_back(u8"fonts/fönt.bin");
  report_batch.files.front().font_identities.push_back({
      u8"fonts/fönt.bin",
      3,
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "",
  });
  const std::string report = svg_squisher::render_json_report(report_batch, recursive);
  expect(report.find("\"schemaVersion\": 1") != std::string::npos &&
             report.find("\"generatorVersion\":") != std::string::npos &&
             report.find("\"status\":\"failed\"") != std::string::npos &&
             report.find("nested/c.svg") != std::string::npos &&
             report.find("\"continueOnError\":true") != std::string::npos &&
             report.find("\"allowInPlace\":false") != std::string::npos &&
             report.find("\"fonts\":[") != std::string::npos &&
             report.find(u8"\"path\":\"fonts/fönt.bin\",\"bytes\":3,") !=
                 std::string::npos &&
             report.find("\"sha256\":\"ba7816bf8f01cfea414140de5dae2223"
                         "b00361a396177a9cb410ff61f20015ad\"") != std::string::npos &&
             report.find("\"missingGlyphs\":") != std::string::npos &&
             report.find("\"outputPathCommands\":") != std::string::npos,
         "JSON report includes schema, failures, paths, and byte-level font provenance");

  fs::remove_all(temporary, ignored);
}

}  // namespace

int main() {
  test_exclusive_atomic_file_writes();
  test_expanded_traversal_bounds();
  test_symbol_viewport_contract();
  test_conversion_contract();
  test_explicit_conversion_policies();
  test_preserved_document_semantics();
  test_strict_capability_coverage();
  test_safe_file_and_batch_workflow();
  if (failures == 0) std::cout << "All core tests passed\n";
  return failures == 0 ? 0 : 1;
}
