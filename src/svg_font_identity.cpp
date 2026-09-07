#include "svg_font_identity.h"

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace svg_squisher {
namespace {

constexpr std::array<std::uint32_t, 64> sha256_round_constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::uint32_t rotate_right(std::uint32_t value, unsigned int bits) {
  return (value >> bits) | (value << (32U - bits));
}

class Sha256 {
public:
  void update(const unsigned char* data, std::size_t length) {
    total_bytes_ += static_cast<std::uint64_t>(length);
    while (length > 0) {
      const std::size_t available = block_.size() - block_size_;
      const std::size_t count = length < available ? length : available;
      for (std::size_t index = 0; index < count; ++index) {
        block_[block_size_ + index] = data[index];
      }
      block_size_ += count;
      data += count;
      length -= count;
      if (block_size_ == block_.size()) {
        transform(block_.data());
        block_size_ = 0;
      }
    }
  }

  std::string finish() {
    const std::uint64_t bit_length = total_bytes_ * 8U;
    block_[block_size_++] = 0x80U;

    if (block_size_ > 56) {
      while (block_size_ < block_.size()) block_[block_size_++] = 0;
      transform(block_.data());
      block_size_ = 0;
    }
    while (block_size_ < 56) block_[block_size_++] = 0;
    for (std::size_t index = 0; index < 8; ++index) {
      block_[63 - index] =
          static_cast<unsigned char>((bit_length >> (index * 8U)) & 0xffU);
    }
    transform(block_.data());

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(64);
    for (const std::uint32_t word : state_) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        digest.push_back(hex_digits[(word >> static_cast<unsigned int>(shift)) & 0x0fU]);
      }
    }
    return digest;
  }

private:
  void transform(const unsigned char* block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const std::size_t offset = index * 4;
      words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                     (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                     (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                     static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const std::uint32_t previous_15 = words[index - 15];
      const std::uint32_t previous_2 = words[index - 2];
      const std::uint32_t sigma0 = rotate_right(previous_15, 7U) ^
                                   rotate_right(previous_15, 18U) ^
                                   (previous_15 >> 3U);
      const std::uint32_t sigma1 = rotate_right(previous_2, 17U) ^
                                   rotate_right(previous_2, 19U) ^
                                   (previous_2 >> 10U);
      words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t big_sigma1 =
          rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 =
          h + big_sigma1 + choose + sha256_round_constants[index] + words[index];
      const std::uint32_t big_sigma0 =
          rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = big_sigma0 + majority;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<unsigned char, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t total_bytes_ = 0;
};

struct FileSnapshot {
  std::uintmax_t bytes = 0;
  fs::file_time_type modified;
};

struct CachedIdentity {
  FileSnapshot snapshot;
  std::string sha256;
};

struct StreamedIdentity {
  std::uintmax_t bytes = 0;
  std::string sha256;
  std::string error;
};

std::mutex identity_cache_mutex;
std::unordered_map<std::string, CachedIdentity> identity_cache;
constexpr std::size_t max_cached_identities = 128;

bool same_snapshot(const FileSnapshot& left, const FileSnapshot& right) {
  return left.bytes == right.bytes && left.modified == right.modified;
}

std::optional<FileSnapshot> snapshot_file(const fs::path& path,
                                          const std::string& display_path,
                                          std::string& error) {
  std::error_code status_error;
  const fs::file_status status = fs::status(path, status_error);
  if (status_error) {
    error = "Could not inspect font file '" + display_path + "': " + status_error.message();
    return std::nullopt;
  }
  if (!fs::exists(status)) {
    error = "Font file does not exist: '" + display_path + "'";
    return std::nullopt;
  }
  if (!fs::is_regular_file(status)) {
    error = "Font path is not a regular file: '" + display_path + "'";
    return std::nullopt;
  }

  std::error_code size_error;
  const std::uintmax_t bytes = fs::file_size(path, size_error);
  if (size_error) {
    error = "Could not read font file size for '" + display_path + "': " +
            size_error.message();
    return std::nullopt;
  }

  std::error_code time_error;
  const fs::file_time_type modified = fs::last_write_time(path, time_error);
  if (time_error) {
    error = "Could not read font modification time for '" + display_path + "': " +
            time_error.message();
    return std::nullopt;
  }
  return FileSnapshot{bytes, modified};
}

std::optional<std::string> find_cached_identity(const std::string& key,
                                                const FileSnapshot& snapshot) {
  const std::lock_guard<std::mutex> lock(identity_cache_mutex);
  const auto cached = identity_cache.find(key);
  if (cached == identity_cache.end() ||
      !same_snapshot(cached->second.snapshot, snapshot)) {
    return std::nullopt;
  }
  return cached->second.sha256;
}

void cache_identity(const std::string& key,
                    const FileSnapshot& snapshot,
                    const std::string& sha256) {
  const std::lock_guard<std::mutex> lock(identity_cache_mutex);
  if (identity_cache.find(key) == identity_cache.end() &&
      identity_cache.size() >= max_cached_identities) {
    identity_cache.erase(identity_cache.begin());
  }
  identity_cache[key] = CachedIdentity{snapshot, sha256};
}

StreamedIdentity hash_file(const fs::path& path, const std::string& display_path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return StreamedIdentity{0, {}, "Could not open font file for hashing: '" +
                                       display_path + "'"};
  }

  Sha256 hash;
  std::array<char, 64 * 1024> buffer{};
  std::uintmax_t bytes = 0;
  while (true) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      const auto unsigned_count = static_cast<std::uintmax_t>(count);
      if (bytes > std::numeric_limits<std::uintmax_t>::max() - unsigned_count) {
        return StreamedIdentity{0, {}, "Font file is too large to count safely: '" +
                                           display_path + "'"};
      }
      bytes += unsigned_count;
      hash.update(reinterpret_cast<const unsigned char*>(buffer.data()),
                  static_cast<std::size_t>(count));
    }
    if (input.eof()) break;
    if (!input) {
      return StreamedIdentity{0, {}, "Could not finish reading font file: '" +
                                         display_path + "'"};
    }
  }
  return StreamedIdentity{bytes, hash.finish(), {}};
}

