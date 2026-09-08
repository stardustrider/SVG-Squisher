#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "svg_font_identity.h"
#include "svg_squisher.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

fs::path unique_test_path() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         fs::u8path("svg-squisher-f\xc3\xb6nt-" + std::to_string(suffix) + ".bin");
}

std::vector<unsigned char> read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path& path,
                 const std::vector<unsigned char>& bytes,
                 std::size_t padded_size = 0) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  for (std::size_t remaining = padded_size > bytes.size() ? padded_size - bytes.size() : 0;
       remaining > 0;) {
    const std::size_t amount = std::min<std::size_t>(remaining, 64 * 1024);
    const std::vector<char> padding(amount, 0);
    output.write(padding.data(), static_cast<std::streamsize>(padding.size()));
    remaining -= amount;
  }
  if (!output) throw std::runtime_error("Could not write test font snapshot");
}

std::optional<std::pair<fs::path, fs::path>> discover_distinct_font_pair() {
  const std::vector<std::pair<fs::path, fs::path>> candidates{
    {"C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/times.ttf"},
    {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
     "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf"},
    {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSerif.ttf"},
    {"/System/Library/Fonts/Supplemental/Arial.ttf",
     "/System/Library/Fonts/Supplemental/Times New Roman.ttf"},
    {"/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/NewYork.ttf"},
  };
  for (const auto& candidate : candidates) {
    if (fs::is_regular_file(candidate.first) && fs::is_regular_file(candidate.second)) {
      return candidate;
    }
  }
  return std::nullopt;
}

void test_known_digest_without_metadata_cache() {
  const fs::path test_path = unique_test_path();
  const std::string caller_path = test_path.generic_u8string();
  {
    std::ofstream output(test_path, std::ios::binary | std::ios::trunc);
    expect(output.is_open(), "the UTF-8-named test file can be created");
    output.write("abc", 3);
    expect(output.good(), "the known SHA-256 test vector can be written");
  }

  const svg_squisher::FontIdentity first =
      svg_squisher::identify_font_file(caller_path);
  expect(first.path == caller_path, "font identity preserves the caller-provided path");
  expect(first.error.empty(), "a readable font file has no identity error");
  expect(first.bytes == 3, "font identity reports the exact byte count");
  expect(first.sha256 ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "font identity matches the SHA-256 known vector for abc");

  const fs::file_time_type unchanged_time = fs::last_write_time(test_path);
  {
    std::ofstream output(test_path, std::ios::binary | std::ios::trunc);
    output.write("abd", 3);
  }
  fs::last_write_time(test_path, unchanged_time);
  const svg_squisher::FontIdentity same_metadata =
      svg_squisher::identify_font_file(caller_path);
  expect(same_metadata.bytes == first.bytes && same_metadata.sha256 ==
             "a52d159f262b2c6ddb724a61840befc36eb30c88877a4030b65cbe86298449c9",
         "same-size, same-mtime replacement is re-read instead of returning stale identity");

  const std::string padding_boundary =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  {
    std::ofstream output(test_path, std::ios::binary | std::ios::trunc);
    output.write(padding_boundary.data(),
                 static_cast<std::streamsize>(padding_boundary.size()));
    expect(output.good(), "the SHA-256 padding-boundary vector can be written");
  }
  const svg_squisher::FontIdentity boundary =
      svg_squisher::identify_font_file(caller_path);
  expect(boundary.bytes == padding_boundary.size(),
         "a changed-size font invalidates the cached byte count");
  expect(boundary.sha256 ==
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "font identity hashes a SHA-256 padding-boundary vector correctly");

  std::error_code remove_error;
  fs::remove(test_path, remove_error);
  expect(!remove_error, "the UTF-8-named test file can be removed");

  const svg_squisher::FontIdentity missing =
      svg_squisher::identify_font_file(caller_path);
  expect(missing.path == caller_path, "a missing file still preserves the caller path");
  expect(!missing.bytes.has_value(), "a missing file does not claim a byte count");
  expect(!missing.sha256.has_value(), "a missing file does not claim a SHA-256 digest");
  expect(!missing.error.empty(), "a missing file reports why identity is unavailable");
}

void test_shaping_and_identity_share_owned_bytes() {
  const auto font_pair = discover_distinct_font_pair();
  expect(font_pair.has_value(), "two distinct system fonts are available for snapshot testing");
  if (!font_pair) return;

  const std::vector<unsigned char> first_font = read_bytes(font_pair->first);
  const std::vector<unsigned char> second_font = read_bytes(font_pair->second);
  expect(!first_font.empty() && !second_font.empty(), "font snapshot fixtures are readable");
  if (first_font.empty() || second_font.empty()) return;

  const fs::path test_path = unique_test_path();
  const std::string caller_path = test_path.generic_u8string();
  const std::size_t padded_size = std::max(first_font.size(), second_font.size());
  const fs::file_time_type fixed_time = fs::file_time_type::clock::now() -
                                        std::chrono::hours(24);
  const std::string source =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 300 80\">"
    "<text x=\"5\" y=\"55\" font-size=\"48\">WMWM</text></svg>";
  svg_squisher::Options options;
  options.font_path = caller_path;
  svg_squisher::SvgSquisher squisher;

  write_bytes(test_path, first_font, padded_size);
  fs::last_write_time(test_path, fixed_time);
  const svg_squisher::ConversionResult first = squisher.convert_string(source, options);
  const svg_squisher::FontIdentity first_disk =
      svg_squisher::identify_font_file(caller_path);
  expect(first.success && first.font_identities.size() == 1 &&
             first.fonts_used == std::vector<std::string>{caller_path} &&
             first.font_identities.front().sha256 == first_disk.sha256,
         "first conversion reports the identity of the bytes used for shaping");

  write_bytes(test_path, second_font, padded_size);
  fs::last_write_time(test_path, fixed_time);
  const svg_squisher::ConversionResult second = squisher.convert_string(source, options);
  const svg_squisher::FontIdentity second_disk =
      svg_squisher::identify_font_file(caller_path);
  expect(second.success && second.font_identities.size() == 1 &&
             second.fonts_used == std::vector<std::string>{caller_path} &&
             second.font_identities.front().sha256 == second_disk.sha256,
         "replacement conversion reports the identity of its newly shaped bytes");
  if (first.font_identities.size() == 1 && second.font_identities.size() == 1) {
    expect(first.font_identities.front().bytes == second.font_identities.front().bytes &&
               first.font_identities.front().sha256 != second.font_identities.front().sha256,
           "same-size, same-mtime valid font replacement changes reported byte identity");
  }
  expect(first.svg != second.svg,
         "same-path valid font replacement reloads the FreeType face and changes outlines");

  std::error_code ignored;
  fs::remove(test_path, ignored);
}

}  // namespace

int main() {
  try {
    test_known_digest_without_metadata_cache();
    test_shaping_and_identity_share_owned_bytes();
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: unexpected exception: " << exception.what() << '\n';
    return 1;
  }

  if (failures != 0) {
    std::cerr << failures << " font identity test(s) failed\n";
    return 1;
  }
  std::cout << "All font identity tests passed\n";
  return 0;
}
