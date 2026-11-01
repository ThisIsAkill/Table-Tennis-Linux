// backend.h - runs scripts/install/tt-install.sh as a child process and parses its progress
// protocol. One line per message on stdout; anything else is plain log text:
//
//   @@step <id> <human title>   ids, in order: check tools image extract sdk codegen build
//                               install integrate (tools/image appear at most once, or not at all)
//   @@progress <0-100>          overall progress
//   @@log <text>
//   @@warn <text>               shown without interrupting the install
//   @@error <message>           followed by a non-zero exit
//   @@done <install dir>
#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tt::backend {

// The known steps in their expected order, with default titles and a rough share of the total
// time; used to lay out the step list before the backend announces anything and to estimate the
// remaining time.
struct StepInfo {
  const char* id;
  const char* title;
  float weight;
};
const std::vector<StepInfo>& KnownSteps();
int KnownStepIndex(const std::string& id);  // -1 when unknown

struct Options {
  std::filesystem::path backend;  // the script to run
  std::filesystem::path iso;
  std::filesystem::path prefix;
  std::optional<std::filesystem::path> cache;  // --cache DIR, only when set
  bool steam = true;
  bool desktop = true;
};

enum class LineKind { Step, Progress, Log, Warn, Error, Result, Done, Plain };
struct Line {
  LineKind kind = LineKind::Plain;
  std::string a;  // step id, message, install dir
  std::string b;  // step title
  int value = 0;  // progress
  std::string raw;
};
Line ParseLine(const std::string& text);

// Finds the backend script relative to the installer: walks up from the installer's directory
// looking for scripts/install/tt-install.sh (so the binary may live at the checkout root or in
// tools/installer/bin). Returns an empty path when nothing is found.
std::filesystem::path FindBackend(const std::filesystem::path& installer_dir);
// The source checkout containing `backend` (two levels above scripts/install/), or empty.
std::filesystem::path RepoRootOf(const std::filesystem::path& backend);

class Process {
 public:
  Process() = default;
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  // Starts the backend; returns false with `error` set when it cannot be started.
  bool Start(const Options& options, std::string& error);
  std::vector<std::string> CommandLine(const Options& options) const;

  // Lines that arrived since the last call (output is also mirrored to the log file).
  std::vector<std::string> TakeLines();
  // Polls the child; once it has exited, Running() is false and ExitCode() is set.
  void Poll();
  bool Running() const { return pid_ > 0 && !exited_; }
  bool Started() const { return pid_ > 0; }
  std::optional<int> ExitCode() const { return exited_ ? std::optional<int>(exit_code_) : std::nullopt; }
  bool KilledBySignal() const { return exited_ && killed_by_signal_; }

  // Asks the whole process group to stop: SIGINT first, SIGTERM a few seconds later, SIGKILL
  // as a last resort. Call Poll() until Running() is false.
  void Cancel();
  bool Cancelling() const { return cancel_stage_ > 0; }

  const std::filesystem::path& LogFile() const { return log_file_; }
  std::string FullLog() const;

 private:
  void ReaderLoop(int fd);
  void OpenLogFile();

  pid_t pid_ = 0;
  bool exited_ = false;
  bool killed_by_signal_ = false;
  int exit_code_ = 0;
  int cancel_stage_ = 0;
  std::chrono::steady_clock::time_point cancel_next_{};
  std::thread reader_;
  std::atomic<bool> reader_done_{false};
  mutable std::mutex mutex_;
  std::deque<std::string> pending_;
  std::vector<std::string> all_;
  std::filesystem::path log_file_;
  FILE* log_ = nullptr;
};

}  // namespace tt::backend
