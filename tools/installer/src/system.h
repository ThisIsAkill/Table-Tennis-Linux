// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// system.h - small operating-system helpers for the installer: home and mount discovery, free
// space, the ISO scan, and launching things detached from the installer.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace tt::sys {

std::filesystem::path HomeDir();
std::filesystem::path ExecutableDir();
std::string UserName();

// Free bytes on the filesystem that holds `path` (or its nearest existing ancestor); 0 when it
// cannot be determined.
uint64_t FreeBytes(const std::filesystem::path& path);
// Device id of the filesystem holding `path` (nearest existing ancestor); 0 when unknown.
uint64_t DeviceOf(const std::filesystem::path& path);

// Mounted user media: SD cards and external drives under /run/media and /media.
struct Mount {
  std::filesystem::path mountpoint;
  std::string device;    // "/dev/mmcblk0p1"
  std::string label;     // the directory name, e.g. "primary" or "Samsung T7"
  bool is_sd_card = false;
  uint64_t fake_device = 0;  // set for mounts simulated with TT_INSTALLER_FAKE_MOUNTS
};
// Reads /proc/mounts. For UI testing without real media, TT_INSTALLER_FAKE_MOUNTS lists
// pretend mounts as "dir|label|sd;dir|label|usb".
std::vector<Mount> UserMounts();

bool IsSteamOS();
bool IsExecutableFile(const std::filesystem::path& path);
std::string FormatBytes(uint64_t bytes);  // "7.3 GB"

// Runs a program detached from the installer (its own session, stdio to /dev/null).
bool LaunchDetached(const std::vector<std::string>& argv,
                    const std::filesystem::path& working_dir = {});
void OpenInFileManager(const std::filesystem::path& dir);

// --- ISO scan --------------------------------------------------------------------------------
struct FoundIso {
  std::filesystem::path path;
  uint64_t bytes = 0;
  std::string where;   // "on your SD card", "in Downloads"
  int score = 0;       // higher first: name mentions table tennis, plausible size
};

// Looks through the usual places (Downloads, Desktop, Games, the home folder, mounted media)
// one or two levels deep for .iso files larger than one gigabyte. `extra_dirs` are added for
// testing. Runs on its own thread; poll `Done()` and take `Results()`.
class IsoScanner {
 public:
  IsoScanner() = default;
  ~IsoScanner();
  void Start(std::vector<std::filesystem::path> extra_dirs);
  bool Done() const { return done_.load(); }
  bool Running() const { return started_ && !done_.load(); }
  std::vector<FoundIso> Results();  // valid once Done()

 private:
  void Run(std::vector<std::filesystem::path> dirs);
  std::thread thread_;
  std::atomic<bool> done_{false};
  bool started_ = false;
  std::vector<FoundIso> results_;
};

// A friendly description of where a file lives ("on your SD card", "in Downloads").
std::string DescribeLocation(const std::filesystem::path& file);

}  // namespace tt::sys
