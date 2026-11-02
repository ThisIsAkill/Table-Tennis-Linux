#include "generated/default/tabletennis_init.h"
#include "system_link.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(system_link, true, "Network",
                    "System link: play the game's online modes against other PCs and Steam Decks "
                    "on the local network");

// Session layer entry points that read the forcesyslink parameter (rage::sysParam at
// 0x825D0FDC, value pointer at +4): FindSessions (broadcast search instead of LIVE matchmaking),
// and the host and join paths (system link session flags).
extern "C" void __imp__sub_823AD528(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823AD608(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823AD730(PPCContext& ctx, uint8_t* base);
// pongLiveManager's session notification handler (this, session, code, data).
extern "C" void __imp__sub_823AE760(PPCContext& ctx, uint8_t* base);
// The generated code's import is redirected to the hook below (tabletennis_pch.h); this file
// calls the engine's implementation.
#undef __imp__XamUserGetXUID
extern "C" REX_FUNC(__imp__XamUserGetXUID);

namespace tabletennis::lan {
namespace {
constexpr uint32_t kForceSyslinkValue = 0x825D0FE0;
constexpr uint32_t kForceSyslinkName = 0x8206D7A8;  // any non-null string counts as set

void ApplyForceSyslink(uint8_t* base) {
  const uint32_t value = Enabled() ? kForceSyslinkName : 0;
  if (REX_LOAD_U32(kForceSyslinkValue) != value) {
    REX_STORE_U32(kForceSyslinkValue, value);
    REXLOG_INFO("Tabletennis: system link sessions {}", value ? "on" : "off");
  }
}
}  // namespace

// Every copy of the game has the same local profile XUID, and the session layer keys players by
// XUID, so a peer with our own XUID would be taken for ourselves. Give the player an XUID
// that is unique to this machine (and network address, for several instances on one machine).
// Saves are stored by the engine's profile, not by this value, so they are unaffected.
uint64_t MachineXuid(uint64_t profile_xuid) {
  static const uint32_t id = [] {
    std::string seed;
    std::ifstream machine_id("/etc/machine-id");
    std::getline(machine_id, seed);
#if defined(_WIN32)
    if (const char* host = std::getenv("COMPUTERNAME")) seed += host;
#else
    char host[256] = {};
    gethostname(host, sizeof(host) - 1);
    seed += host;
#endif
    seed += rex::cvar::GetFlagByName("net_address");
    uint32_t hash = 2166136261u;  // FNV-1a
    for (unsigned char c : seed) hash = (hash ^ c) * 16777619u;
    return hash ? hash : 1;
  }();
  return (profile_xuid & 0xFFFFFFFF00000000ull) | id;
}

bool Enabled() {
  return REXCVAR_GET(system_link);
}


}  // namespace tabletennis::lan

extern "C" void sub_823AD528(PPCContext& ctx, uint8_t* base) {
  tabletennis::lan::ApplyForceSyslink(base);
  __imp__sub_823AD528(ctx, base);
}

extern "C" void sub_823AD608(PPCContext& ctx, uint8_t* base) {
  tabletennis::lan::ApplyForceSyslink(base);
  __imp__sub_823AD608(ctx, base);
}

extern "C" void sub_823AD730(PPCContext& ctx, uint8_t* base) {
  tabletennis::lan::ApplyForceSyslink(base);
  __imp__sub_823AD730(ctx, base);
}

// Before a match the lobby waits until the session reports both "started" (notification 14)
// and "arbitrated" (notification 12) on every machine. Arbitration is an Xbox LIVE service for
// ranked matches; system link sessions are created without it and never send notification 12,
// so the lobby would wait forever with "CONNECTING" on both screens. With system link, a
// successful start also counts as arbitrated.
extern "C" void sub_823AE760(PPCContext& ctx, uint8_t* base) {
  const PPCContext call = ctx;
  __imp__sub_823AE760(ctx, base);
  if (tabletennis::lan::Enabled() && call.r5.u32 == 14) {
    PPCContext arbitrated = call;
    arbitrated.r5.u64 = 12;
    __imp__sub_823AE760(arbitrated, base);
  }
}

// XamUserGetXUID(user_index, type_mask, xuid_ptr)
extern "C" void Tabletennis_XamUserGetXUID_Hook(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t xuid_ptr = ctx.r5.u32;
  __imp__XamUserGetXUID(ctx, base);
  if (tabletennis::lan::Enabled() && user == 0 && ctx.r3.u32 == 0 && xuid_ptr) {
    const uint64_t xuid = REX_LOAD_U64(xuid_ptr);
    if (xuid) REX_STORE_U64(xuid_ptr, tabletennis::lan::MachineXuid(xuid));
  }
}

extern "C" void __imp__Tabletennis_XamUserGetXUID_Hook(PPCContext& ctx, uint8_t* base) {
  Tabletennis_XamUserGetXUID_Hook(ctx, base);
}

// Diagnostics: TT_GAME_LOG=1 sends the game's own debug messages to the log. The retail build
// keeps Rockstar's debug print calls but links them all to one empty function (sub_8240E6D0),
// whose callers pass either (format, ...) or (buffer, format, ...). The network session code
// reports every step through it, which is the best way to see why a join or match fails.
namespace {
bool GameLogEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("TT_GAME_LOG");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

bool ReadGuestString(uint8_t* base, uint32_t address, std::string* out) {
  // Format strings live in the executable's read-only data.
  if (address < 0x82000000 || address >= 0x83000000) return false;
  out->clear();
  for (int i = 0; i < 512; ++i) {
    const uint8_t c = REX_LOAD_U8(address + i);
    if (!c) return out->size() >= 2;
    if ((c < 0x20 && c != '\n' && c != '\t') || c >= 0x7F) return false;
    out->push_back(char(c));
  }
  return false;
}

bool ReadGuestArgString(uint8_t* base, uint32_t address, std::string* out) {
  out->clear();
  if (address < 0x40000000 || address >= 0xC0000000) return false;
  for (int i = 0; i < 256; ++i) {
    const uint8_t c = REX_LOAD_U8(address + i);
    if (!c) return true;
    out->push_back(c >= 0x20 && c < 0x7F ? char(c) : '?');
  }
  return true;
}
}  // namespace

extern "C" void sub_8240E6D0(PPCContext& ctx, uint8_t* base) {
  if (!GameLogEnabled()) return;
  const uint64_t regs[8] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
                            ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  std::string format;
  int next;
  if (ReadGuestString(base, ctx.r3.u32, &format)) {
    next = 1;
  } else if (ReadGuestString(base, ctx.r4.u32, &format)) {
    next = 2;
  } else {
    return;
  }
  std::string out;
  for (size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%') {
      if (format[i] != '\n') out.push_back(format[i]);
      continue;
    }
    size_t j = i + 1;
    while (j < format.size() && std::strchr("-+ #0123456789.lhI6", format[j])) ++j;
    if (j >= format.size()) break;
    const char conversion = format[j];
    i = j;
    if (conversion == '%') {
      out.push_back('%');
      continue;
    }
    if (conversion == 'f' || conversion == 'g') {  // floating point arguments use FPRs
      out += "<float>";
      continue;
    }
    const uint64_t value = next < 8 ? regs[next++] : 0;
    std::string text;
    switch (conversion) {
      case 's': out += ReadGuestArgString(base, uint32_t(value), &text) ? text : "?"; break;
      case 'd': case 'i': out += std::to_string(int32_t(value)); break;
      case 'u': out += std::to_string(uint32_t(value)); break;
      case 'x': case 'X': case 'p': out += fmt::format("{:x}", value); break;
      case 'c': out.push_back(char(value)); break;
      default: out += '?'; break;
    }
  }
  REXLOG_INFO("Game: {}", out);
}
