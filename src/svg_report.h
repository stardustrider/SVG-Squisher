#pragma once

#include <filesystem>
#include <string>

#include "svg_squisher.h"

namespace svg_squisher {

std::string render_json_report(const BatchResult& result, const Options& options);
void write_json_report(const std::filesystem::path& path,
                       const BatchResult& result,
                       const Options& options);
BatchResult batch_result_from_file(const FileConversionResult& result);

}  // namespace svg_squisher
