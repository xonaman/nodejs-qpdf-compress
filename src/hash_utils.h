// FNV-1a hash utility — shared between image deduplication and stream
// deduplication
#pragma once

#include <cstddef>
#include <cstdint>

inline uint64_t fnv1aHash(const uint8_t *data, size_t size) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}
