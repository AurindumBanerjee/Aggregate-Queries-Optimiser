// Slotted-page record layout.
//
//   0..16   common page header (LSN, CRC, type)
//  16  u16  slot count
//  18  u16  free_end: records occupy [free_end, kPageSize), growing downward
//  20  u16  garbage: bytes inside the record area that belong to deleted/shrunk records
//  24  u32  aux1   (leaf: next leaf   | internal: leftmost child)
//  28  u32  aux2   (leaf: prev leaf   | internal: unused)
//  32..     slot directory, 4 bytes per slot {u16 offset, u16 length}, growing upward
//
// The slot directory is kept in *logical order* (key order for B+ tree nodes), so inserting or
// removing at index i shifts directory entries but never moves record bytes. Free space is
// reclaimed lazily by compact(), which runs automatically when an insert needs it.
#pragma once

#include <span>

#include "aqo/page.hpp"

namespace aqo {

class SlottedPage {
 public:
  static constexpr std::size_t kHeaderEnd = 32;
  static constexpr std::size_t kSlotSize = 4;
  static constexpr std::size_t kCapacity = kPageSize - kHeaderEnd;  // bytes usable for slots + records

  explicit SlottedPage(std::byte* page) : p_(page) {}

  /// Formats an empty page of the given type (keeps the LSN field untouched).
  void init(PageType type);

  [[nodiscard]] std::size_t count() const { return load<std::uint16_t>(p_ + 16); }
  [[nodiscard]] page_id_t aux1() const { return load<std::uint32_t>(p_ + 24); }
  [[nodiscard]] page_id_t aux2() const { return load<std::uint32_t>(p_ + 28); }
  void set_aux1(page_id_t v) { store<std::uint32_t>(p_ + 24, v); }
  void set_aux2(page_id_t v) { store<std::uint32_t>(p_ + 28, v); }

  /// Bytes available for a new record *including* its slot, after compaction.
  [[nodiscard]] std::size_t free_space() const;
  /// Bytes consumed by live records and slots.
  [[nodiscard]] std::size_t used_space() const { return kCapacity - free_space(); }
  [[nodiscard]] bool fits(std::size_t record_len) const { return free_space() >= record_len + kSlotSize; }

  [[nodiscard]] std::span<const std::byte> get(std::size_t idx) const;
  [[nodiscard]] std::span<std::byte> get_mutable(std::size_t idx);

  /// Inserts a record so that it becomes slot `idx` (0 <= idx <= count). Returns false if it does not fit.
  bool insert_at(std::size_t idx, std::span<const std::byte> record);
  /// Removes slot `idx`, shifting later slots down.
  void remove_at(std::size_t idx);
  /// Replaces slot `idx` in place if the new record is not larger, else relocates it. False if no room.
  bool replace_at(std::size_t idx, std::span<const std::byte> record);
  /// Defragments the record area.
  void compact();
  /// Removes all slots.
  void clear();

  /// Structural self-check; returns "" when consistent.
  [[nodiscard]] std::string validate() const;

 private:
  [[nodiscard]] std::size_t free_end() const { return load<std::uint16_t>(p_ + 18); }
  [[nodiscard]] std::size_t garbage() const { return load<std::uint16_t>(p_ + 20); }
  void set_count(std::size_t v) { store<std::uint16_t>(p_ + 16, static_cast<std::uint16_t>(v)); }
  void set_free_end(std::size_t v) { store<std::uint16_t>(p_ + 18, static_cast<std::uint16_t>(v)); }
  void set_garbage(std::size_t v) { store<std::uint16_t>(p_ + 20, static_cast<std::uint16_t>(v)); }
  [[nodiscard]] std::byte* slot(std::size_t i) const { return p_ + kHeaderEnd + i * kSlotSize; }
  [[nodiscard]] std::size_t slot_off(std::size_t i) const { return load<std::uint16_t>(slot(i)); }
  [[nodiscard]] std::size_t slot_len(std::size_t i) const { return load<std::uint16_t>(slot(i) + 2); }
  void set_slot(std::size_t i, std::size_t off, std::size_t len) {
    store<std::uint16_t>(slot(i), static_cast<std::uint16_t>(off));
    store<std::uint16_t>(slot(i) + 2, static_cast<std::uint16_t>(len));
  }
  [[nodiscard]] std::size_t contiguous_free() const { return free_end() - (kHeaderEnd + count() * kSlotSize); }

  std::byte* p_;
};

}  // namespace aqo
