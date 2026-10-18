#include "aqo/disk_manager.hpp"

#include <array>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace aqo {

std::uint32_t crc32(const std::byte* data, std::size_t n) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) c = table[(c ^ static_cast<std::uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void sync_file(std::FILE* f) {
  if (std::fflush(f) != 0) throw StorageError("fflush failed");
#ifdef _WIN32
  if (_commit(_fileno(f)) != 0) throw StorageError("_commit failed");
#else
  if (fsync(fileno(f)) != 0) throw StorageError("fsync failed");
#endif
}

DiskManager::DiskManager(const std::string& path, FaultInjector* faults) : path_(path), faults_(faults) {
  file_ = std::fopen(path.c_str(), "r+b");
  if (file_ == nullptr) file_ = std::fopen(path.c_str(), "w+b");
  if (file_ == nullptr) throw StorageError("cannot open database file: " + path);
  std::fseek(file_, 0, SEEK_END);
  long len = std::ftell(file_);
  page_count_ = static_cast<page_id_t>(len / static_cast<long>(kPageSize));
}

DiskManager::~DiskManager() {
  if (file_ != nullptr) std::fclose(file_);
}

void DiskManager::read_page(page_id_t pid, std::byte* buf) {
  ++reads_;
  if (pid >= page_count_) {
    std::memset(buf, 0, kPageSize);
    return;
  }
  if (std::fseek(file_, static_cast<long>(pid) * static_cast<long>(kPageSize), SEEK_SET) != 0 ||
      std::fread(buf, 1, kPageSize, file_) != kPageSize)
    throw StorageError("short read of page " + std::to_string(pid));
  std::uint32_t stored = load<std::uint32_t>(buf + kCrcOff);
  std::byte copy[kPageSize];
  std::memcpy(copy, buf, kPageSize);
  store<std::uint32_t>(copy + kCrcOff, 0);
  bool all_zero = true;
  for (std::size_t i = 0; i < kPageSize && all_zero; ++i) all_zero = buf[i] == std::byte{0};
  if (!all_zero && crc32(copy, kPageSize) != stored) throw StorageError("checksum mismatch on page " + std::to_string(pid));
}

void DiskManager::write_page(page_id_t pid, const std::byte* buf) {
  if (faults_ != nullptr && faults_->should_crash()) throw SimulatedCrash();  // page writes are atomic in this model
  ++writes_;
  std::byte copy[kPageSize];
  std::memcpy(copy, buf, kPageSize);
  store<std::uint32_t>(copy + kCrcOff, 0);
  store<std::uint32_t>(copy + kCrcOff, crc32(copy, kPageSize));
  // Extend the file with zero pages if there is a gap, so page_count stays consistent.
  if (pid > page_count_) {
    std::vector<std::byte> zeros(kPageSize);
    std::fseek(file_, static_cast<long>(page_count_) * static_cast<long>(kPageSize), SEEK_SET);
    for (page_id_t p = page_count_; p < pid; ++p) std::fwrite(zeros.data(), 1, kPageSize, file_);
  }
  if (std::fseek(file_, static_cast<long>(pid) * static_cast<long>(kPageSize), SEEK_SET) != 0 ||
      std::fwrite(copy, 1, kPageSize, file_) != kPageSize)
    throw StorageError("write failed for page " + std::to_string(pid));
  if (pid >= page_count_) page_count_ = pid + 1;
}

void DiskManager::sync() {
  if (faults_ != nullptr) {
    if (faults_->should_crash()) throw SimulatedCrash();
    return;  // tests with an injector skip the real fsync
  }
  sync_file(file_);
}

}  // namespace aqo
