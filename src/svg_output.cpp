#include "svg_output.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <deque>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "svg_computed_style.h"
#include "svg_entry.h"
#include "svg_geometry.h"
#include "svg_paint.h"
#include "svg_path.h"
#include "svg_path_data.h"
#include "svg_shape.h"
#include "svg_style.h"
#include "svg_transform.h"
#include "svg_util.h"

namespace fs = std::filesystem;

namespace svg_squisher {
namespace {

std::atomic<unsigned long long> temporary_file_counter{0};

struct ExclusiveTemporaryFile {
  fs::path path;
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;

  ExclusiveTemporaryFile(fs::path temporary_path, HANDLE temporary_handle)
      : path(std::move(temporary_path)), handle(temporary_handle) {}
#else
  int descriptor = -1;

  ExclusiveTemporaryFile(fs::path temporary_path, int temporary_descriptor)
      : path(std::move(temporary_path)), descriptor(temporary_descriptor) {}
#endif

  ExclusiveTemporaryFile(const ExclusiveTemporaryFile&) = delete;
  ExclusiveTemporaryFile& operator=(const ExclusiveTemporaryFile&) = delete;

  ExclusiveTemporaryFile(ExclusiveTemporaryFile&& other) noexcept
      : path(std::move(other.path)) {
#if defined(_WIN32)
    handle = other.handle;
    other.handle = INVALID_HANDLE_VALUE;
#else
    descriptor = other.descriptor;
    other.descriptor = -1;
#endif
    other.path.clear();
  }

  void disarm_path() noexcept {
    path.clear();
  }

  void remove_owned_path() noexcept {
    if (path.empty()) return;
    std::error_code ignored;
    fs::remove(path, ignored);
    path.clear();
  }

  ~ExclusiveTemporaryFile() {
#if defined(_WIN32)
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      handle = INVALID_HANDLE_VALUE;
    }
#else
    if (descriptor >= 0) {
      ::close(descriptor);
      descriptor = -1;
    }
#endif
    // The temporary path is normally consumed by the final install. On any
    // exception, close its handle first and then remove it. Windows opens the
    // file without delete sharing, so attempting removal before this point
    // would silently leave the temporary file behind.
    remove_owned_path();
  }
};

unsigned long long current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<unsigned long long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long long>(::getpid());
#endif
}

#if !defined(_WIN32)
std::string native_error_message(int error) {
  return std::error_code(error, std::generic_category()).message();
}
#endif

struct ContainedOutputDestination {
  fs::path root;
  fs::path relative_path;
  fs::path path;
};

bool path_is_at_or_within(const fs::path& candidate, const fs::path& root) {
  auto candidate_component = candidate.begin();
  for (auto root_component = root.begin(); root_component != root.end();
       ++root_component, ++candidate_component) {
    if (candidate_component == candidate.end() ||
        *candidate_component != *root_component) {
      return false;
    }
  }
  return true;
}

fs::file_status checked_symlink_status(const fs::path& path) {
  std::error_code error;
  const fs::file_status status = fs::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("Unable to inspect contained output path: " + path.string() +
                             " (" + error.message() + ")");
  }
  return status;
}

bool path_is_symlink_or_reparse_point(const fs::path& path,
                                      const fs::file_status& status) {
  if (fs::is_symlink(status)) return true;
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const unsigned long error = GetLastError();
    throw std::runtime_error("Unable to inspect contained output path: " + path.string() +
                             " (Windows error " + std::to_string(error) + ")");
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  (void)path;
  return false;
#endif
}

void validate_relative_output_path(const fs::path& relative_path) {
  if (relative_path.empty() || relative_path.is_absolute() ||
      relative_path.has_root_path() || relative_path.filename().empty()) {
    throw std::runtime_error("Contained output path must name a relative file");
  }
  for (const fs::path& component : relative_path) {
    if (component.empty() || component == "." || component == "..") {
      throw std::runtime_error(
          "Contained output path cannot contain empty, current, or parent components");
    }
  }
}

void validate_contained_parent(const ContainedOutputDestination& destination) {
  fs::path current = destination.root;
  for (const fs::path& component : destination.relative_path.parent_path()) {
    current /= component;
    const fs::file_status status = checked_symlink_status(current);
    if (!fs::exists(status)) {
      throw std::runtime_error(
          "Contained output directory changed or disappeared during the write: " +
          current.string());
    }
    if (path_is_symlink_or_reparse_point(current, status)) {
      throw std::runtime_error(
          "Contained output path contains a symbolic link or reparse point beneath "
          "the output root: " + current.string());
    }
    if (!fs::is_directory(status)) {
      throw std::runtime_error(
          "Contained output parent is not a directory: " + current.string());
    }
  }

  std::error_code canonical_error;
  const fs::path canonical_parent = fs::canonical(current, canonical_error);
  if (canonical_error ||
      !path_is_at_or_within(canonical_parent, destination.root)) {
    throw std::runtime_error(
        "Contained output parent does not resolve beneath the output root: " +
        current.string());
  }
}

