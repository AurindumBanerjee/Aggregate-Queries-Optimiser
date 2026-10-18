#include "aqo/slotted_page.hpp"

#include <algorithm>
#include <vector>

namespace aqo {

void SlottedPage::init(PageType type) {
  lsn_t lsn = page_lsn(p_);
  std::memset(p_, 0, kPageSize);
  set_page_lsn(p_, lsn);
  set_page_type(p_, type);
  set_count(0);
  set_free_end(kPageSize);
  set_garbage(0);
  set_aux1(kInvalidPage);
  set_aux2(kInvalidPage);
}

std::size_t SlottedPage::free_space() const {
  return contiguous_free() + garbage();
}

std::span<const std::byte> SlottedPage::get(std::size_t idx) const {
  return {p_ + slot_off(idx), slot_len(idx)};
}
std::span<std::byte> SlottedPage::get_mutable(std::size_t idx) {
  return {p_ + slot_off(idx), slot_len(idx)};
}

void SlottedPage::compact() {
  std::size_t n = count();
  std::size_t end = kPageSize;
  std::vector<std::byte> scratch(kPageSize);
  // Copy live records, in slot order, to the end of a scratch page, then copy back.
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t len = slot_len(i);
    end -= len;
    std::memcpy(scratch.data() + end, p_ + slot_off(i), len);
    set_slot(i, end, len);
  }
  std::memcpy(p_ + end, scratch.data() + end, kPageSize - end);
  set_free_end(end);
  set_garbage(0);
}

bool SlottedPage::insert_at(std::size_t idx, std::span<const std::byte> record) {
  std::size_t n = count();
  if (idx > n) throw StorageError("slot index out of range");
  if (!fits(record.size())) return false;
  if (contiguous_free() < record.size() + kSlotSize) compact();
  std::size_t off = free_end() - record.size();
  std::memcpy(p_ + off, record.data(), record.size());
  std::memmove(slot(idx + 1), slot(idx), (n - idx) * kSlotSize);
  set_slot(idx, off, record.size());
  set_count(n + 1);
  set_free_end(off);
  return true;
}

void SlottedPage::remove_at(std::size_t idx) {
  std::size_t n = count();
  if (idx >= n) throw StorageError("slot index out of range");
  std::size_t off = slot_off(idx), len = slot_len(idx);
  if (off == free_end()) {
    set_free_end(off + len);  // freed the lowest record: reclaim immediately
  } else {
    set_garbage(garbage() + len);
  }
  std::memmove(slot(idx), slot(idx + 1), (n - idx - 1) * kSlotSize);
  set_count(n - 1);
}

bool SlottedPage::replace_at(std::size_t idx, std::span<const std::byte> record) {
  std::size_t old_len = slot_len(idx);
  if (record.size() <= old_len) {
    std::memcpy(p_ + slot_off(idx), record.data(), record.size());
    set_garbage(garbage() + (old_len - record.size()));
    set_slot(idx, slot_off(idx), record.size());
    return true;
  }
  if (free_space() + old_len < record.size()) return false;  // slot is reused, so no extra slot bytes
  remove_at(idx);
  bool ok = insert_at(idx, record);
  return ok;
}

void SlottedPage::clear() {
  set_count(0);
  set_free_end(kPageSize);
  set_garbage(0);
}

std::string SlottedPage::validate() const {
  std::size_t n = count();
  std::size_t dir_end = kHeaderEnd + n * kSlotSize;
  if (dir_end > free_end() || free_end() > kPageSize) return "slot directory overlaps record area";
  std::size_t live = 0;
  std::vector<std::pair<std::size_t, std::size_t>> spans;
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t off = slot_off(i), len = slot_len(i);
    if (off < free_end() || off + len > kPageSize) return "record out of bounds";
    live += len;
    spans.emplace_back(off, off + len);
  }
  std::sort(spans.begin(), spans.end());
  for (std::size_t i = 1; i < spans.size(); ++i)
    if (spans[i].first < spans[i - 1].second) return "records overlap";
  if (kPageSize - free_end() != live + garbage()) return "garbage accounting mismatch";
  return "";
}

}  // namespace aqo
