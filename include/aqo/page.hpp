// On-disk page constants and the common page header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace aqo {

using page_id_t = std::uint32_t;
using lsn_t = std::uint64_t;

inline constexpr std::size_t kPageSize = 4096;
inline constexpr page_id_t kInvalidPage = 0xFFFFFFFFu;
inline constexpr page_id_t kMetaPage = 0;

enum class PageType : std::uint8_t { kFree = 0, kMeta = 1, kLeaf = 2, kInternal = 3 };

// Common header, bytes [0, 16) of every page:
//   0  u64 page LSN (last WAL record applied to this page)
//   8  u32 CRC32 of the page with this field zeroed (set by DiskManager on write)
//  12  u8  PageType
inline constexpr std::size_t kLsnOff = 0;
inline constexpr std::size_t kCrcOff = 8;
inline constexpr std::size_t kTypeOff = 12;
inline constexpr std::size_t kCommonHeader = 16;

template <class T>
inline T load(const std::byte* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}
template <class T>
inline void store(std::byte* p, T v) {
  std::memcpy(p, &v, sizeof(T));
}

inline lsn_t page_lsn(const std::byte* page) { return load<lsn_t>(page + kLsnOff); }
inline void set_page_lsn(std::byte* page, lsn_t lsn) { store<lsn_t>(page + kLsnOff, lsn); }
inline PageType page_type(const std::byte* page) { return static_cast<PageType>(load<std::uint8_t>(page + kTypeOff)); }
inline void set_page_type(std::byte* page, PageType t) { store<std::uint8_t>(page + kTypeOff, static_cast<std::uint8_t>(t)); }

struct StorageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::uint32_t crc32(const std::byte* data, std::size_t n);

}  // namespace aqo
