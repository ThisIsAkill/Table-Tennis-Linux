// system.cpp - see system.h.

#include "system.h"

#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace tt::sys {
namespace fs = std::filesystem;

namespace {

std::string Lower(std::string text) {
  for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

fs::path NearestExisting(fs::path path) {
  std::error_code ec;
  while (!path.empty() && !fs::exists(path, ec)) {
    const fs::path parent = path.parent_path();
    if (parent == path) break;
    path = parent;
  }
  return path;
}

}  // namespace

fs::path HomeDir() {
  if (const char* home = std::getenv("HOME"); home && *home) return home;
  if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
  return "/";
}

std::string UserName() {
  if (const char* user = std::getenv("USER"); user && *user) return user;
  if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_name) return pw->pw_name;
  return "user";
}

fs::path ExecutableDir() {
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec || exe.empty()) return fs::current_path(ec);
  return exe.parent_path();
}

uint64_t FreeBytes(const fs::path& path) {
  const fs::path probe = NearestExisting(path);
  struct statvfs st {};
  if (probe.empty() || statvfs(probe.c_str(), &st) != 0) return 0;
  return uint64_t(st.f_bavail) * uint64_t(st.f_frsize);
}

uint64_t DeviceOf(const fs::path& path) {
  const fs::path probe = NearestExisting(path);
  struct stat st {};
  if (probe.empty() || stat(probe.c_str(), &st) != 0) return 0;
  return uint64_t(st.st_dev);
}

std::vector<Mount> UserMounts() {
  std::vector<Mount> mounts;
  if (const char* fake = std::getenv("TT_INSTALLER_FAKE_MOUNTS"); fake && *fake) {
    std::istringstream list(fake);
    std::string entry;
    uint64_t device = 0xFA4E0000;
    while (std::getline(list, entry, ';')) {
      std::istringstream fields(entry);
      std::string dir, label, kind;
      std::getline(fields, dir, '|');
      std::getline(fields, label, '|');
      std::getline(fields, kind, '|');
      if (dir.empty()) continue;
      Mount m;
      m.mountpoint = dir;
      m.device = kind == "sd" ? "/dev/mmcblk0p1" : "/dev/sdz1";
      m.label = label.empty() ? m.mountpoint.filename().string() : label;
      m.is_sd_card = kind == "sd";
      m.fake_device = ++device;
      mounts.push_back(std::move(m));
    }
    return mounts;
  }
  std::ifstream in("/proc/mounts");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string device, where, type;
    if (!(fields >> device >> where >> type)) continue;
    // /proc/mounts escapes spaces as \040.
    std::string decoded;
    for (size_t i = 0; i < where.size(); ++i) {
      if (where[i] == '\\' && i + 3 < where.size()) {
        decoded += char(std::strtol(where.substr(i + 1, 3).c_str(), nullptr, 8));
        i += 3;
      } else {
        decoded += where[i];
      }
    }
    const bool user_media = decoded.rfind("/run/media/", 0) == 0 || decoded.rfind("/media/", 0) == 0;
    if (!user_media || device.rfind("/dev/", 0) != 0) continue;
    if (type == "iso9660" || type == "udf" || type == "squashfs") continue;
    Mount m;
    m.mountpoint = decoded;
    m.device = device;
    m.label = m.mountpoint.filename().string();
    m.is_sd_card = device.find("mmcblk") != std::string::npos;
    mounts.push_back(std::move(m));
  }
  return mounts;
}

bool IsSteamOS() {
  std::ifstream in("/etc/os-release");
  std::string line;
  while (std::getline(in, line)) {
    if (line == "ID=steamos" || line == "ID=\"steamos\"") return true;
  }
  return false;
}

bool IsExecutableFile(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec) && access(path.c_str(), X_OK) == 0;
}

std::string FormatBytes(uint64_t bytes) {
  char buffer[32];
  const double gb = double(bytes) / 1e9;
  if (gb >= 100.0) {
    std::snprintf(buffer, sizeof(buffer), "%.0f GB", gb);
  } else if (gb >= 1.0) {
    std::snprintf(buffer, sizeof(buffer), "%.1f GB", gb);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.0f MB", double(bytes) / 1e6);
  }
  return buffer;
}