bool normalize_font_path(const std::string& utf8_path,
                         fs::path& normalized,
                         std::string& cache_key,
                         std::string& error) {
  if (utf8_path.empty()) {
    error = "Font path is empty";
    return false;
  }
  try {
    const fs::path decoded = fs::u8path(utf8_path);
    std::error_code absolute_error;
    const fs::path absolute = fs::absolute(decoded, absolute_error);
    if (absolute_error) {
      error = "Could not resolve font path '" + utf8_path + "': " +
              absolute_error.message();
      return false;
    }
    normalized = absolute.lexically_normal();
    cache_key = normalized.generic_u8string();
    return true;
  } catch (const std::exception& exception) {
    error = "Could not interpret font path '" + utf8_path + "': " + exception.what();
    return false;
  }
}

}  // namespace

FontIdentity identify_font_file(const std::string& utf8_path) {
  FontIdentity result;
  result.path = utf8_path;

  fs::path normalized;
  std::string cache_key;
  if (!normalize_font_path(utf8_path, normalized, cache_key, result.error)) return result;

  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string snapshot_error;
    const std::optional<FileSnapshot> before =
        snapshot_file(normalized, utf8_path, snapshot_error);
    if (!before) {
      result.error = std::move(snapshot_error);
      return result;
    }

    const std::optional<std::string> cached = find_cached_identity(cache_key, *before);
    if (cached) {
      const std::optional<FileSnapshot> after =
          snapshot_file(normalized, utf8_path, snapshot_error);
      if (!after) {
        if (attempt == 0) continue;
        result.error = std::move(snapshot_error);
        return result;
      }
      if (same_snapshot(*before, *after)) {
        result.bytes = before->bytes;
        result.sha256 = *cached;
        return result;
      }
    } else {
      const StreamedIdentity streamed = hash_file(normalized, utf8_path);
      if (!streamed.error.empty()) {
        if (attempt == 0) continue;
        result.error = streamed.error;
        return result;
      }

      const std::optional<FileSnapshot> after =
          snapshot_file(normalized, utf8_path, snapshot_error);
      if (!after) {
        if (attempt == 0) continue;
        result.error = std::move(snapshot_error);
        return result;
      }
      if (streamed.bytes == before->bytes && same_snapshot(*before, *after)) {
        cache_identity(cache_key, *after, streamed.sha256);
        result.bytes = streamed.bytes;
        result.sha256 = streamed.sha256;
        return result;
      }
    }

    if (attempt == 1) {
      result.error = "Font file changed while its identity was being computed: '" +
                     utf8_path + "'";
      return result;
    }
  }

  result.error = "Could not compute font identity: '" + utf8_path + "'";
  return result;
}

}  // namespace svg_squisher