ContainedOutputDestination prepare_contained_output_destination(
    const fs::path& output_root,
    const fs::path& relative_path) {
  validate_relative_output_path(relative_path);
  if (output_root.empty()) {
    throw std::runtime_error("Contained output root must name a directory");
  }

  std::error_code absolute_error;
  const fs::path absolute_root = fs::absolute(output_root, absolute_error);
  if (absolute_error) {
    throw std::runtime_error("Unable to resolve output root: " + output_root.string() +
                             " (" + absolute_error.message() + ")");
  }

  std::error_code create_error;
  fs::create_directories(absolute_root, create_error);
  if (create_error) {
    throw std::runtime_error("Unable to create output root: " + output_root.string() +
                             " (" + create_error.message() + ")");
  }

  std::error_code canonical_error;
  const fs::path canonical_root = fs::canonical(absolute_root, canonical_error);
  if (canonical_error) {
    throw std::runtime_error("Unable to resolve output root: " + output_root.string() +
                             " (" + canonical_error.message() + ")");
  }
  std::error_code directory_error;
  if (!fs::is_directory(canonical_root, directory_error) || directory_error) {
    throw std::runtime_error("Contained output root is not a directory: " +
                             output_root.string());
  }

  fs::path current = canonical_root;
  for (const fs::path& component : relative_path.parent_path()) {
    current /= component;
    fs::file_status status = checked_symlink_status(current);
    if (!fs::exists(status)) {
      std::error_code component_error;
      fs::create_directory(current, component_error);
      if (component_error) {
        throw std::runtime_error("Unable to create contained output directory: " +
                                 current.string() + " (" +
                                 component_error.message() + ")");
      }
      status = checked_symlink_status(current);
    }
    if (path_is_symlink_or_reparse_point(current, status)) {
      throw std::runtime_error(
          "Contained output path contains a symbolic link or reparse point beneath "
          "the output root: " + current.string());
    }
    if (!fs::is_directory(status)) {
      throw std::runtime_error(
          "Contained output parent is not a directory: " + current.string());
    }
  }

  ContainedOutputDestination destination{
      canonical_root,
      relative_path,
      canonical_root / relative_path,
  };
  validate_contained_parent(destination);
  return destination;
}

void validate_contained_temporary_file(
    const ContainedOutputDestination& destination,
    const fs::path& temporary_path) {
  const fs::file_status status = checked_symlink_status(temporary_path);
  if (!fs::is_regular_file(status) ||
      path_is_symlink_or_reparse_point(temporary_path, status)) {
    throw std::runtime_error(
        "Contained temporary output is no longer an ordinary file: " +
        temporary_path.string());
  }

  std::error_code canonical_error;
  const fs::path canonical_temporary = fs::canonical(temporary_path, canonical_error);
  if (canonical_error ||
      !path_is_at_or_within(canonical_temporary, destination.root)) {
    throw std::runtime_error(
        "Contained temporary output does not resolve beneath the output root: " +
        temporary_path.string());
  }
}

ExclusiveTemporaryFile create_exclusive_temporary_file(const fs::path& path) {
  const fs::path parent = path.parent_path();
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto suffix = temporary_file_counter.fetch_add(1, std::memory_order_relaxed);
    fs::path candidate_name(".");
    candidate_name += path.filename().native();
    candidate_name +=
        fs::path(".svg-squisher-" + std::to_string(current_process_id()) + "-" +
                 std::to_string(suffix) + ".tmp").native();
    fs::path candidate = parent / candidate_name;
#if defined(_WIN32)
    HANDLE handle = CreateFileW(candidate.c_str(),
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      return ExclusiveTemporaryFile(std::move(candidate), handle);
    }
    const unsigned long error = GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
    throw std::runtime_error("Unable to create temporary output file: " + candidate.string() +
                             " (Windows error " + std::to_string(error) + ")");
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    int descriptor;
    do {
      descriptor = ::open(candidate.c_str(), flags, 0666);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor >= 0) {
      return ExclusiveTemporaryFile(std::move(candidate), descriptor);
    }
    const int error = errno;
    if (error == EEXIST) continue;
    throw std::runtime_error("Unable to create temporary output file: " + candidate.string() +
                             " (" + native_error_message(error) + ")");
#endif
  }
  throw std::runtime_error("Unable to allocate temporary output beside: " + path.string());
}

