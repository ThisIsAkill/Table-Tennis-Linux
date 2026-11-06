// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// chd_disc.cpp - see chd_disc.h. The filesystem walk follows the SDK's DiscImageDevice (the
// Xbox 360 GDFX layout: a volume descriptor at sector 32 of the game partition, directories as
// binary trees of entries), but every read goes through a CHD hunk cache instead of a memory map.
#include "chd_disc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <list>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <libchdr/chd.h>

#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/string/utf8.h>

namespace tabletennis::chd {
namespace {

using rex::X_STATUS;

constexpr uint64_t kSectorSize = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
// Decompressed hunks kept in memory (a DVD CHD hunk is usually 4 KiB-64 KiB). The game streams
// files in large sequential reads, so a small cache mostly serves the directory walk and reads
// that straddle a hunk boundary.
constexpr size_t kCachedHunks = 64;

// The CHD file with a small LRU cache of decompressed hunks. libchdr is not thread-safe per
// file, and the game reads from several threads, so every read takes the lock.
class Image {
 public:
  ~Image() {
    if (chd_) chd_close(chd_);
  }

  bool Open(const std::filesystem::path& path) {
    const chd_error error = chd_open(path.string().c_str(), CHD_OPEN_READ, nullptr, &chd_);
    if (error != CHDERR_NONE) {
      REXLOG_ERROR("Could not open CHD image {}: {}", path.string(), chd_error_string(error));
      chd_ = nullptr;
      return false;
    }
    const chd_header* header = chd_get_header(chd_);
    hunk_bytes_ = header->hunkbytes;
    size_ = header->logicalbytes;
    if (!hunk_bytes_ || !size_) {
      REXLOG_ERROR("CHD image {} has no data", path.string());
      return false;
    }
    REXLOG_INFO("CHD image {}: {} MB of disc data, {} KiB hunks", path.string(), size_ >> 20,
                hunk_bytes_ >> 10);
    return true;
  }

  uint64_t size() const { return size_; }

  // Copies [offset, offset + length) of the uncompressed disc into out.
  bool Read(uint64_t offset, void* out, size_t length) {
    if (offset > size_ || length > size_ - offset) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto* dst = static_cast<uint8_t*>(out);
    while (length) {
      const uint32_t hunk = uint32_t(offset / hunk_bytes_);
      const uint32_t inside = uint32_t(offset % hunk_bytes_);
      const size_t chunk = std::min<size_t>(length, hunk_bytes_ - inside);
      const std::vector<uint8_t>* data = Hunk(hunk);
      if (!data) return false;
      std::memcpy(dst, data->data() + inside, chunk);
      dst += chunk;
      offset += chunk;
      length -= chunk;
    }
    return true;
  }

 private:
  const std::vector<uint8_t>* Hunk(uint32_t hunk) {
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->first == hunk) {
        cache_.splice(cache_.begin(), cache_, it);
        return &cache_.front().second;
      }
    }
    std::vector<uint8_t> data;
    if (cache_.size() >= kCachedHunks) {
      data = std::move(cache_.back().second);
      cache_.pop_back();
    }
    data.resize(hunk_bytes_);
    const chd_error error = chd_read(chd_, hunk, data.data());
    if (error != CHDERR_NONE) {
      REXLOG_ERROR("CHD read of hunk {} failed: {}", hunk, chd_error_string(error));
      return nullptr;
    }
    cache_.emplace_front(hunk, std::move(data));
    return &cache_.front().second;
  }

  chd_file* chd_ = nullptr;
  uint32_t hunk_bytes_ = 0;
  uint64_t size_ = 0;
  std::mutex mutex_;
  std::list<std::pair<uint32_t, std::vector<uint8_t>>> cache_;
};

class ChdEntry;

class ChdFile : public rex::filesystem::File {
 public:
  ChdFile(uint32_t file_access, ChdEntry* entry);
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t*) override {
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t) override { return X_STATUS_ACCESS_DENIED; }

 private:
  ChdEntry* chd_entry_;
};

class ChdEntry : public rex::filesystem::Entry {
 public:
  ChdEntry(rex::filesystem::Device* device, Entry* parent, std::string_view path, Image* image)
      : Entry(device, parent, path), image_(image) {}

  X_STATUS Open(uint32_t desired_access, rex::filesystem::File** out_file) override {
    *out_file = new ChdFile(desired_access, this);
    return X_STATUS_SUCCESS;
  }

  Image* image() const { return image_; }
  uint64_t data_offset() const { return data_offset_; }
  uint64_t data_size() const { return data_size_; }

 private:
  friend class ChdDevice;
  Image* image_;
  uint64_t data_offset_ = 0;
  uint64_t data_size_ = 0;
};

ChdFile::ChdFile(uint32_t file_access, ChdEntry* entry)
    : File(file_access, entry), chd_entry_(entry) {}

