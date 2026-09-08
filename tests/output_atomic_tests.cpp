#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "svg_output.h"
#include "svg_squisher.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

fs::path unique_test_directory() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("svg-squisher-no-overwrite-" + std::to_string(suffix));
}

void wait_for_start(std::atomic<std::size_t>& ready,
                    std::atomic<bool>& start,
                    std::size_t participant_count) {
  ready.fetch_add(1, std::memory_order_acq_rel);
  while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
  (void)participant_count;
}

void release_together(std::atomic<std::size_t>& ready,
                      std::atomic<bool>& start,
                      std::size_t participant_count) {
  while (ready.load(std::memory_order_acquire) != participant_count) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
}

bool has_temporary_output(const fs::path& directory, const std::string& output_name) {
  const std::string prefix = "." + output_name + ".svg-squisher-";
  for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
    if (entry.path().filename().string().rfind(prefix, 0) == 0) return true;
  }
  return false;
}

void test_atomic_create_only_writer() {
  const fs::path temporary = unique_test_directory();
  fs::create_directories(temporary);
  const fs::path output = temporary / "winner.svg";
  constexpr std::size_t writer_count = 16;

  std::vector<std::string> payloads;
  std::vector<int> installed(writer_count, 0);
  std::vector<std::string> errors(writer_count);
  std::vector<std::thread> writers;
  std::atomic<std::size_t> ready{0};
  std::atomic<bool> start{false};
  for (std::size_t index = 0; index < writer_count; ++index) {
    payloads.push_back(
      "writer-" + std::to_string(index) + "\n" +
      std::string(128 * 1024, static_cast<char>('A' + index)));
  }
  for (std::size_t index = 0; index < writer_count; ++index) {
    writers.emplace_back([&, index] {
      wait_for_start(ready, start, writer_count);
      try {
        installed[index] = svg_squisher::write_file(output, payloads[index], false) ? 1 : 0;
      } catch (const std::exception& error) {
        errors[index] = error.what();
      }
    });
  }
  release_together(ready, start, writer_count);
  for (std::thread& writer : writers) writer.join();

  expect(std::all_of(errors.begin(), errors.end(), [](const std::string& error) {
           return error.empty();
         }),
         "concurrent create-only writers complete without I/O errors");
  expect(std::count(installed.begin(), installed.end(), 1) == 1,
         "exactly one concurrent create-only writer atomically claims the destination");
  const std::string final_payload = svg_squisher::read_file(output);
  expect(std::find(payloads.begin(), payloads.end(), final_payload) != payloads.end(),
         "the create-only destination contains one complete writer payload");
  expect(!svg_squisher::write_file(output, "must not replace", false) &&
           svg_squisher::read_file(output) == final_payload,
         "a later create-only write reports the collision and preserves destination bytes");
  expect(!has_temporary_output(temporary, output.filename().string()),
         "create-only races leave no temporary output files");

  std::error_code ignored;
  fs::remove_all(temporary, ignored);
}

std::string large_svg_fixture() {
  std::string source =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\">";
  for (std::size_t index = 0; index < 1500; ++index) {
    source += "<rect x=\"" + std::to_string(index % 100) +
              "\" y=\"" + std::to_string((index / 100) % 100) +
              "\" width=\"1\" height=\"1\"/>";
  }
  source += "</svg>";
  return source;
}

void test_no_overwrite_conversion_race_is_reported_as_skipped() {
  const fs::path temporary = unique_test_directory();
  fs::create_directories(temporary);
  const fs::path input = temporary / "input.svg";
  const fs::path output = temporary / "output.svg";
  const std::string source = large_svg_fixture();
  svg_squisher::write_file(input, source);

  svg_squisher::Options options;
  options.overwrite = false;
  const svg_squisher::ConversionResult expected =
    svg_squisher::SvgSquisher{}.convert_string(source, options);
  expect(expected.success, "no-overwrite race fixture converts successfully");

  constexpr std::size_t converter_count = 8;
  std::vector<svg_squisher::FileConversionResult> results(converter_count);
  std::vector<std::thread> converters;
  std::atomic<std::size_t> ready{0};
  std::atomic<bool> start{false};
  for (std::size_t index = 0; index < converter_count; ++index) {
    converters.emplace_back([&, index] {
      wait_for_start(ready, start, converter_count);
      results[index] = svg_squisher::SvgSquisher{}.squish_file_with_result(
        input, output, options);
    });
  }
  release_together(ready, start, converter_count);
  for (std::thread& converter : converters) converter.join();

  const std::size_t successes = static_cast<std::size_t>(std::count_if(
    results.begin(), results.end(),
    [](const svg_squisher::FileConversionResult& result) { return result.success; }));
  const std::size_t skips = static_cast<std::size_t>(std::count_if(
    results.begin(), results.end(),
    [](const svg_squisher::FileConversionResult& result) {
      return result.skipped && !result.success &&
             result.error.find("already exists") != std::string::npos;
    }));
  expect(successes == 1,
         "exactly one no-overwrite conversion wins the final atomic install");
  expect(skips == converter_count - 1,
         "every losing no-overwrite conversion is surfaced as skipped");
  expect(svg_squisher::read_file(output) == expected.svg,
         "the conversion race leaves the complete deterministic SVG output");
  expect(!has_temporary_output(temporary, output.filename().string()),
         "conversion races leave no temporary output files");

  std::error_code ignored;
  fs::remove_all(temporary, ignored);
}

