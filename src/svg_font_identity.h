#pragma once

#include <memory>
#include <string>
#include <vector>

#include "svg_squisher.h"

namespace svg_squisher {

struct FontData {
  FontIdentity identity;
  std::shared_ptr<const std::vector<unsigned char>> bytes;
};

struct FontSnapshotState;

// Keeps every read of a font path within one conversion tied to the same owned
// byte buffer. This makes text measurement, shaping, and reported provenance a
// single snapshot even if the file is replaced while conversion is running.
class FontSnapshotScope {
public:
  FontSnapshotScope();
  ~FontSnapshotScope();

  FontSnapshotScope(const FontSnapshotScope&) = delete;
  FontSnapshotScope& operator=(const FontSnapshotScope&) = delete;

  std::vector<FontIdentity> identities() const;

private:
  std::unique_ptr<FontSnapshotState> state_;
  FontSnapshotState* previous_ = nullptr;
};

FontData load_font_data(const std::string& utf8_path);
FontIdentity identify_font_file(const std::string& utf8_path);

}  // namespace svg_squisher
