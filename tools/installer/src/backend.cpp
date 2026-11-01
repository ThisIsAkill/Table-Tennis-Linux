// backend.cpp - see backend.h.

#include "backend.h"

#include "system.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace tt::backend {
namespace fs = std::filesystem;

const std::vector<StepInfo>& KnownSteps() {
  static const std::vector<StepInfo> steps = {
      {"check", "Checking your system", 1.0f},
      {"tools", "Getting build tools", 10.0f},
      {"image", "Preparing the build container", 8.0f},
      {"extract", "Reading your game disc", 2.0f},
      {"sdk", "Building the ReXGlue SDK", 30.0f},
      {"codegen", "Generating the game code", 8.0f},
      {"build", "Compiling the game", 32.0f},
      {"install", "Installing", 2.0f},
      {"integrate", "Adding shortcuts", 1.0f},
  };
  return steps;
}

int KnownStepIndex(const std::string& id) {
  const auto& steps = KnownSteps();
  for (size_t i = 0; i < steps.size(); ++i) {
    if (id == steps[i].id) return int(i);
  }
  return -1;
}

Line ParseLine(const std::string& text) {
  Line line;
  line.raw = text;
  auto rest_after = [&](size_t prefix_len) {
    size_t start = prefix_len;
    while (start < text.size() && text[start] == ' ') ++start;
    return text.substr(start);
  };
  auto starts = [&](const char* prefix) {
    const size_t n = std::strlen(prefix);
    return text.compare(0, n, prefix) == 0 && (text.size() == n || text[n] == ' ');
  };
  if (starts("@@step")) {
    line.kind = LineKind::Step;
    const std::string rest = rest_after(6);
    const size_t space = rest.find(' ');
    line.a = rest.substr(0, space);
    if (space != std::string::npos) {
      size_t start = space;
      while (start < rest.size() && rest[start] == ' ') ++start;
      line.b = rest.substr(start);
    }
    if (line.b.empty()) {
      const int known = KnownStepIndex(line.a);
      line.b = known >= 0 ? KnownSteps()[known].title : line.a;
    }
  } else if (starts("@@progress")) {
    line.kind = LineKind::Progress;
    line.value = std::atoi(rest_after(10).c_str());
    if (line.value < 0) line.value = 0;
    if (line.value > 100) line.value = 100;
  } else if (starts("@@log")) {
    line.kind = LineKind::Log;
    line.a = rest_after(5);
  } else if (starts("@@warn")) {
    line.kind = LineKind::Warn;
    line.a = rest_after(6);
  } else if (starts("@@error")) {
    line.kind = LineKind::Error;
    line.a = rest_after(7);
  } else if (starts("@@result")) {
    // "@@result <what> <outcome>", e.g. "@@result steam added".
    line.kind = LineKind::Result;
    const std::string rest = rest_after(8);
    const size_t space = rest.find(' ');
    line.a = rest.substr(0, space);
    if (space != std::string::npos) line.b = rest.substr(rest.find_first_not_of(' ', space));
  } else if (starts("@@done")) {
    line.kind = LineKind::Done;
    line.a = rest_after(6);
  } else {
    line.kind = LineKind::Plain;
    line.a = text;
  }
  return line;
}

fs::path FindBackend(const fs::path& installer_dir) {
  std::error_code ec;
  fs::path dir = fs::weakly_canonical(installer_dir, ec);
  if (ec) dir = installer_dir;
  for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
    const fs::path candidate = dir / "scripts" / "install" / "tt-install.sh";
    if (fs::is_regular_file(candidate, ec)) return candidate;
    const fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}

fs::path RepoRootOf(const fs::path& backend) {
  if (backend.empty()) return {};
  // <root>/scripts/install/tt-install.sh, or a stand-in script elsewhere: walk up until a
  // directory with project/tools/disc_image.py appears.
  std::error_code ec;
  fs::path dir = fs::weakly_canonical(backend, ec).parent_path();
  for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
    if (fs::is_regular_file(dir / "project" / "tools" / "disc_image.py", ec)) return dir;
    const fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}

// --- Process ----------------------------------------------------------------------------------
Process::~Process() {
  if (Running()) {
    kill(-pid_, SIGTERM);
    int status = 0;
    waitpid(pid_, &status, 0);
  }
  if (reader_.joinable()) reader_.join();
  if (log_) std::fclose(log_);
}

std::vector<std::string> Process::CommandLine(const Options& o) const {
  std::vector<std::string> argv = {o.backend.string(), "--iso", o.iso.string(), "--prefix",
                                   o.prefix.string(), "--steam", o.steam ? "yes" : "no",
                                   "--desktop", o.desktop ? "yes" : "no"};
  if (o.cache) {
    argv.push_back("--cache");
    argv.push_back(o.cache->string());
  }
  return argv;
}

