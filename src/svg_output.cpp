#include "svg_output.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <deque>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
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

#include "svg_entry.h"
#include "svg_dom.h"
#include "svg_geometry.h"
#include "svg_paint.h"
#include "svg_path.h"
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
  }

  ~ExclusiveTemporaryFile() {
#if defined(_WIN32)
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
    if (descriptor >= 0) ::close(descriptor);
#endif
  }
};

#if !defined(_WIN32)
std::string native_error_message(int error) {
  return std::error_code(error, std::generic_category()).message();
}
#endif

ExclusiveTemporaryFile create_exclusive_temporary_file(const fs::path& path) {
  const fs::path parent = path.parent_path();
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto suffix = temporary_file_counter.fetch_add(1, std::memory_order_relaxed);
    fs::path candidate_name(".");
    candidate_name += path.filename().native();
    candidate_name +=
        fs::path(".svg-squisher-" + std::to_string(suffix) + ".tmp").native();
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
  HANDLE handle = temporary.handle;
  temporary.handle = INVALID_HANDLE_VALUE;
  if (!CloseHandle(handle)) {
    const unsigned long error = GetLastError();
    throw std::runtime_error("Unable to close output file: " + output_path.string() +
                             " (Windows error " + std::to_string(error) + ")");
  }
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

void replace_file(const fs::path& temporary_path, const fs::path& output_path) {
#if defined(_WIN32)
  if (!MoveFileExW(temporary_path.c_str(),
                   output_path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const unsigned long error = GetLastError();
    std::error_code ignored;
    fs::remove(temporary_path, ignored);
    throw std::runtime_error("Unable to replace output file: " + output_path.string() +
                             " (Windows error " + std::to_string(error) + ")");
  }
#else
  std::error_code ec;
  fs::rename(temporary_path, output_path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary_path, ignored);
    throw std::runtime_error("Unable to replace output file: " + output_path.string() +
                             " (" + ec.message() + ")");
  }
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

std::string resource_url_target(const std::string& value,
                                std::size_t open,
                                std::size_t close) {
  std::string target = trim(value.substr(open + 4, close - open - 4));
  if (target.size() >= 2 &&
      ((target.front() == '\'' && target.back() == '\'') ||
       (target.front() == '"' && target.back() == '"'))) {
    target = trim(target.substr(1, target.size() - 2));
  }
  return target;
}

bool is_local_fragment_reference(const std::string& value) {
  const std::string target = trim(value);
  return target.size() > 1 && target.front() == '#';
}

bool has_external_resource_url(const std::string& value) {
  const std::string lowered = lower_copy(value);
  std::size_t cursor = 0;
  while ((cursor = lowered.find("url(", cursor)) != std::string::npos) {
    const std::size_t close = lowered.find(')', cursor + 4);
    if (close == std::string::npos) return false;
    const std::string target = resource_url_target(value, cursor, close);
    if (!target.empty() && !is_local_fragment_reference(target)) return true;
    cursor = close + 1;
  }
  return false;
}

void collect_reference_ids_from_value(const std::string& value,
                                      std::unordered_set<std::string>& referenced_ids) {
  const std::string lowered = lower_copy(value);
  std::size_t cursor = 0;
  while ((cursor = lowered.find("url(", cursor)) != std::string::npos) {
    const std::size_t close = lowered.find(')', cursor + 4);
    if (close == std::string::npos) break;
    const std::string target = resource_url_target(value, cursor, close);
    if (is_local_fragment_reference(target)) referenced_ids.insert(target.substr(1));
    cursor = close == std::string::npos ? value.size() : close + 1;
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
         has_external_resource_url(value);
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
  return has_external_resource_url(paint) ? "none" : paint;
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

void set_attribute(pugi::xml_node& node, const char* name, const std::string& value) {
  pugi::xml_attribute attribute = node.attribute(name);
  if (!attribute) attribute = node.append_attribute(name);
  attribute.set_value(value.c_str());
}

void materialize_definition_styles(const pugi::xml_node& source,
                                   pugi::xml_node destination,
                                   const std::vector<CssRule>& rules,
                                   const StyleState& inherited) {
  if (source.type() != pugi::node_element || destination.type() != pugi::node_element) return;
  const StyleState style = resolve_style(source, rules, inherited);
  const std::string name = source.name();
  const bool presentation_container =
    name != "linearGradient" && name != "radialGradient" && name != "stop" &&
    name != "filter" && !(name.size() > 2 && name[0] == 'f' && name[1] == 'e');
  if (presentation_container) {
    set_attribute(destination, "fill", style.fill);
    set_attribute(destination, "fill-opacity", style.fill_opacity);
    set_attribute(destination, "fill-rule", style.fill_rule);
    set_attribute(destination, "stroke", style.stroke);
    set_attribute(destination, "stroke-opacity", style.stroke_opacity);
    set_attribute(destination, "stroke-width", style.stroke_width);
    if (!style.stroke_dasharray.empty()) {
      set_attribute(destination, "stroke-dasharray", style.stroke_dasharray);
    }
    set_attribute(destination, "stroke-linecap", style.stroke_linecap);
    set_attribute(destination, "stroke-linejoin", style.stroke_linejoin);
    set_attribute(destination, "stroke-miterlimit", style.stroke_miterlimit);
    set_attribute(destination, "opacity", style.opacity);
    set_attribute(destination, "display", style.display);
    set_attribute(destination, "visibility", style.visibility);
    std::string materialized_style = destination.attribute("style").as_string();
    if (!materialized_style.empty() && materialized_style.back() != ';') {
      materialized_style += ';';
    }
    const auto append_style = [&](const char* property, const std::string& value) {
      materialized_style += property;
      materialized_style += ':';
      materialized_style += value;
      materialized_style += " !important;";
    };
    append_style("fill", style.fill);
    append_style("fill-opacity", style.fill_opacity);
    append_style("fill-rule", style.fill_rule);
    append_style("stroke", style.stroke);
    append_style("stroke-opacity", style.stroke_opacity);
    append_style("stroke-width", style.stroke_width);
    if (!style.stroke_dasharray.empty()) append_style("stroke-dasharray", style.stroke_dasharray);
    append_style("stroke-linecap", style.stroke_linecap);
    append_style("stroke-linejoin", style.stroke_linejoin);
    append_style("stroke-miterlimit", style.stroke_miterlimit);
    append_style("opacity", style.opacity);
    append_style("display", style.display);
    append_style("visibility", style.visibility);
    set_attribute(destination, "style", materialized_style);
  }

  pugi::xml_node destination_child = destination.first_child();
  for (const pugi::xml_node source_child : source.children()) {
    while (destination_child && destination_child.type() != source_child.type()) {
      destination_child = destination_child.next_sibling();
    }
    if (!destination_child) break;
    if (source_child.type() == pugi::node_element) {
      materialize_definition_styles(source_child, destination_child, rules, style);
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

std::vector<std::string> collect_serialized_defs(const pugi::xml_node& svg_root,
                                                 const std::vector<PathEntry>& paths) {
  std::unordered_set<std::string> referenced_ids;
  collect_referenced_def_ids(paths, referenced_ids);
  if (referenced_ids.empty()) return {};

  std::deque<std::string> pending(referenced_ids.begin(), referenced_ids.end());
  while (!pending.empty()) {
    const std::string id = pending.front();
    pending.pop_front();
    const auto definition = find_by_id(svg_root, id);
    if (!definition) continue;

    const std::size_t previous_size = referenced_ids.size();
    collect_definition_dependencies(*definition, referenced_ids);
    if (referenced_ids.size() == previous_size) continue;
    for (const std::string& candidate : referenced_ids) {
      if (candidate != id) pending.push_back(candidate);
    }
  }

  std::vector<std::string> serialized_defs;
  const std::vector<CssRule> rules = parse_css_rules(svg_root);
  for (const pugi::xpath_node& match : svg_root.select_nodes(".//*[@id]")) {
    const pugi::xml_node definition = match.node();
    if (referenced_ids.find(definition.attribute("id").as_string()) == referenced_ids.end()) {
      continue;
    }
    if (!definition_element_is_safe(definition.name())) continue;
    pugi::xml_document materialized;
    pugi::xml_node copy = materialized.append_copy(definition);
    materialize_definition_styles(
      definition, copy, rules, inherited_style_for_definition(definition, rules));
    sanitize_definition_subtree(copy);
    std::ostringstream buffer;
    copy.print(buffer, "  ", pugi::format_default, pugi::encoding_utf8);
    serialized_defs.push_back(buffer.str());
  }
  return serialized_defs;
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

std::string render_svg_document(const pugi::xml_node& svg_root,
                                const std::vector<PathEntry>& paths,
                                const std::optional<std::string>& fill_override) {
  std::ostringstream out;
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\"";
  const std::vector<std::string> serialized_defs = fill_override.has_value()
    ? std::vector<std::string>{}
    : collect_serialized_defs(svg_root, paths);
  if (!serialized_defs.empty()) {
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

  if (!serialized_defs.empty()) {
    out << "  <defs>\n";
    for (const std::string& def : serialized_defs) {
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
  return out.str();
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

void write_file(const fs::path& path, const std::string& text) {
  if (path.empty() || path.filename().empty()) {
    throw std::runtime_error("Output path must name a file");
  }

  const fs::path parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
      throw std::runtime_error("Unable to create output directory: " + parent.string() +
                               " (" + ec.message() + ")");
    }
  }

  ExclusiveTemporaryFile temporary = create_exclusive_temporary_file(path);
  try {
    write_flush_and_close(temporary, path, text);
    replace_file(temporary.path, path);
  } catch (...) {
    std::error_code ignored;
    fs::remove(temporary.path, ignored);
    throw;
  }
}

}  // namespace svg_squisher
