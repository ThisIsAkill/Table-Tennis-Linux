#include "generated/default/tabletennis_init.h"
#include "system_link.h"
#include "pc_settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>

#include <atomic>
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
// The front end's requestOnline (frontend object in r3): runs when ONLINE or SYSTEM LINK is
// chosen in the main menu, before the game's online menu opens.
extern "C" void __imp__sub_822FD580(PPCContext& ctx, uint8_t* base);
// Local gamer update (gamer object in r3), run regularly: it re-reads the gamer's XUID and peer
// descriptor (its XNADDR, sub_82373650 on gamer+40) when the sign-in changes.
extern "C" void __imp__sub_823697F8(PPCContext& ctx, uint8_t* base);
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

namespace {
// The menu patch leaves the chosen main menu row (3 ONLINE, 4 SYSTEM LINK) in
// iCurrChooseModeSel, which the PLAYER MATCH screen otherwise only sets to 0-2; the front end
// resets it when the online menu opens, so it is read every frame and kept here.
std::atomic<int> g_main_menu_choice{-1};

// Reads the row from the front-end movie and sets the variable back to 0, the same
// check-and-clear the game does for its own flags.
int TakeMainMenuRow(PPCContext& ctx, uint8_t* base, uint32_t movie) {
  static constexpr char name_text[] = "iCurrChooseModeSel";
  static uint32_t name = 0;  // one guest copy of the name (only one variable is read here)
  if (!name) {
    const size_t size = std::strlen(name_text) + 1;
    name = rex::system::kernel_memory()->SystemHeapAlloc(uint32_t(size));
    if (!name) return -1;
    std::memcpy(base + name, name_text, size);
  }
  PPCContext call = ctx;
  call.r3.u64 = movie;
  call.r4.u64 = name;
  sub_823F9280(call, base);
  if (!call.r3.u32) return -1;
  const uint32_t slot = call.r3.u32;
  call = ctx;
  call.r3.u64 = slot;
  sub_823FA5C8(call, base);
  const int value = call.r3.s32;
  if (value != 3 && value != 4) return -1;
  call = ctx;
  call.r3.u64 = movie;
  call.r4.u64 = name;
  sub_823F9318(call, base);
  if (call.r3.u32) {
    REX_STORE_U32(call.r3.u32 + 0, 0);
    REX_STORE_U32(call.r3.u32 + 4, 3);
  }
  return value;
}
}  // namespace

void RefreshPeerDescriptor(PPCContext& ctx, uint8_t* base);

void tabletennis::lan::PollFrontend(PPCContext& ctx, uint8_t* base, uint32_t movie) {
  if (!movie || !Enabled()) return;
  RefreshPeerDescriptor(ctx, base);
  const int row = TakeMainMenuRow(ctx, base, movie);
  if (row >= 0) g_main_menu_choice.store(row);
}

// The main menu has ONLINE (row 3) and SYSTEM LINK (row 4); both open the game's online menu.
// The menu patch leaves the chosen row in iCurrChooseModeSel (patch_menu.py, _add_online).
// ONLINE plays through the online server and first asks how (pc_settings OnlinePrompt);
// SYSTEM LINK plays on the local network.
extern "C" void sub_822FD580(PPCContext& ctx, uint8_t* base) {
  if (tabletennis::lan::Enabled()) {
    const int row = g_main_menu_choice.exchange(-1);
    if (row == 3) {
      tabletennis::pc_settings::OpenOnlinePrompt();
    } else {
      REXLOG_INFO("Tabletennis: SYSTEM LINK chosen (local network, main menu row {})", row);
      rex::system::net::SetOnline("off", "");
    }
  }
  __imp__sub_822FD580(ctx, base);
}

// The game reads its own address (XNetGetTitleXnAddr) into the local peer descriptor at sign-in
// and keeps it. Online play gives the machine a room address, so the descriptor is read again
// whenever the address changes (from the front end's frame, below), as the game itself does
// after a sign-in change. The local gamer object is remembered from the game's own update.
namespace {
std::atomic<uint32_t> g_local_gamer{0};
std::atomic<uint32_t> g_descriptor_address{0};
}  // namespace

extern "C" void sub_823697F8(PPCContext& ctx, uint8_t* base) {
  const uint32_t gamer = ctx.r3.u32;
  uint32_t expected = 0;
  g_local_gamer.compare_exchange_strong(expected, gamer);  // the first one is user 0
  __imp__sub_823697F8(ctx, base);
  g_descriptor_address.store(rex::system::net::LocalAddress());
}

void RefreshPeerDescriptor(PPCContext& ctx, uint8_t* base) {
  const uint32_t gamer = g_local_gamer.load();
  if (!gamer) return;
  const uint32_t address = rex::system::net::LocalAddress();
  const uint32_t previous = g_descriptor_address.exchange(address);
  if (!previous || previous == address) return;
  REXLOG_INFO("Tabletennis: own network address changed, reading the peer descriptor again");
  PPCContext call = ctx;
  call.r3.u64 = gamer + 40;
  sub_82373650(call, base);
}