void Process::OpenLogFile() {
  fs::path dir;
  if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) {
    dir = fs::path(state) / "tt-recomp-installer";
  } else {
    dir = sys::HomeDir() / ".local" / "state" / "tt-recomp-installer";
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  log_file_ = dir / "install.log";
  log_ = std::fopen(log_file_.c_str(), "w");
  if (!log_) log_file_.clear();
}

bool Process::Start(const Options& options, std::string& error) {
  if (pid_ > 0) {
    error = "already running";
    return false;
  }
  if (!sys::IsExecutableFile(options.backend)) {
    error = "The setup script " + options.backend.string() + " is missing or not executable.";
    return false;
  }
  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
    error = std::string("pipe: ") + std::strerror(errno);
    return false;
  }
  const std::vector<std::string> argv = CommandLine(options);
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);

  OpenLogFile();
  if (log_) {
    std::fprintf(log_, "$");
    for (const auto& a : argv) std::fprintf(log_, " %s", a.c_str());
    std::fprintf(log_, "\n");
    std::fflush(log_);
  }

  const pid_t pid = fork();
  if (pid < 0) {
    error = std::string("fork: ") + std::strerror(errno);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return false;
  }
  if (pid == 0) {
    // Child: own process group so Cancel() reaches everything the script starts.
    setsid();
    const int null = open("/dev/null", O_RDONLY);
    if (null >= 0) dup2(null, 0);
    dup2(pipe_fds[1], 1);
    dup2(pipe_fds[1], 2);
    setenv("PYTHONUNBUFFERED", "1", 1);
    setenv("TT_INSTALLER_GUI", "1", 1);
    execv(args[0], args.data());
    const char* message = "@@error The setup script could not be started\n";
    if (write(1, message, std::strlen(message)) < 0) {
    }
    _exit(127);
  }
  close(pipe_fds[1]);
  pid_ = pid;
  reader_ = std::thread(&Process::ReaderLoop, this, pipe_fds[0]);
  return true;
}

void Process::ReaderLoop(int fd) {
  std::string buffer;
  char chunk[4096];
  auto push = [&](std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    std::lock_guard lock(mutex_);
    if (log_) {
      std::fprintf(log_, "%s\n", line.c_str());
      std::fflush(log_);
    }
    all_.push_back(line);
    pending_.push_back(std::move(line));
  };
  for (;;) {
    const ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    buffer.append(chunk, size_t(n));
    size_t start = 0;
    for (;;) {
      const size_t nl = buffer.find('\n', start);
      if (nl == std::string::npos) break;
      push(buffer.substr(start, nl - start));
      start = nl + 1;
    }
    buffer.erase(0, start);
  }
  if (!buffer.empty()) push(buffer);
  close(fd);
  reader_done_.store(true);
}

std::vector<std::string> Process::TakeLines() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> lines(pending_.begin(), pending_.end());
  pending_.clear();
  return lines;
}

std::string Process::FullLog() const {
  std::lock_guard lock(mutex_);
  std::string text;
  for (const auto& line : all_) {
    text += line;
    text += '\n';
  }
  return text;
}

void Process::Poll() {
  if (pid_ <= 0 || exited_) return;
  int status = 0;
  const pid_t r = waitpid(pid_, &status, WNOHANG);
  if (r == pid_) {
    exited_ = true;
    if (WIFEXITED(status)) {
      exit_code_ = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      killed_by_signal_ = true;
      exit_code_ = 128 + WTERMSIG(status);
    }
    // Let the reader drain what is left in the pipe before callers read the final lines.
    if (reader_.joinable()) reader_.join();
    if (log_) {
      std::fprintf(log_, "[exit %d]\n", exit_code_);
      std::fclose(log_);
      log_ = nullptr;
    }
    return;
  }
  if (cancel_stage_ > 0 && std::chrono::steady_clock::now() >= cancel_next_) {
    if (cancel_stage_ == 1) {
      kill(-pid_, SIGTERM);
      cancel_stage_ = 2;
      cancel_next_ = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    } else if (cancel_stage_ == 2) {
      kill(-pid_, SIGKILL);
      cancel_stage_ = 3;
    }
  }
}

void Process::Cancel() {
  if (!Running() || cancel_stage_ > 0) return;
  kill(-pid_, SIGINT);
  cancel_stage_ = 1;
  cancel_next_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

}  // namespace tt::backend