void write_flush_and_close(ExclusiveTemporaryFile& temporary,
                           const fs::path& output_path,
                           const std::string& text) {
  std::size_t offset = 0;
#if defined(_WIN32)
  while (offset < text.size()) {
    const std::size_t remaining = text.size() - offset;
    const DWORD requested = static_cast<DWORD>(
        std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(temporary.handle, text.data() + offset, requested, &written, nullptr) ||
        written == 0) {
      const unsigned long error = GetLastError();
      throw std::runtime_error("Unable to write output file completely: " +
                               output_path.string() + " (Windows error " +
                               std::to_string(error) + ")");
    }
    offset += written;
  }
  if (!FlushFileBuffers(temporary.handle)) {
    const unsigned long error = GetLastError();
    throw std::runtime_error("Unable to flush output file: " + output_path.string() +
                             " (Windows error " + std::to_string(error) + ")");
  }
  if (!CloseHandle(temporary.handle)) {
    const unsigned long error = GetLastError();
    throw std::runtime_error("Unable to close output file: " + output_path.string() +
                             " (Windows error " + std::to_string(error) + ")");
  }
  temporary.handle = INVALID_HANDLE_VALUE;
#else
  while (offset < text.size()) {
    const std::size_t remaining = text.size() - offset;
    const std::size_t requested = std::min<std::size_t>(
        remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    ssize_t written;
    do {
      written = ::write(temporary.descriptor, text.data() + offset, requested);
    } while (written < 0 && errno == EINTR);
    if (written <= 0) {
      const int error = written < 0 ? errno : EIO;
      throw std::runtime_error("Unable to write output file completely: " +
                               output_path.string() + " (" + native_error_message(error) + ")");
    }
    offset += static_cast<std::size_t>(written);
  }
  int sync_result;
  do {
    sync_result = ::fsync(temporary.descriptor);
  } while (sync_result != 0 && errno == EINTR);
  if (sync_result != 0) {
    const int error = errno;
    throw std::runtime_error("Unable to flush output file: " + output_path.string() +
                             " (" + native_error_message(error) + ")");
  }
  const int descriptor = temporary.descriptor;
  temporary.descriptor = -1;
  if (::close(descriptor) != 0) {
    const int error = errno;
    throw std::runtime_error("Unable to close output file: " + output_path.string() +
                             " (" + native_error_message(error) + ")");
  }
#endif
}

void replace_file(ExclusiveTemporaryFile& temporary, const fs::path& output_path) {
#if defined(_WIN32)
  if (!MoveFileExW(temporary.path.c_str(),
                   output_path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const unsigned long error = GetLastError();
    throw std::runtime_error("Unable to replace output file: " + output_path.string() +
                             " (Windows error " + std::to_string(error) + ")");
  }
#else
  std::error_code ec;
  fs::rename(temporary.path, output_path, ec);
  if (ec) {
    throw std::runtime_error("Unable to replace output file: " + output_path.string() +
                             " (" + ec.message() + ")");
  }
#endif
  temporary.disarm_path();
}

bool install_file_without_replacement(ExclusiveTemporaryFile& temporary,
                                      const fs::path& output_path) {
#if defined(_WIN32)
  if (MoveFileExW(
        temporary.path.c_str(), output_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
    temporary.disarm_path();
    return true;
  }
  const unsigned long error = GetLastError();
  if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) return false;
  throw std::runtime_error("Unable to install output file: " + output_path.string() +
                           " (Windows error " + std::to_string(error) + ")");
#else
  int link_result;
  do {
    link_result = ::link(temporary.path.c_str(), output_path.c_str());
  } while (link_result != 0 && errno == EINTR);
  if (link_result == 0) {
    temporary.remove_owned_path();
    return true;
  }
  const int error = errno;
  if (error == EEXIST) return false;
  throw std::runtime_error("Unable to install output file: " + output_path.string() +
                           " (" + native_error_message(error) + ")");
#endif
}

void collect_referenced_def_ids(const std::vector<PathEntry>& paths,
                                std::unordered_set<std::string>& referenced_ids) {
  for (const PathEntry& entry : paths) {
    const PathEntryAnalysis analysis = analyze_path_entry(entry);
    if (entry.emit_fill && analysis.fill_paint.is_gradient) {
      if (const auto id = parse_paint(entry.fill).url_id) referenced_ids.insert(*id);
    }
    if (entry.emit_stroke && analysis.stroke_paint.is_gradient) {
      if (const auto id = parse_paint(entry.stroke).url_id) referenced_ids.insert(*id);
    }
  }
}

bool is_local_fragment_reference(const std::string& value) {
  const std::string target = trim(value);
  return target.size() > 1 && target.front() == '#';
}

void collect_reference_ids_from_value(const std::string& value,
                                      std::unordered_set<std::string>& referenced_ids) {
  for (const std::string& id : analyze_css_urls(value).local_fragment_ids) {
    referenced_ids.insert(id);
  }

  const std::string trimmed_value = trim(value);
  if (is_local_fragment_reference(trimmed_value)) {
    referenced_ids.insert(trimmed_value.substr(1));
  }
}

bool definition_element_is_safe(const std::string& name) {
  static const std::unordered_set<std::string> safe_elements = {
    "defs", "linearGradient", "radialGradient", "pattern", "stop", "g", "symbol",
    "path", "rect", "circle", "ellipse", "line", "polyline", "polygon", "use",
    "title", "desc",
  };
  return safe_elements.find(name) != safe_elements.end();
}

bool definition_attribute_is_unsafe(const pugi::xml_attribute& attribute) {
  const std::string name = lower_copy(attribute.name());
  const std::size_t namespace_separator = name.rfind(':');
  const std::string local_name = namespace_separator == std::string::npos
      ? name
      : name.substr(namespace_separator + 1);
  const std::string value = attribute.as_string();
  if (local_name == "style" || name == "xml:base" || local_name == "src" ||
      local_name.rfind("on", 0) == 0) {
    return true;
  }
  if (local_name == "href") {
    return !is_local_fragment_reference(value);
  }
  static const std::unordered_set<std::string> unsupported_attributes = {
    "clip-path", "mask", "filter", "marker-start", "marker-mid", "marker-end",
  };
  return unsupported_attributes.find(local_name) != unsupported_attributes.end() ||
         analyze_css_urls(value).has_unsafe_url;
}

void sanitize_definition_subtree(const pugi::xml_node& root) {
  std::vector<pugi::xml_node> pending{root};
  while (!pending.empty()) {
    pugi::xml_node node = pending.back();
    pending.pop_back();

    for (pugi::xml_attribute attribute = node.first_attribute(); attribute;) {
      const pugi::xml_attribute next = attribute.next_attribute();
      if (definition_attribute_is_unsafe(attribute)) node.remove_attribute(attribute);
      attribute = next;
    }

    for (pugi::xml_node child = node.first_child(); child;) {
      const pugi::xml_node next = child.next_sibling();
      if (child.type() == pugi::node_element) {
        if (definition_element_is_safe(child.name())) {
          pending.push_back(child);
        } else {
          node.remove_child(child);
        }
      } else if (child.type() != pugi::node_pcdata && child.type() != pugi::node_cdata) {
        node.remove_child(child);
      }
      child = next;
    }
  }
}

std::string safe_serialized_paint(const std::string& paint) {
  return analyze_css_urls(paint).has_unsafe_url ? "none" : paint;
}

void collect_definition_dependencies(const pugi::xml_node& node,
                                     std::unordered_set<std::string>& referenced_ids) {
  for (const pugi::xml_attribute attribute : node.attributes()) {
    collect_reference_ids_from_value(attribute.as_string(), referenced_ids);
  }
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_element) {
      collect_definition_dependencies(child, referenced_ids);
    } else if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
      collect_reference_ids_from_value(child.value(), referenced_ids);
    }
  }
}