bool LaunchDetached(const std::vector<std::string>& argv, const fs::path& working_dir) {
  if (argv.empty()) return false;
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  // Double fork so the program is reparented to init and never becomes our zombie.
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    const pid_t inner = fork();
    if (inner != 0) _exit(inner < 0 ? 1 : 0);
    const int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2) close(null);
    }
    if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) _exit(1);
    execvp(args[0], args.data());
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void OpenInFileManager(const fs::path& dir) { LaunchDetached({"xdg-open", dir.string()}); }

// --- ISO scan --------------------------------------------------------------------------------
std::string DescribeLocation(const fs::path& file) {
  const std::string path = file.string();
  for (const Mount& m : UserMounts()) {
    const std::string root = m.mountpoint.string();
    if (path.rfind(root + "/", 0) == 0) {
      return m.is_sd_card ? "on your SD card" : "on " + m.label;
    }
  }
  const fs::path home = HomeDir();
  const fs::path parent = file.parent_path();
  if (parent == home) return "in your home folder";
  for (const char* name : {"Downloads", "Desktop", "Games", "Documents", "Videos", "Music"}) {
    const std::string prefix = (home / name).string();
    if (path.rfind(prefix + "/", 0) == 0) return std::string("in ") + name;
  }
  if (path.rfind(home.string() + "/", 0) == 0) return "in " + parent.filename().string();
  return "in " + parent.string();
}

IsoScanner::~IsoScanner() {
  if (thread_.joinable()) thread_.join();
}

void IsoScanner::Start(std::vector<fs::path> extra_dirs) {
  if (started_) return;
  started_ = true;
  std::vector<fs::path> dirs = std::move(extra_dirs);
  const fs::path home = HomeDir();
  for (const char* name : {"Downloads", "Desktop", "Games", "ROMs", "Emulation/roms", "Documents"}) {
    dirs.push_back(home / name);
  }
  dirs.push_back(home);
  for (const Mount& m : UserMounts()) dirs.push_back(m.mountpoint);
  thread_ = std::thread(&IsoScanner::Run, this, std::move(dirs));
}

std::vector<FoundIso> IsoScanner::Results() {
  if (thread_.joinable()) thread_.join();
  return results_;
}

void IsoScanner::Run(std::vector<fs::path> dirs) {
  constexpr uint64_t kMinBytes = 1000ull * 1000 * 1000;
  constexpr int kMaxDepth = 2;
  std::vector<FoundIso> found;
  std::vector<fs::path> seen;
  std::error_code ec;
  std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (const auto& entry : it) {
      const fs::path p = entry.path();
      const std::string name = p.filename().string();
      if (name.empty() || name[0] == '.') continue;
      if (entry.is_symlink(ec)) continue;
      if (entry.is_directory(ec)) {
        if (depth < kMaxDepth) walk(p, depth + 1);
        continue;
      }
      if (!entry.is_regular_file(ec)) continue;
      if (Lower(p.extension().string()) != ".iso") continue;
      const uint64_t bytes = entry.file_size(ec);
      if (ec || bytes < kMinBytes) continue;
      const fs::path canonical = fs::weakly_canonical(p, ec);
      if (std::find(seen.begin(), seen.end(), canonical) != seen.end()) continue;
      seen.push_back(canonical);
      FoundIso iso;
      iso.path = p;
      iso.bytes = bytes;
      iso.where = DescribeLocation(p);
      const std::string lower = Lower(name);
      if (lower.find("tabletennis") != std::string::npos ||
          lower.find("table tennis") != std::string::npos ||
          lower.find("table_tennis") != std::string::npos ||
          lower.find("table-tennis") != std::string::npos) {
        iso.score += 100;
      }
      // The retail disc is a dual-layer image of about 7.3 GB.
      const double gb = double(bytes) / 1e9;
      if (gb > 6.5 && gb < 8.5) iso.score += 20;
      found.push_back(std::move(iso));
    }
  };
  for (const fs::path& dir : dirs) {
    if (fs::is_directory(dir, ec)) walk(dir, 0);
  }
  std::stable_sort(found.begin(), found.end(),
                   [](const FoundIso& a, const FoundIso& b) { return a.score > b.score; });
  results_ = std::move(found);
  done_.store(true);
}

}  // namespace tt::sys
