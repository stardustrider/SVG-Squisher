#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "svg_font_identity.h"

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

void test_known_digest_and_cache() {
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

  const svg_squisher::FontIdentity cached =
      svg_squisher::identify_font_file(caller_path);
  expect(cached.path == first.path && cached.bytes == first.bytes &&
             cached.sha256 == first.sha256 && cached.error.empty(),
         "a cached lookup returns the same successful identity");

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

}  // namespace

int main() {
  try {
    test_known_digest_and_cache();
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