X_STATUS ChdFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) {
  if (byte_offset >= chd_entry_->size()) return X_STATUS_END_OF_FILE;
  const size_t length = std::min<size_t>(buffer.size(), chd_entry_->data_size() - byte_offset);
  if (!chd_entry_->image()->Read(chd_entry_->data_offset() + byte_offset, buffer.data(), length)) {
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_bytes_read = length;
  return X_STATUS_SUCCESS;
}

class ChdDevice : public rex::filesystem::Device {
 public:
  ChdDevice(std::string_view mount_path, std::filesystem::path path)
      : Device(mount_path), name_("GDFX"), path_(std::move(path)) {}

  bool Initialize() override {
    if (!image_.Open(path_)) return false;
    // Sector 32 of the game partition holds the volume descriptor; the partition starts at one
    // of a few offsets depending on the disc layout (the same list as DiscImageDevice).
    static constexpr std::array<uint64_t, 5> kPartitionOffsets = {0x00000000, 0x0000FB20,
                                                                  0x00020600, 0x02080000,
                                                                  0x0FD90000};
    std::array<uint8_t, 32> descriptor{};
    bool found = false;
    for (const uint64_t offset : kPartitionOffsets) {
      if (image_.Read(offset + 32 * kSectorSize, descriptor.data(), descriptor.size()) &&
          std::memcmp(descriptor.data(), kMagic, 20) == 0) {
        game_offset_ = offset;
        found = true;
        break;
      }
    }
    if (!found) {
      REXLOG_ERROR("CHD image {} holds no Xbox 360 game disc", path_.string());
      return false;
    }
    const uint32_t root_sector = rex::memory::load<uint32_t>(descriptor.data() + 20);
    const uint32_t root_size = rex::memory::load<uint32_t>(descriptor.data() + 24);
    if (root_size < 13 || root_size > 32 * 1024 * 1024) {
      REXLOG_ERROR("CHD image {}: damaged disc directory", path_.string());
      return false;
    }
    auto root = std::make_unique<ChdEntry>(this, nullptr, "", &image_);
    root->attributes_ = rex::filesystem::kFileAttributeDirectory;
    ChdEntry* root_entry = root.get();
    root_entry_ = std::move(root);
    if (!ReadDirectory(root_sector, root_size, root_entry, 0)) {
      REXLOG_ERROR("CHD image {}: could not read the disc directories", path_.string());
      return false;
    }
    REXLOG_INFO("Mounted CHD disc: {} files, {} MB", file_count_, total_file_size_ >> 20);
    return true;
  }

  void Dump(rex::string::StringBuffer* string_buffer) override {
    string_buffer->AppendFormat("{}: CHD disc, {} files, {} bytes\n", mount_path(), file_count_,
                                total_file_size_);
  }

  rex::filesystem::Entry* ResolvePath(const std::string_view path) override {
    return root_entry_->ResolvePath(path);
  }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override {
    return uint32_t(image_.size() / bytes_per_sector());
  }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  // Reads a directory's entry table and adds its entries (and their subdirectories) to parent.
  bool ReadDirectory(uint32_t sector, uint32_t size, ChdEntry* parent, int depth) {
    if (depth > 32 || size > 32 * 1024 * 1024) return false;
    std::vector<uint8_t> table(size);
    if (!image_.Read(game_offset_ + uint64_t(sector) * kSectorSize, table.data(), size)) {
      return false;
    }
    std::vector<bool> visited(size / 4 + 1, false);
    return ReadEntry(table, 0, parent, depth, visited);
  }

  // One node of a directory's binary tree: left subtree, the entry, then the right subtree.
  bool ReadEntry(const std::vector<uint8_t>& table, uint32_t ordinal, ChdEntry* parent, int depth,
                 std::vector<bool>& visited) {
    const size_t offset = size_t(ordinal) * 4;
    if (offset + 14 > table.size() || visited[ordinal]) return false;
    visited[ordinal] = true;
    const uint8_t* p = table.data() + offset;
    const uint16_t node_l = rex::memory::load<uint16_t>(p + 0);
    const uint16_t node_r = rex::memory::load<uint16_t>(p + 2);
    const uint32_t sector = rex::memory::load<uint32_t>(p + 4);
    const uint32_t length = rex::memory::load<uint32_t>(p + 8);
    const uint8_t attributes = p[12];
    const uint8_t name_length = p[13];
    if (offset + 14 + name_length > table.size()) return false;

    if (node_l && !ReadEntry(table, node_l, parent, depth, visited)) return false;

    const std::string name(reinterpret_cast<const char*>(p + 14), name_length);
    auto entry = std::make_unique<ChdEntry>(
        this, parent, rex::string::utf8_join_guest_paths(parent->path(), name), &image_);
    entry->attributes_ = attributes | rex::filesystem::kFileAttributeReadOnly;
    entry->size_ = length;
    entry->allocation_size_ = rex::round_up(size_t(length), size_t(bytes_per_sector()));
    // January 1, 1970 (UTC) in 100-nanosecond intervals, as DiscImageDevice reports.
    entry->create_timestamp_ = entry->access_timestamp_ = entry->write_timestamp_ =
        10000 * 11644473600000LL;
    if (attributes & rex::filesystem::kFileAttributeDirectory) {
      if (length && !ReadDirectory(sector, length, entry.get(), depth + 1)) return false;
    } else {
      entry->data_offset_ = game_offset_ + uint64_t(sector) * kSectorSize;
      entry->data_size_ = length;
      if (entry->data_offset_ > image_.size() || length > image_.size() - entry->data_offset_) {
        return false;
      }
      ++file_count_;
      total_file_size_ += length;
    }
    parent->children_.emplace_back(std::move(entry));

    if (node_r && !ReadEntry(table, node_r, parent, depth, visited)) return false;
    return true;
  }

  std::string name_;
  std::filesystem::path path_;
  Image image_;
  std::unique_ptr<rex::filesystem::Entry> root_entry_;
  uint64_t game_offset_ = 0;
  uint64_t file_count_ = 0;
  uint64_t total_file_size_ = 0;
};

}  // namespace

bool IsChdPath(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return extension == ".chd";
}

std::unique_ptr<rex::filesystem::Device> CreateDevice(std::string_view mount_path,
                                                      const std::filesystem::path& path) {
  auto device = std::make_unique<ChdDevice>(mount_path, path);
  if (!device->Initialize()) return nullptr;
  return device;
}

}  // namespace tabletennis::chd