using DefinitionIndex = std::unordered_map<std::string, pugi::xml_node>;

DefinitionIndex index_definitions(const pugi::xml_node& root) {
  DefinitionIndex definitions;
  std::deque<pugi::xml_node> pending{root};
  while (!pending.empty()) {
    const pugi::xml_node node = pending.front();
    pending.pop_front();
    if (const pugi::xml_attribute id = node.attribute("id")) {
      definitions.emplace(id.as_string(), node);
    }
    for (const pugi::xml_node child : node.children()) {
      if (child.type() == pugi::node_element) pending.push_back(child);
    }
  }
  return definitions;
}

void collect_use_targets(const pugi::xml_node& node,
                         std::unordered_set<std::string>& target_ids) {
  if (std::string(node.name()) == "use") {
    const pugi::xml_attribute href = node.attribute("href")
      ? node.attribute("href")
      : node.attribute("xlink:href");
    const std::string target = href ? trim(href.as_string()) : "";
    if (is_local_fragment_reference(target)) target_ids.insert(target.substr(1));
  }
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_element) collect_use_targets(child, target_ids);
  }
}

void set_attribute(pugi::xml_node& node, const char* name, const std::string& value) {
  pugi::xml_attribute attribute = node.attribute(name);
  if (!attribute) attribute = node.append_attribute(name);
  attribute.set_value(value.c_str());
}

void materialize_definition_styles(const pugi::xml_node& source,
                                   pugi::xml_node destination,
                                   const std::vector<CssRule>& rules,
                                   const StyleState& inherited,
                                   const std::unordered_set<std::string>& use_target_ids,
                                   bool local_properties_only) {
  if (source.type() != pugi::node_element || destination.type() != pugi::node_element) return;
  if (const pugi::xml_attribute id = source.attribute("id")) {
    local_properties_only = local_properties_only ||
      use_target_ids.find(id.as_string()) != use_target_ids.end();
  }
  const StyleState style = resolve_style(source, rules, inherited);
  const std::string name = source.name();
  const bool presentation_container =
    name != "linearGradient" && name != "radialGradient" && name != "stop" &&
    name != "filter" && !(name.size() > 2 && name[0] == 'f' && name[1] == 'e');
  if (presentation_container) {
    const auto materialize = [&](const char* property, const std::string& value) {
      if (!local_properties_only || node_has_local_property(source, rules, property)) {
        set_attribute(destination, property, value);
      }
    };
    materialize("fill", safe_serialized_paint(style.fill));
    materialize("fill-opacity", style.fill_opacity);
    materialize("fill-rule", style.fill_rule);
    materialize("stroke", safe_serialized_paint(style.stroke));
    materialize("stroke-opacity", style.stroke_opacity);
    materialize("stroke-width", style.stroke_width);
    if (!style.stroke_dasharray.empty()) {
      materialize("stroke-dasharray", style.stroke_dasharray);
    }
    materialize("stroke-linecap", style.stroke_linecap);
    materialize("stroke-linejoin", style.stroke_linejoin);
    materialize("stroke-miterlimit", style.stroke_miterlimit);
    materialize("opacity", style.opacity);
    materialize("display", style.display);
    materialize("visibility", style.visibility);
  }

  pugi::xml_node destination_child = destination.first_child();
  for (const pugi::xml_node source_child : source.children()) {
    while (destination_child && destination_child.type() != source_child.type()) {
      destination_child = destination_child.next_sibling();
    }
    if (!destination_child) break;
    if (source_child.type() == pugi::node_element) {
      materialize_definition_styles(
        source_child, destination_child, rules, style, use_target_ids,
        local_properties_only);
    }
    destination_child = destination_child.next_sibling();
  }
}

