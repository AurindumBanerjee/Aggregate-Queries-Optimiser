#include "aqo/wal.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "aqo/disk_manager.hpp"

namespace aqo {
namespace {

constexpr std::uint64_t kWalMagic = 0x4c41575f4f514121ULL;
constexpr std::uint32_t kMaxPayload = 64u << 20;

struct Writer {
  std::vector<std::byte> out;
  template <class T>
  void put(T v) {
    std::byte b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.insert(out.end(), b, b + sizeof(T));
  }
  void bytes(const std::vector<std::byte>& v) { out.insert(out.end(), v.begin(), v.end()); }
};

struct Reader {
  const std::byte* p;
  std::size_t left;
  bool ok = true;
  template <class T>
  T get() {
    if (left < sizeof(T)) {
      ok = false;
      return T{};
    }
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    left -= sizeof(T);
    return v;
  }
  std::vector<std::byte> bytes(std::size_t n) {
    if (left < n) {
      ok = false;
      return {};
    }
    std::vector<std::byte> v(p, p + n);
    p += n;
    left -= n;
    return v;
  }
};

}  // namespace

std::vector<std::byte> Wal::encode(const LogRecord& r) {
  Writer w;
  w.put<std::uint8_t>(static_cast<std::uint8_t>(r.type));
  w.put<std::uint64_t>(r.txn);
  w.put<std::uint64_t>(r.prev_lsn);
  switch (r.type) {
    case LogType::kUpdate:
    case LogType::kClr:
      w.put<std::uint32_t>(r.page);
      w.put<std::uint16_t>(static_cast<std::uint16_t>(r.segments.size()));
      for (const auto& s : r.segments) {
        w.put<std::uint16_t>(s.offset);
        w.put<std::uint16_t>(static_cast<std::uint16_t>(s.after.size()));
        if (r.type == LogType::kUpdate) w.bytes(s.before);
        w.bytes(s.after);
      }
      if (r.type == LogType::kClr) w.put<std::uint64_t>(r.undo_next);
      break;
    case LogType::kCheckpoint:
      w.put<std::uint32_t>(static_cast<std::uint32_t>(r.active_txns.size()));
      for (auto& [t, l] : r.active_txns) {
        w.put<std::uint64_t>(t);
        w.put<std::uint64_t>(l);
      }
      w.put<std::uint32_t>(static_cast<std::uint32_t>(r.dirty_pages.size()));
      for (auto& [p, l] : r.dirty_pages) {
        w.put<std::uint32_t>(p);
        w.put<std::uint64_t>(l);
      }
      break;
    default:
      break;
  }
  return std::move(w.out);
}

bool Wal::decode(const std::byte* payload, std::size_t len, LogRecord& r) {
  Reader rd{payload, len};
  r = LogRecord{};
  auto t = rd.get<std::uint8_t>();
  if (t < 1 || t > 6) return false;
  r.type = static_cast<LogType>(t);
  r.txn = rd.get<std::uint64_t>();
  r.prev_lsn = rd.get<std::uint64_t>();
  switch (r.type) {
    case LogType::kUpdate:
    case LogType::kClr: {
      r.page = rd.get<std::uint32_t>();
      auto n = rd.get<std::uint16_t>();
      for (std::uint16_t i = 0; i < n && rd.ok; ++i) {
        Segment s;
        s.offset = rd.get<std::uint16_t>();
        auto l = rd.get<std::uint16_t>();
        if (r.type == LogType::kUpdate) s.before = rd.bytes(l);
        s.after = rd.bytes(l);
        r.segments.push_back(std::move(s));
      }
      if (r.type == LogType::kClr) r.undo_next = rd.get<std::uint64_t>();
      break;
    }
    case LogType::kCheckpoint: {
      auto n = rd.get<std::uint32_t>();
      for (std::uint32_t i = 0; i < n && rd.ok; ++i) {
        auto tx = rd.get<std::uint64_t>();
        auto l = rd.get<std::uint64_t>();
        r.active_txns.emplace_back(tx, l);
      }
      auto m = rd.get<std::uint32_t>();
      for (std::uint32_t i = 0; i < m && rd.ok; ++i) {
        auto p = rd.get<std::uint32_t>();
        auto l = rd.get<std::uint64_t>();
        r.dirty_pages.emplace_back(p, l);
      }
      break;
    }
    default:
      break;
  }
  return rd.ok && rd.left == 0;
}

void Wal::write_header(std::FILE* f) const {
  std::byte h[kHeaderSize] = {};
  store<std::uint64_t>(h, kWalMagic);
  store<std::uint64_t>(h + 8, base_lsn_);
  store<std::uint64_t>(h + 16, checkpoint_lsn_);
  store<std::uint32_t>(h + 24, crc32(h, 24));
  std::fseek(f, 0, SEEK_SET);
  if (std::fwrite(h, 1, kHeaderSize, f) != kHeaderSize) throw StorageError("cannot write WAL header");
}

Wal::Wal(const std::string& path, bool sync_on_flush, FaultInjector* faults)
    : path_(path), sync_on_flush_(sync_on_flush), faults_(faults) {
  std::error_code ec;
  bool exists = std::filesystem::exists(path_, ec) && std::filesystem::file_size(path_, ec) >= kHeaderSize;
  if (!exists) {
    std::FILE* f = std::fopen(path_.c_str(), "wb");
    if (f == nullptr) throw StorageError("cannot create WAL " + path_);
    write_header(f);
    sync_file(f);
    std::fclose(f);
    file_end_ = synced_size_ = kHeaderSize;
    open_file();
    return;
  }
  // Existing log: read header, find the end of the valid prefix, drop any torn tail.
  std::FILE* f = std::fopen(path_.c_str(), "rb");
  if (f == nullptr) throw StorageError("cannot open WAL " + path_);
  std::byte h[kHeaderSize];
  if (std::fread(h, 1, kHeaderSize, f) != kHeaderSize || load<std::uint64_t>(h) != kWalMagic ||
      crc32(h, 24) != load<std::uint32_t>(h + 24)) {
    std::fclose(f);
    throw StorageError("corrupt WAL header in " + path_);
  }
  base_lsn_ = load<std::uint64_t>(h + 8);
  checkpoint_lsn_ = load<std::uint64_t>(h + 16);
  std::uint64_t pos = kHeaderSize;
  std::vector<std::byte> payload;
  for (;;) {
    std::byte fh[8];
    if (std::fread(fh, 1, 8, f) != 8) break;
    auto len = load<std::uint32_t>(fh);
    if (len == 0 || len > kMaxPayload) break;
    payload.resize(len);
    if (std::fread(payload.data(), 1, len, f) != len) break;
    if (crc32(payload.data(), len) != load<std::uint32_t>(fh + 4)) break;
    LogRecord tmp;
    if (!decode(payload.data(), len, tmp)) break;
    pos += 8 + len;
  }
  std::fclose(f);
  if (std::filesystem::file_size(path_, ec) != pos) std::filesystem::resize_file(path_, pos);
  file_end_ = synced_size_ = pos;
  open_file();
}

void Wal::open_file() {
  file_ = std::fopen(path_.c_str(), "r+b");
  if (file_ == nullptr) throw StorageError("cannot open WAL " + path_);
}

Wal::~Wal() {
  if (file_ != nullptr) std::fclose(file_);
}

lsn_t Wal::append(const LogRecord& rec) {
  lsn_t lsn = end_lsn();
  auto payload = encode(rec);
  std::byte fh[8];
  store<std::uint32_t>(fh, static_cast<std::uint32_t>(payload.size()));
  store<std::uint32_t>(fh + 4, crc32(payload.data(), payload.size()));
  buffer_.insert(buffer_.end(), fh, fh + 8);
  buffer_.insert(buffer_.end(), payload.begin(), payload.end());
  bytes_appended_ += 8 + payload.size();
  return lsn;
}

void Wal::write_buffer_to_os() {
  if (buffer_.empty()) return;
  std::size_t n = buffer_.size();
  std::size_t cut = n;
  bool crash = faults_ != nullptr && faults_->should_crash();
  if (crash) cut = faults_->random_below(n + 1);  // torn write: only a prefix reaches the file
  std::fseek(file_, static_cast<long>(file_end_), SEEK_SET);
  if (cut > 0 && std::fwrite(buffer_.data(), 1, cut, file_) != cut) throw StorageError("WAL write failed");
  std::fflush(file_);
  if (crash) {
    file_end_ += cut;
    throw SimulatedCrash();
  }
  file_end_ += n;
  buffer_.clear();
}

void Wal::flush(lsn_t up_to) {
  if (up_to < base_lsn_) return;  // nothing logged yet (lsn 0)
  std::uint64_t off = offset_of(up_to);
  if (off < (sync_on_flush_ ? synced_size_ : file_end_)) return;  // already durable
  if (buffer_.empty() && (!sync_on_flush_ || file_end_ == synced_size_)) return;
  write_buffer_to_os();
  ++flush_count_;
  if (sync_on_flush_) {
    if (faults_ != nullptr && faults_->should_crash()) throw SimulatedCrash();  // written but not yet fsynced
    if (faults_ == nullptr) sync_file(file_);  // tests with an injector track durability virtually
    synced_size_ = file_end_;
  }
}

std::optional<LogRecord> Wal::read(lsn_t lsn) {
  if (lsn < base_lsn_) return std::nullopt;
  std::uint64_t off = offset_of(lsn);
  std::uint64_t total = file_end_ + buffer_.size();
  auto read_bytes = [&](std::uint64_t at, std::byte* dst, std::size_t n) {
    if (at + n > total) return false;
    std::size_t done = 0;
    if (at < file_end_) {
      std::size_t from_file = static_cast<std::size_t>(std::min<std::uint64_t>(n, file_end_ - at));
      std::fseek(file_, static_cast<long>(at), SEEK_SET);
      if (std::fread(dst, 1, from_file, file_) != from_file) return false;
      done = from_file;
    }
    if (done < n) std::memcpy(dst + done, buffer_.data() + (at + done - file_end_), n - done);
    return true;
  };
  std::byte fh[8];
  if (!read_bytes(off, fh, 8)) return std::nullopt;
  auto len = load<std::uint32_t>(fh);
  if (len == 0 || len > kMaxPayload) return std::nullopt;
  std::vector<std::byte> payload(len);
  if (!read_bytes(off + 8, payload.data(), len)) return std::nullopt;
  if (crc32(payload.data(), len) != load<std::uint32_t>(fh + 4)) return std::nullopt;
  LogRecord r;
  if (!decode(payload.data(), len, r)) return std::nullopt;
  r.lsn = lsn;
  return r;
}

void Wal::scan(lsn_t from, const std::function<bool(const LogRecord&)>& fn) {
  write_buffer_to_os();  // make unflushed records visible to the reader below
  lsn_t lsn = from == 0 ? base_lsn_ : from;
  if (lsn < base_lsn_ || offset_of(lsn) >= file_end_) return;
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path_.c_str(), "rb"), &std::fclose);  // closed even if fn throws
  std::FILE* f = file.get();
  if (f == nullptr) throw StorageError("cannot open WAL for scanning");
  std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
  std::fseek(f, static_cast<long>(offset_of(lsn)), SEEK_SET);
  std::vector<std::byte> payload;
  std::uint64_t pos = offset_of(lsn);
  while (pos < file_end_) {
    std::byte fh[8];
    if (std::fread(fh, 1, 8, f) != 8) break;
    auto len = load<std::uint32_t>(fh);
    if (len == 0 || len > kMaxPayload) break;
    payload.resize(len);
    if (std::fread(payload.data(), 1, len, f) != len) break;
    if (crc32(payload.data(), len) != load<std::uint32_t>(fh + 4)) break;
    LogRecord r;
    if (!decode(payload.data(), len, r)) break;
    r.lsn = lsn;
    if (!fn(r)) break;
    lsn += 8 + len;
    pos += 8 + len;
  }
}

