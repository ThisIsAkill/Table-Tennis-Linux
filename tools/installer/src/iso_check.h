// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// iso_check.h - verifies that a chosen file is the supported Table Tennis disc image.
//
// This is the one place that knows how verification is done. Today it runs the project's own
// read-only check (project/tools/disc_image.py, the same code the backend's `check` step uses)
// in a python3 child process; when a backend is named explicitly with --backend, that script is
// asked instead with `--verify PATH` and must answer `@@verify ok <description>` or
// `@@verify fail <reason>` on stdout. Switching the real backend over means flipping
// `kAskBackend` below once scripts/install/tt-install.sh supports --verify.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

namespace tt::iso {

enum class Verdict {
  Ok,
  NotADiscImage,   // no Xbox 360 filesystem: not an ISO of a 360 game
  WrongGame,       // a 360 disc, but not Table Tennis
  WrongVersion,    // Table Tennis, but not the supported USA/Europe retail release
  Modified,        // the menu archive differs: a modified or unsupported image
  Damaged,         // truncated or unreadable
  Unreadable,      // permission denied, vanished, not a file
  ToolMissing,     // python3 or the check script is missing
  Failed,          // anything else; `detail` has the raw message
};

struct Result {
  Verdict verdict = Verdict::Failed;
  std::string headline;  // "This is the right disc"
  std::string detail;    // what was found, in plain words
  std::string raw;       // the tool's own message, for the details view
};

struct Setup {
  std::filesystem::path repo_root;  // checkout holding project/tools/disc_image.py
  std::filesystem::path backend;    // set when a backend was named explicitly
  bool ask_backend = false;         // use `backend --verify` instead of the python check
};

// Blocking check; run it from the Checker below so the window stays responsive.
Result Verify(const std::filesystem::path& iso, const Setup& setup);

class Checker {
 public:
  ~Checker();
  void Start(std::filesystem::path iso, Setup setup);
  bool Running() const { return started_ && !done_.load(); }
  bool Done() const { return done_.load(); }
  Result Take();  // joins the thread; valid once Done()
  void Reset();

 private:
  std::thread thread_;
  std::atomic<bool> done_{false};
  bool started_ = false;
  Result result_;
};

}  // namespace tt::iso
