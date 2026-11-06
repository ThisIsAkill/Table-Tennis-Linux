// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// CHD disc images: the Xbox 360 disc compressed with MAME's chdman (createdvd), mounted as the
// game disc like an ISO. Decompressed on demand through libchdr; nothing is unpacked to disk.
#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include <rex/filesystem/device.h>

namespace tabletennis::chd {

// True for a path ending in .chd (any case).
bool IsChdPath(const std::filesystem::path& path);

// A read-only device for the disc in a CHD image, or nullptr if it cannot be opened or holds no
// Xbox 360 game filesystem (the reason is logged).
std::unique_ptr<rex::filesystem::Device> CreateDevice(std::string_view mount_path,
                                                      const std::filesystem::path& path);

}  // namespace tabletennis::chd
