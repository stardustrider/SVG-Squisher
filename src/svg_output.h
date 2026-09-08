#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "svg_squisher.h"

namespace svg_squisher {

std::string xml_escape(const std::string& value);

struct SerializedDefinitions {
  std::vector<std::string> elements;
  bool has_visible_live_stroke = false;
};

SerializedDefinitions collect_serialized_defs(const pugi::xml_node& svg_root,
                                              const std::vector<PathEntry>& paths);

std::string render_path_entry(const PathEntry& entry, const std::optional<std::string>& fill_override);

struct RenderedSvgDocument {
  std::string svg;
  bool retained_definition_has_visible_live_stroke = false;
};

RenderedSvgDocument render_svg_document(
    const pugi::xml_node& svg_root,
    const std::vector<PathEntry>& paths,
    const std::optional<std::string>& fill_override);

std::string read_file(const std::filesystem::path& path);

// Returns false only when overwrite is false and another file already owns the
// destination at the atomic install point.
bool write_file(const std::filesystem::path& path,
                const std::string& text,
                bool overwrite = true);

// Writes a relative destination beneath output_root. The root itself may
// resolve through a user-selected symlink or reparse point, but every existing
// component below that resolved root must be an ordinary directory.
bool write_file_contained(const std::filesystem::path& output_root,
                          const std::filesystem::path& relative_path,
                          const std::string& text,
                          bool overwrite = true);

}  // namespace svg_squisher