void test_contained_writer_rejects_linked_descendants() {
  const fs::path temporary = unique_test_directory();
  const fs::path output_root = temporary / "output";
  const fs::path outside = temporary / "outside";
  const fs::path sentinel = outside / "result.svg";
  fs::create_directories(output_root);
  fs::create_directories(outside);
  svg_squisher::write_file(sentinel, "outside sentinel");

  bool lexical_escape_rejected = false;
  try {
    (void)svg_squisher::write_file_contained(
        output_root, "../outside/result.svg", "replacement");
  } catch (const std::exception& error) {
    lexical_escape_rejected =
        std::string(error.what()).find("parent components") != std::string::npos;
  }
  expect(lexical_escape_rejected &&
             svg_squisher::read_file(sentinel) == "outside sentinel",
         "contained writes reject lexical parent escapes before touching the target");

  std::error_code link_error;
  fs::create_directory_symlink(outside, output_root / "linked", link_error);
#if !defined(_WIN32)
  expect(!link_error, "contained-writer fixture creates a directory symlink");
#endif
  if (!link_error) {
    bool linked_parent_rejected = false;
    try {
      (void)svg_squisher::write_file_contained(
          output_root, "linked/result.svg", "replacement");
    } catch (const std::exception& error) {
      linked_parent_rejected =
          std::string(error.what()).find("symbolic link or reparse point") !=
          std::string::npos;
    }
    expect(linked_parent_rejected &&
               svg_squisher::read_file(sentinel) == "outside sentinel",
           "contained writes reject a linked parent and preserve outside bytes");
    expect(!has_temporary_output(outside, sentinel.filename().string()),
           "a rejected linked parent creates no temporary file outside the root");

    const fs::path actual_root = temporary / "actual-root";
    const fs::path linked_root = temporary / "linked-root";
    fs::create_directories(actual_root);
    std::error_code root_link_error;
    fs::create_directory_symlink(actual_root, linked_root, root_link_error);
    if (!root_link_error) {
      bool root_link_write_succeeded = false;
      try {
        root_link_write_succeeded = svg_squisher::write_file_contained(
            linked_root, "nested/allowed.svg", "allowed through selected root");
      } catch (...) {
        root_link_write_succeeded = false;
      }
      expect(root_link_write_succeeded &&
                 svg_squisher::read_file(actual_root / "nested/allowed.svg") ==
                     "allowed through selected root",
             "the explicitly selected output root may itself be a directory link");
    }
  }

  std::error_code ignored;
  fs::remove_all(temporary, ignored);
}

#if !defined(_WIN32)
void test_cross_process_atomic_replacement() {
  const fs::path temporary = unique_test_directory();
  fs::create_directories(temporary);
  const fs::path output = temporary / "process-winner.svg";
  const fs::path start = temporary / "start";
  constexpr std::size_t writer_count = 12;

  std::vector<std::string> payloads;
  std::vector<pid_t> children;
  payloads.reserve(writer_count);
  children.reserve(writer_count);
  for (std::size_t index = 0; index < writer_count; ++index) {
    payloads.push_back(
      "process-writer-" + std::to_string(index) + "\n" +
      std::string(512 * 1024, static_cast<char>('A' + index)));
    const pid_t child = ::fork();
    if (child == 0) {
      const fs::path ready = temporary / ("ready-" + std::to_string(index));
      try {
        if (!svg_squisher::write_file(ready, "ready")) _exit(2);
        while (!fs::exists(start)) ::usleep(1000);
        if (!svg_squisher::write_file(output, payloads[index])) _exit(3);
        _exit(0);
      } catch (...) {
        _exit(4);
      }
    }
    if (child > 0) {
      children.push_back(child);
    } else {
      expect(false, "a cross-process atomic writer can be started");
      break;
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    std::size_t ready_count = 0;
    for (std::size_t index = 0; index < children.size(); ++index) {
      if (fs::exists(temporary / ("ready-" + std::to_string(index)))) ++ready_count;
    }
    if (ready_count == children.size()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  svg_squisher::write_file(start, "start");

  for (const pid_t child : children) {
    int status = 0;
    expect(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
             WEXITSTATUS(status) == 0,
           "each forked atomic writer completes successfully");
  }
  expect(children.size() == writer_count,
         "the cross-process test starts every requested writer");
  if (fs::is_regular_file(output)) {
    const std::string final_payload = svg_squisher::read_file(output);
    expect(std::find(payloads.begin(), payloads.end(), final_payload) != payloads.end(),
           "cross-process replacement leaves one complete writer payload");
  } else {
    expect(false, "cross-process replacement creates the destination file");
  }
  expect(!has_temporary_output(temporary, output.filename().string()),
         "cross-process replacement leaves no temporary output files");

  std::error_code ignored;
  fs::remove_all(temporary, ignored);
}
#endif

}  // namespace

int main() {
  test_atomic_create_only_writer();
  test_no_overwrite_conversion_race_is_reported_as_skipped();
  test_contained_writer_rejects_linked_descendants();
#if !defined(_WIN32)
  test_cross_process_atomic_replacement();
#endif
  if (failures != 0) return 1;
  std::cout << "atomic output tests passed\n";
  return 0;
}