StyleState inherited_style_for_definition(const pugi::xml_node& definition,
                                          const std::vector<CssRule>& rules) {
  std::vector<pugi::xml_node> ancestors;
  for (pugi::xml_node node = definition.parent(); node && node.type() == pugi::node_element;
       node = node.parent()) {
    ancestors.push_back(node);
  }
  StyleState inherited;
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
    inherited = resolve_style(*it, rules, inherited);
  }
  return inherited;
}

bool points_differ(Point left, Point right) {
  return left.x != right.x || left.y != right.y;
}

bool definition_element_has_stroke_geometry(const pugi::xml_node& node,
                                            const ComputedStyle& computed) {
  const std::string name = node.name();
  if (name == "rect") {
    return attr_double(node, "width") > 0.0 && attr_double(node, "height") > 0.0;
  }
  if (name == "circle") return attr_double(node, "r") > 0.0;
  if (name == "ellipse") {
    return attr_double(node, "rx") > 0.0 && attr_double(node, "ry") > 0.0;
  }
  if (name == "line") {
    const Point start{attr_double(node, "x1"), attr_double(node, "y1")};
    const Point end{attr_double(node, "x2"), attr_double(node, "y2")};
    return points_differ(start, end) || computed.stroke_linecap != StrokeLineCap::Butt;
  }

  std::optional<ParsedPath> path;
  if (name == "path") {
    path = parse_path_data(node.attribute("d").as_string());
  } else if (name == "polyline" || name == "polygon") {
    const auto parsed_points = parse_points_list(node.attribute("points").as_string());
    if (!parsed_points || parsed_points->size() < 4) return false;
    const std::vector<double>& points = *parsed_points;
    ParsedPath parsed;
    Point current{points[0], points[1]};
    parsed.segments.push_back({PathSegmentKind::Move, {}, {}, {}, current});
    for (std::size_t index = 2; index < points.size(); index += 2) {
      const Point next{points[index], points[index + 1]};
      parsed.segments.push_back({PathSegmentKind::Line, current, {}, {}, next});
      current = next;
    }
    if (name == "polygon") {
      parsed.segments.push_back(
        {PathSegmentKind::Close, current, {}, {}, {points[0], points[1]}});
    }
    path = std::move(parsed);
  } else {
    return false;
  }
  return path && std::any_of(path->segments.begin(), path->segments.end(),
                             [&](const PathSegment& segment) {
    switch (segment.kind) {
      case PathSegmentKind::Move:
        return false;
      case PathSegmentKind::Line:
        return points_differ(segment.start, segment.end) ||
               computed.stroke_linecap != StrokeLineCap::Butt;
      case PathSegmentKind::Close:
        return points_differ(segment.start, segment.end);
      case PathSegmentKind::Arc:
        // An arc with a zero radius renders as a straight line.
        return points_differ(segment.start, segment.end);
      case PathSegmentKind::Quadratic:
        return points_differ(segment.start, segment.end) ||
               points_differ(segment.start, segment.control1) ||
               computed.stroke_linecap != StrokeLineCap::Butt;
      case PathSegmentKind::Cubic:
        return points_differ(segment.start, segment.end) ||
               points_differ(segment.start, segment.control1) ||
               points_differ(segment.start, segment.control2) ||
               computed.stroke_linecap != StrokeLineCap::Butt;
    }
    return false;
  });
}

enum class DefinitionNodeRole {
  Child,
  PatternRoot,
  UseRoot,
};

bool use_can_reference_geometry(const std::string& name) {
  static const std::unordered_set<std::string> elements = {
    "g", "symbol", "path", "rect", "circle", "ellipse", "line", "polyline",
    "polygon", "use",
  };
  return elements.find(name) != elements.end();
}

bool definition_child_can_render(const std::string& name) {
  static const std::unordered_set<std::string> non_rendering = {
    "defs", "linearGradient", "radialGradient", "pattern", "stop", "symbol",
    "title", "desc",
  };
  return non_rendering.find(name) == non_rendering.end();
}