void Wal::set_checkpoint(lsn_t lsn) {
  checkpoint_lsn_ = lsn;
  write_header(file_);
  sync_file(file_);
}

void Wal::truncate_all() {
  flush_all();
  const lsn_t old_base = base_lsn_, old_ckpt = checkpoint_lsn_;
  const lsn_t new_base = end_lsn();
  std::string tmp = path_ + ".tmp";
  base_lsn_ = new_base;  // write_header serialises the members
  checkpoint_lsn_ = 0;
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    base_lsn_ = old_base;
    checkpoint_lsn_ = old_ckpt;
    throw StorageError("cannot create " + tmp);
  }
  write_header(f);
  sync_file(f);
  std::fclose(f);
  std::fclose(file_);
  file_ = nullptr;
  // Another process (antivirus, indexer) can briefly hold the file on Windows, making the rename fail.
  // Retry; if it never succeeds keep the old log: the checkpoint is still valid, the log just stays long.
  bool replaced = false;
  for (int attempt = 0; attempt < 100 && !replaced; ++attempt) {
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    replaced = !ec;
    if (!replaced) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (replaced) {
    file_end_ = synced_size_ = kHeaderSize;
    buffer_.clear();
  } else {
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    base_lsn_ = old_base;
    checkpoint_lsn_ = old_ckpt;
  }
  open_file();
}

}  // namespace aqo
