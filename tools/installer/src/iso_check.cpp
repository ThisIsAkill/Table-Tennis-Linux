// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// iso_check.cpp - see iso_check.h.

#include "iso_check.h"

#include "system.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace tt::iso {
namespace fs = std::filesystem;

namespace {

// Runs `argv` with stdout and stderr captured; returns the exit status (or -1).
int RunCapture(const std::vector<std::string>& argv, std::string& output) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[1]);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(fds[1]);
  char chunk[1024];
  for (;;) {
    const ssize_t n = read(fds[0], chunk, sizeof(chunk));
    if (n <= 0) break;
    output.append(chunk, size_t(n));
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

std::string LastLine(const std::string& text) {
  std::string last;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    if (!line.empty()) last = line;
    start = end + 1;
  }
  return last;
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

Result Classify(const std::string& message) {
  Result r;
  r.raw = message;
  if (Contains(message, "No supported XGD2 filesystem")) {
    r.verdict = Verdict::NotADiscImage;
    r.headline = "This is not an Xbox 360 disc image";
    r.detail = "The file has no Xbox 360 disc filesystem. Choose the .iso or .chd of your retail Table Tennis disc.";
  } else if (Contains(message, "is missing from the ISO")) {
    r.verdict = Verdict::WrongGame;
    r.headline = "This disc is not Table Tennis";
    r.detail = "It is an Xbox 360 disc image, but the game files of Table Tennis are not on it.";
  } else if (Contains(message, "Unsupported game executable")) {
    r.verdict = Verdict::WrongVersion;
    r.headline = "This version of the game is not supported";
    r.detail = "Only the USA / Europe retail disc (Title ID 545407DF) works. Other regions, demos and rips with a modified executable are not supported.";
  } else if (Contains(message, "Unsupported menu archive")) {
    r.verdict = Verdict::Modified;
    r.headline = "This disc image has been modified";
    r.detail = "The game's menu files differ from the retail disc, so the port's menu patch cannot be applied. Use an unmodified image.";
  } else if (Contains(message, "Truncated") || Contains(message, "outside the ISO") ||
             Contains(message, "Invalid")) {
    r.verdict = Verdict::Damaged;
    r.headline = "This disc image looks damaged";
    r.detail = "It ends early or its file table is broken. If it was copied over, copy it again, or re-dump the disc.";
  } else if (Contains(message, "Permission denied") || Contains(message, "No such file") ||
             Contains(message, "Is a directory")) {
    r.verdict = Verdict::Unreadable;
    r.headline = "The file could not be read";
    r.detail = "Check that it still exists and that you have permission to read it.";
  } else if (Contains(message, "python3") || Contains(message, "disc_image")) {
    r.verdict = Verdict::ToolMissing;
    r.headline = "The disc check tool is missing";
    r.detail = "python3 and project/tools/disc_image.py are needed next to the installer.";
  } else {
    r.verdict = Verdict::Failed;
    r.headline = "The disc could not be checked";
    r.detail = message.empty() ? "No details were reported." : message;
  }
  return r;
}

Result Ok() {
  Result r;
  r.verdict = Verdict::Ok;
  r.headline = "This is the right disc";
  r.detail = "Rockstar Games presents Table Tennis, USA / Europe retail release (Title ID 545407DF). The image is unmodified and complete.";
  r.raw = "Supported retail ISO verified.";
  return r;
}

Result VerifyWithPython(const fs::path& iso, const fs::path& repo_root) {
  const fs::path tools = repo_root / "project" / "tools";
  std::error_code ec;
  if (!fs::is_regular_file(tools / "disc_image.py", ec)) {
    Result r = Classify("disc_image.py missing");
    r.verdict = Verdict::ToolMissing;
    return r;
  }
  // -I keeps the interpreter from picking up anything from the environment or the current
  // directory; only the project's own tools directory is on the path.
  const std::string program =
      "import sys\n"
      "sys.path.insert(0, sys.argv[1])\n"
      "from disc_image import read_inputs\n"
      "try:\n"
      "    read_inputs(sys.argv[2])\n"
      "except Exception as error:\n"
      "    print(f'FAIL {type(error).__name__}: {error}')\n"
      "    sys.exit(1)\n"
      "print('OK')\n";
  std::string output;
  const int status = RunCapture({"python3", "-I", "-c", program, tools.string(), iso.string()}, output);
  if (status == 127) return Classify("python3 is not installed");
  const std::string last = LastLine(output);
  if (status == 0 && last == "OK") return Ok();
  std::string message = last;
  if (message.rfind("FAIL ", 0) == 0) message = message.substr(5);
  if (message.empty()) message = "python3 exited with status " + std::to_string(status);
  return Classify(message);
}

Result VerifyWithBackend(const fs::path& iso, const fs::path& backend) {
  std::string output;
  const int status = RunCapture({backend.string(), "--verify", iso.string()}, output);
  // Find the @@verify line; anything else is log text.
  std::string verdict_line;
  size_t start = 0;
  while (start < output.size()) {
    size_t end = output.find('\n', start);
    if (end == std::string::npos) end = output.size();
    const std::string line = output.substr(start, end - start);
    if (line.rfind("@@verify ", 0) == 0) verdict_line = line.substr(9);
    start = end + 1;
  }
  if (verdict_line.rfind("ok", 0) == 0) {
    Result r = Ok();
    const std::string description = verdict_line.size() > 3 ? verdict_line.substr(3) : "";
    if (!description.empty()) r.detail = description;
    return r;
  }
  if (verdict_line.rfind("fail", 0) == 0) {
    return Classify(verdict_line.size() > 5 ? verdict_line.substr(5) : "");
  }
  return Classify(status == 127 ? "The setup script could not be run"
                                : "The setup script gave no verdict (exit " + std::to_string(status) + ")");
}

}  // namespace

Result Verify(const fs::path& iso, const Setup& setup) {
  std::error_code ec;
  if (!fs::is_regular_file(iso, ec)) {
    return Classify("No such file: " + iso.string());
  }
  if (setup.ask_backend && !setup.backend.empty()) return VerifyWithBackend(iso, setup.backend);
  return VerifyWithPython(iso, setup.repo_root);
}

Checker::~Checker() {
  if (thread_.joinable()) thread_.join();
}

void Checker::Start(fs::path iso, Setup setup) {
  Reset();
  started_ = true;
  thread_ = std::thread([this, iso = std::move(iso), setup = std::move(setup)] {
    result_ = Verify(iso, setup);
    done_.store(true);
  });
}

Result Checker::Take() {
  if (thread_.joinable()) thread_.join();
  return result_;
}

void Checker::Reset() {
  if (thread_.joinable()) thread_.join();
  started_ = false;
  done_.store(false);
  result_ = Result{};
}

}  // namespace tt::iso