Matrix definition_transform(const pugi::xml_node& node, const Matrix& inherited) {
  Matrix composed = inherited;
  const auto append_transform = [&](const pugi::xml_attribute& attribute) {
    if (attribute && transform_is_valid(attribute.as_string())) {
      composed = multiply(composed, parse_transform(attribute.as_string()));
    }
  };
  append_transform(node.attribute("transform"));
  if (std::string(node.name()) == "pattern") {
    append_transform(node.attribute("patternTransform"));
  }
  if (std::string(node.name()) == "use") {
    Matrix translation;
    translation.e = attr_double(node, "x");
    translation.f = attr_double(node, "y");
    composed = multiply(composed, translation);
  }
  return composed;
}

bool transform_can_render(const Matrix& transform) {
  return transform.a * transform.d - transform.b * transform.c != 0.0;
}

bool materialized_definition_has_visible_live_stroke(
    const pugi::xml_node& node,
    const DefinitionIndex& definitions,
    const StyleState& inherited = {},
    bool ancestor_displayed = true,
    double ancestor_opacity = 1.0,
    const Matrix& ancestor_transform = {},
    DefinitionNodeRole role = DefinitionNodeRole::Child,
    std::unordered_set<std::string>* active_references = nullptr) {
  if (node.type() != pugi::node_element) return false;

  std::unordered_set<std::string> owned_active_references;
  if (!active_references) active_references = &owned_active_references;

  const std::string name = node.name();
  if (role == DefinitionNodeRole::PatternRoot && name != "pattern") return false;
  if (role == DefinitionNodeRole::UseRoot && !use_can_reference_geometry(name)) return false;
  if (role == DefinitionNodeRole::Child && !definition_child_can_render(name)) return false;

  static const std::vector<CssRule> no_rules;
  const StyleState style = resolve_style(node, no_rules, inherited);
  const ComputedStyle computed = compute_style(style);
  const bool displayed = ancestor_displayed && computed.displayed;
  const double effective_opacity = ancestor_opacity * computed.opacity;
  const Matrix transform = definition_transform(node, ancestor_transform);
  if (!displayed || effective_opacity <= 0.0 || !transform_can_render(transform)) return false;

  if (name == "use") {
    const pugi::xml_attribute href = node.attribute("href")
      ? node.attribute("href")
      : node.attribute("xlink:href");
    const std::string target = href ? trim(href.as_string()) : "";
    if (!is_local_fragment_reference(target)) return false;
    const std::string id = target.substr(1);
    const auto match = definitions.find(id);
    if (match == definitions.end() || !active_references->insert(id).second) return false;
    const bool visible = materialized_definition_has_visible_live_stroke(
      match->second, definitions, style, displayed, effective_opacity, transform,
      DefinitionNodeRole::UseRoot, active_references);
    active_references->erase(id);
    return visible;
  }

  if (computed.visible &&
      definition_element_has_stroke_geometry(node, computed) &&
      computed.has_stroke && computed.stroke_width > 0.0) {
    return true;
  }

  bool has_element_child = false;
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_element) has_element_child = true;
    if (materialized_definition_has_visible_live_stroke(
          child, definitions, style, displayed, effective_opacity, transform,
          DefinitionNodeRole::Child, active_references)) {
      return true;
    }
  }

  if (role == DefinitionNodeRole::PatternRoot && !has_element_child) {
    const pugi::xml_attribute href = node.attribute("href")
      ? node.attribute("href")
      : node.attribute("xlink:href");
    const std::string target = href ? trim(href.as_string()) : "";
    if (is_local_fragment_reference(target)) {
      const std::string id = target.substr(1);
      const auto match = definitions.find(id);
      if (match != definitions.end() && active_references->insert(id).second) {
        const bool visible = materialized_definition_has_visible_live_stroke(
          match->second, definitions, style, displayed, effective_opacity, transform,
          DefinitionNodeRole::PatternRoot, active_references);
        active_references->erase(id);
        if (visible) return true;
      }
    }
  }
  return false;
}

}  // namespace

std::string xml_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

SerializedDefinitions collect_serialized_defs(const pugi::xml_node& svg_root,
                                              const std::vector<PathEntry>& paths) {
  SerializedDefinitions result;
  std::unordered_set<std::string> referenced_ids;
  collect_referenced_def_ids(paths, referenced_ids);
  if (referenced_ids.empty()) return result;

  const std::unordered_set<std::string> paint_root_ids = referenced_ids;
  const DefinitionIndex source_definitions = index_definitions(svg_root);
  std::deque<std::string> pending(referenced_ids.begin(), referenced_ids.end());
  std::unordered_set<std::string> processed;
  std::unordered_set<std::string> use_target_ids;
  while (!pending.empty()) {
    const std::string id = pending.front();
    pending.pop_front();
    if (!processed.insert(id).second) continue;
    const auto definition = source_definitions.find(id);
    if (definition == source_definitions.end()) continue;

    std::unordered_set<std::string> dependencies;
    collect_definition_dependencies(definition->second, dependencies);
    collect_use_targets(definition->second, use_target_ids);
    for (const std::string& dependency : dependencies) {
      if (referenced_ids.insert(dependency).second) pending.push_back(dependency);
    }
  }

  const std::vector<CssRule> rules = parse_css_rules(svg_root);
  pugi::xml_document materialized;
  pugi::xml_node materialized_root = materialized.append_child("defs");
  for (const pugi::xpath_node& match : svg_root.select_nodes(".//*[@id]")) {
    const pugi::xml_node definition = match.node();
    const std::string id = definition.attribute("id").as_string();
    if (referenced_ids.find(id) == referenced_ids.end()) {
      continue;
    }
    if (!definition_element_is_safe(definition.name())) continue;
    pugi::xml_node copy = materialized_root.append_copy(definition);
    materialize_definition_styles(
      definition, copy, rules, inherited_style_for_definition(definition, rules),
      use_target_ids,
      use_target_ids.find(id) != use_target_ids.end());
    sanitize_definition_subtree(copy);
  }

  const DefinitionIndex retained_definitions = index_definitions(materialized_root);
  for (const std::string& id : paint_root_ids) {
    const auto definition = retained_definitions.find(id);
    if (definition == retained_definitions.end()) continue;
    std::unordered_set<std::string> active_references{id};
    if (materialized_definition_has_visible_live_stroke(
          definition->second, retained_definitions, {}, true, 1.0, {},
          DefinitionNodeRole::PatternRoot, &active_references)) {
      result.has_visible_live_stroke = true;
      break;
    }
  }

  for (const pugi::xml_node copy : materialized_root.children()) {
    std::ostringstream buffer;
    copy.print(buffer, "  ", pugi::format_default, pugi::encoding_utf8);
    result.elements.push_back(buffer.str());
  }
  return result;
}

namespace {

std::string render_path_entry_with_indent(
    const PathEntry& entry,
    const std::optional<std::string>& fill_override,
    const char* indent,
    bool opacity_is_on_wrapper) {
  std::ostringstream out;
  const PathEntryAnalysis analysis = analyze_path_entry(entry);

  if (entry.emit_fill) {
    out << indent << "<path d=\"" << xml_escape(entry.d) << "\"";
    if (!entry.transform.empty()) out << " transform=\"" << xml_escape(entry.transform) << "\"";
    out << " fill=\""
        << xml_escape(safe_serialized_paint(fill_override.value_or(analysis.fill_paint.value)))
        << "\"";
    if (!analysis.fill_rule.empty() && analysis.fill_rule != "nonzero") {
      out << " fill-rule=\"" << xml_escape(analysis.fill_rule) << "\"";
    }
    if (!opacity_is_on_wrapper && analysis.has_non_default_opacity) {
      out << " opacity=\"" << xml_escape(entry.opacity) << "\"";
    }
    if (analysis.has_non_default_fill_opacity) {
      out << " fill-opacity=\"" << xml_escape(entry.fill_opacity) << "\"";
    }
    out << "/>\n";
  }

  if (entry.emit_stroke) {
    out << indent << "<path d=\"" << xml_escape(entry.d) << "\"";
    if (!entry.transform.empty()) out << " transform=\"" << xml_escape(entry.transform) << "\"";
    out << " fill=\"none\"";
    out << " stroke=\""
        << xml_escape(safe_serialized_paint(fill_override.value_or(analysis.stroke_paint.value)))
        << "\"";
    out << " stroke-width=\"" << xml_escape(analysis.stroke_width) << "\"";
    if (!analysis.stroke_dasharray.empty() && lower_copy(analysis.stroke_dasharray) != "none") {
      out << " stroke-dasharray=\"" << xml_escape(analysis.stroke_dasharray) << "\"";
    }
    const std::string linecap = to_string(analysis.stroke_linecap);
    if (!linecap.empty() && linecap != "butt" && linecap != "undefined") {
      out << " stroke-linecap=\"" << xml_escape(linecap) << "\"";
    }
    const std::string linejoin = to_string(analysis.stroke_linejoin);
    if (!linejoin.empty() && linejoin != "miter" && linejoin != "undefined") {
      out << " stroke-linejoin=\"" << xml_escape(linejoin) << "\"";
    }
    if (!analysis.stroke_miterlimit.empty() && analysis.stroke_miterlimit != "4") {
      out << " stroke-miterlimit=\"" << xml_escape(analysis.stroke_miterlimit) << "\"";
    }
    if (!opacity_is_on_wrapper && analysis.has_non_default_opacity) {
      out << " opacity=\"" << xml_escape(entry.opacity) << "\"";
    }
    if (analysis.has_non_default_stroke_opacity) {
      out << " stroke-opacity=\"" << xml_escape(entry.stroke_opacity) << "\"";
    }
    out << "/>\n";
  }

  return out.str();
}

}  // namespace

std::string render_path_entry(const PathEntry& entry, const std::optional<std::string>& fill_override) {
  return render_path_entry_with_indent(entry, fill_override, "  ", false);
}

RenderedSvgDocument render_svg_document(
    const pugi::xml_node& svg_root,
    const std::vector<PathEntry>& paths,
    const std::optional<std::string>& fill_override) {
  std::ostringstream out;
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\"";
  const SerializedDefinitions serialized_defs = fill_override.has_value()
    ? SerializedDefinitions{}
    : collect_serialized_defs(svg_root, paths);
  if (!serialized_defs.elements.empty()) {
    out << " xmlns:xlink=\"http://www.w3.org/1999/xlink\"";
  }
  if (svg_root.attribute("width")) out << " width=\"" << xml_escape(svg_root.attribute("width").as_string()) << "\"";
  if (svg_root.attribute("height")) out << " height=\"" << xml_escape(svg_root.attribute("height").as_string()) << "\"";
  if (svg_root.attribute("preserveAspectRatio")) {
    out << " preserveAspectRatio=\""
        << xml_escape(svg_root.attribute("preserveAspectRatio").as_string()) << "\"";
  }
  for (const char* attr : {"id", "role", "aria-label", "aria-labelledby", "aria-describedby",
                           "aria-hidden", "focusable", "lang", "xml:lang"}) {
    if (svg_root.attribute(attr)) {
      out << " " << attr << "=\"" << xml_escape(svg_root.attribute(attr).as_string()) << "\"";
    }
  }
  if (svg_root.attribute("viewBox")) {
    out << " viewBox=\"" << xml_escape(svg_root.attribute("viewBox").as_string()) << "\"";
  }
  out << ">\n";

  for (const char* metadata_name : {"title", "desc"}) {
    const pugi::xml_node metadata = svg_root.child(metadata_name);
    if (metadata) {
      out << "  <" << metadata_name;
      for (const char* attr : {"id", "lang", "xml:lang"}) {
        if (metadata.attribute(attr)) {
          out << " " << attr << "=\"" << xml_escape(metadata.attribute(attr).as_string()) << "\"";
        }
      }
      out << ">" << xml_escape(metadata.text().as_string())
          << "</" << metadata_name << ">\n";
    }
  }

  if (!serialized_defs.elements.empty()) {
    out << "  <defs>\n";
    for (const std::string& def : serialized_defs.elements) {
      out << def;
    }
    out << "  </defs>\n";
  }

  std::size_t open_compositing_group = 0;
  for (const PathEntry& entry : paths) {
    if (entry.compositing_group != open_compositing_group) {
      if (open_compositing_group != 0) out << "  </g>\n";
      open_compositing_group = entry.compositing_group;
      if (open_compositing_group != 0) {
        out << "  <g opacity=\"" << xml_escape(entry.opacity) << "\">\n";
      }
    }
    out << render_path_entry_with_indent(
      entry,
      fill_override,
      open_compositing_group == 0 ? "  " : "    ",
      open_compositing_group != 0);
  }
  if (open_compositing_group != 0) out << "  </g>\n";

  out << "</svg>\n";
  return {out.str(), serialized_defs.has_visible_live_stroke};
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Unable to open input file: " + path.string());
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  if (in.bad()) {
    throw std::runtime_error("Unable to read input file completely: " + path.string());
  }
  return contents.str();
}

bool write_file_impl(const fs::path& path,
                     const std::string& text,
                     bool overwrite,
                     const ContainedOutputDestination* contained_destination) {
  if (path.empty() || path.filename().empty()) {
    throw std::runtime_error("Output path must name a file");
  }

  if (!contained_destination) {
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      fs::create_directories(parent, ec);
      if (ec) {
        throw std::runtime_error("Unable to create output directory: " + parent.string() +
                                 " (" + ec.message() + ")");
      }
    }
  }

  if (contained_destination) validate_contained_parent(*contained_destination);
  ExclusiveTemporaryFile temporary = create_exclusive_temporary_file(path);
  if (contained_destination) {
    validate_contained_parent(*contained_destination);
  }
  write_flush_and_close(temporary, path, text);
  if (contained_destination) {
    // Recheck the directory chain and owned temporary path immediately before
    // the path-based atomic install. This catches static links and observable
    // parent swaps without weakening the existing atomic replacement contract.
    validate_contained_parent(*contained_destination);
    validate_contained_temporary_file(*contained_destination, temporary.path);
  }
  if (overwrite) {
    replace_file(temporary, path);
  } else if (!install_file_without_replacement(temporary, path)) {
    return false;
  }
  return true;
}

bool write_file(const fs::path& path, const std::string& text, bool overwrite) {
  return write_file_impl(path, text, overwrite, nullptr);
}

bool write_file_contained(const fs::path& output_root,
                          const fs::path& relative_path,
                          const std::string& text,
                          bool overwrite) {
  const ContainedOutputDestination destination =
      prepare_contained_output_destination(output_root, relative_path);
  return write_file_impl(
      destination.path, text, overwrite, &destination);
}

}  // namespace svg_squisher
