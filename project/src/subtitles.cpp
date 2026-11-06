// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// subtitles.cpp - see subtitles.h.
//
// Hooks (recompiled game functions, called on game threads):
//  - sub_821B3AE0(announcer, name): the announcer says a line. Returns 0 when it does not
//    (announcer off); the caption is shown only when it returns non-zero.
//  - sub_82162FF8(sound bank, name): looks up any named sound before it plays. With ALL SOUNDS,
//    crowd and fan sounds looked up during a match (pc::InGameplay) get a short tag. The crowd looks up a front
//    and a surround variant of each sound; a line already on screen is refreshed, not repeated.
#include "subtitles.h"

#include "achievement_ui.h"
#include "pc_features.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_STRING(subtitles, "off", "Tabletennis",
                      "Captions: off, announcer (what the announcer says) or all (also crowd "
                      "reactions)");

extern "C" void __imp__sub_821B3AE0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82162FF8(PPCContext& ctx, uint8_t* base);

namespace tabletennis::subtitles {
namespace {

using Clock = std::chrono::steady_clock;

// One player per nationality; line names use the nationality (FANS_<PLAYER>:..._<NATIONALITY>
// in the sound banks gives the pairs).
struct Player {
  const char* nationality;
  const char* code;  // as in SOLOMON_VS_LUC
  const char* name;  // as the game shows it
};
constexpr std::array<Player, 11> kPlayers = {{
    {"AMERICAN", "HALEY", "Haley"},
    {"AUSTRIAN", "JURGEN", "J\xC3\xBCrgen"},
    {"BRAZILIAN", "KARMEN", "Karmen"},
    {"CHINESE", "LIUPING", "Liu Ping"},
    {"EGYPTIAN", "SOLOMON", "Solayman"},
    {"ENGLISH", "SHANE", "Shane"},
    {"FRENCH", "LUC", "Luc"},
    {"IRISH", "CASSIDY", "Cassidy"},
    {"JAPANESE", "KUMI", "Kumi"},
    {"KOREAN", "JUNG", "Jung Soo"},
    {"SWEDISH", "HANS", "Hans"},
}};

// Player name for a nationality or player code (any case), or empty.
std::string PlayerName(std::string_view key) {
  for (const Player& p : kPlayers) {
    if (key.size() == std::strlen(p.nationality) &&
        std::equal(key.begin(), key.end(), p.nationality,
                   [](char a, char b) { return std::toupper((unsigned char)a) == b; })) {
      return p.name;
    }
    if (key.size() == std::strlen(p.code) &&
        std::equal(key.begin(), key.end(), p.code,
                   [](char a, char b) { return std::toupper((unsigned char)a) == b; })) {
      return p.name;
    }
  }
  // Korea appears once as KOREA.
  if (key == "KOREA") return "Jung Soo";
  return {};
}

bool AllDigits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Splits "A_x_B" around the first occurrence of sep; false if absent.
bool Split(std::string_view s, std::string_view sep, std::string_view& left, std::string_view& right) {
  const size_t at = s.find(sep);
  if (at == std::string_view::npos) return false;
  left = s.substr(0, at);
  right = s.substr(at + sep.size());
  return true;
}

std::string Plural(std::string_view number, const char* one, const char* many) {
  return std::string(number) + " " + (number == "1" ? one : many);
}

struct Caption {
  std::string text;
  int kind;
  Clock::time_point shown;  // first shown (fade in)
  Clock::time_point until;  // hidden after this (a repeat extends it)
};

std::mutex g_mutex;
std::deque<Caption> g_captions;

constexpr auto kAnnouncerHold = std::chrono::milliseconds(3200);
constexpr auto kTagHold = std::chrono::milliseconds(2200);
constexpr size_t kMaxCaptions = 3;

bool LogLines() {
  static const bool log = [] {
    const char* v = std::getenv("TT_SUBTITLE_LOG");
    return v && *v && *v != '0';
  }();
  return log;
}

}  // namespace

std::string Mode() {
  const std::string mode = REXCVAR_GET(subtitles);
  return mode == "announcer" || mode == "all" ? mode : "off";
}

std::string AnnouncerText(const std::string& full) {
  std::string_view line = full;
  if (const size_t colon = line.find(':'); colon != std::string_view::npos) {
    line = line.substr(colon + 1);
  }
  std::string_view a, b;

  // "Announcer_..." lines: serves, points, faults, games, matches and calls.
  if (StartsWith(line, "Announcer_")) {
    const std::string_view rest = line.substr(10);
    if (Split(rest, "serving", a, b) && AllDigits(a) && AllDigits(b)) {
      return std::string(a) + "\xE2\x80\x93" + std::string(b);
    }
    struct Named {
      const char* prefix;
      const char* before;
      const char* after;
    };
    static constexpr Named kNamed[] = {
        {"Serve_", "", " to serve."}, {"Point_", "Point, ", "."}, {"Fault_", "Fault, ", "."},
        {"Game_", "Game, ", "."},     {"MATCH_", "Match, ", "."},
    };
    for (const Named& n : kNamed) {
      if (StartsWith(rest, n.prefix)) {
        const std::string who = PlayerName(rest.substr(std::strlen(n.prefix)));
        if (!who.empty()) return n.before + who + n.after;
      }
    }
    if (rest == "LetServe") return "Let.";
    if (rest == "NET") return "Net.";
    if (rest == "OUT") return "Out.";
    if (rest == "ScoreTied") return "Score tied.";
    return {};
  }

  // Score "7_5", games "2_GAME_1".
  if (Split(line, "_GAME_", a, b) && AllDigits(a) && AllDigits(b)) {
    return "Games: " + std::string(a) + "\xE2\x80\x93" + std::string(b) + ".";
  }
  if (Split(line, "_", a, b) && AllDigits(a) && AllDigits(b)) {
    return std::string(a) + "\xE2\x80\x93" + std::string(b);
  }

  if (line == "GAMEPOINT") return "Game point.";
  if (line == "MATCHPOINT") return "Match point.";
  if (line == "FINALGAME") return "Final game.";
  if (line == "DEUCE") return "Deuce.";
  if (line == "POINT") return "Point.";
  if (line == "QUARTERFINAL") return "Quarter-final.";
  if (line == "SEMIFINAL") return "Semi-final.";
  if (line == "FINAL") return "Final.";
  if (line == "ROUND_TIE") return "The round is tied.";
  if (line == "TOURNAMENT_TIE") return "The tournament is tied.";
  if (StartsWith(line, "GAME") && AllDigits(line.substr(4))) {
    return "Game " + std::string(line.substr(4)) + ".";
  }
  if (StartsWith(line, "ADVANTAGE_")) {
    const std::string who = PlayerName(line.substr(10));
    if (!who.empty()) return "Advantage, " + who + ".";
  }
  if (StartsWith(line, "ROUND_WIN_")) {
    const std::string who = PlayerName(line.substr(10));
    if (!who.empty()) return who + " wins the round.";
  }
  if (StartsWith(line, "TOURNAMENT_WIN_")) {
    const std::string who = PlayerName(line.substr(15));
    if (!who.empty()) return who + " wins the tournament.";
  }

  // Match formats: BESTOF5_11POINTS, SINGLEGAME_21POINTS, 11POINTGAME, 7POINTS_5MINUTES,
  // 10MINUTES_ONCLOCK.
  if (StartsWith(line, "BESTOF") && Split(line.substr(6), "_", a, b) && AllDigits(a) &&
      StartsWith(b, "") && b.size() > 6 && b.substr(b.size() - 6) == "POINTS" &&
      AllDigits(b.substr(0, b.size() - 6))) {
    return "Best of " + std::string(a) + " games, " + std::string(b.substr(0, b.size() - 6)) +
           " points each.";
  }
  if (StartsWith(line, "SINGLEGAME_") && line.size() > 17 && line.substr(line.size() - 6) == "POINTS") {
    return "A single game to " + std::string(line.substr(11, line.size() - 17)) + " points.";
  }
  if (line.size() > 9 && line.substr(line.size() - 9) == "POINTGAME" &&
      AllDigits(line.substr(0, line.size() - 9))) {
    return "Game to " + std::string(line.substr(0, line.size() - 9)) + " points.";
  }
  if (Split(line, "POINTS_", a, b) && AllDigits(a) && b.size() > 7 &&
      b.substr(b.size() - 7) == "MINUTES" && AllDigits(b.substr(0, b.size() - 7))) {
    return Plural(a, "point", "points") + " or " +
           Plural(b.substr(0, b.size() - 7), "minute", "minutes") + ".";
  }
  if (Split(line, "MINUTES_ONCLOCK", a, b) && AllDigits(a) && b.empty()) {
    return Plural(a, "minute", "minutes") + " on the clock.";
  }

  // Players: LIUPING_VS_LUC, CHINESE_VS_FRENCH, CHINESE_lead_FRENCH, CHINESE_trail_FRENCH,
  // CHINESE_UP_3, CHINESE_DOWN_2, CHINESE_LEAD.
  if (Split(line, "_VS_", a, b)) {
    const std::string left = PlayerName(a), right = PlayerName(b);
    if (!left.empty() && !right.empty()) return left + " versus " + right + ".";
  }
  if (Split(line, "_lead_", a, b)) {
    const std::string left = PlayerName(a), right = PlayerName(b);
    if (!left.empty() && !right.empty()) return left + " leads " + right + ".";
  }
  if (Split(line, "_trail_", a, b)) {
    const std::string left = PlayerName(a), right = PlayerName(b);
    if (!left.empty() && !right.empty()) return left + " trails " + right + ".";
  }
  if (Split(line, "_UP_", a, b) && AllDigits(b)) {
    const std::string who = PlayerName(a);
    if (!who.empty()) return who + " leads by " + Plural(b, "game", "games") + ".";
  }
  if (Split(line, "_DOWN_", a, b) && AllDigits(b)) {
    const std::string who = PlayerName(a);
    if (!who.empty()) return who + " trails by " + Plural(b, "game", "games") + ".";
  }
  if (line.size() > 5 && line.substr(line.size() - 5) == "_LEAD") {
    const std::string who = PlayerName(line.substr(0, line.size() - 5));
    if (!who.empty()) return who + " leads.";
  }
  // BOREDOM_xx and other lines whose wording is not known have no caption.
  return {};
}

std::string CrowdText(const std::string& full) {
  std::string_view sound = full;
  const size_t colon = sound.find(':');
  if (colon == std::string_view::npos) return {};
  const std::string_view bank = sound.substr(0, colon);
  const std::string_view name = sound.substr(colon + 1);
  // The crowd bank depends on the venue's size: CROWD_RS, SM_CROWD_RS, PSM_CROWD_RS.
  if (bank.size() >= 8 && bank.substr(bank.size() - 8) == "CROWD_RS") {
    struct Tag {
      const char* prefix;
      const char* text;
    };
    // Longest prefixes first where one extends another.
    static constexpr Tag kTags[] = {
        {"ENDING_APPLAUSE", "[applause]"},
        {"ENDING_CHEER", "[crowd cheers]"},
        {"CHEER_MATCH", "[crowd cheers]"},
        {"CHEER_GAME", "[crowd cheers]"},
        {"CHEER_LG", "[crowd cheers]"},
        {"CHEER_MD", "[crowd cheers]"},
        {"CHEER_BEERDRUNK", "[rowdy cheering]"},
        {"CHEER", "[cheering]"},
        {"APPLAUSE", "[applause]"},
        {"SPIKE_GASP", "[crowd gasps]"},
        {"SPIKE_OH", "[crowd: \xE2\x80\x9Coh!\xE2\x80\x9D]"},
        {"SPIKE_OO", "[crowd: \xE2\x80\x9Cooh!\xE2\x80\x9D]"},
        {"SHOCKED", "[crowd gasps]"},
        {"FAULT_OUT_NEG", "[crowd groans]"},
        {"FAULT_SHORT_NEG", "[crowd groans]"},
        {"FAULT_OUT_POS", "[crowd cheers]"},
        {"FAULT_SHORT_POS", "[crowd cheers]"},
        {"BODYFALL", "[crowd laughs]"},
        {"FAN_GENERIC", "[fans cheer]"},
    };
    for (const Tag& t : kTags) {
      if (StartsWith(name, t.prefix)) return t.text;
    }
    return {};
  }
  if (StartsWith(bank, "FANS_")) {
    const std::string who = PlayerName(bank.substr(5));
    if (who.empty()) return {};
    if (StartsWith(name, "ADM_")) return "[" + who + "'s fans cheer]";
    if (StartsWith(name, "ENC_")) return "[" + who + "'s fans shout encouragement]";
    if (StartsWith(name, "HECKLE_")) return "[heckling]";
    return {};
  }
  // The venue's crowd voices: AMB_<VENUE>:HECKLE_.., BOREDOM_.., SHUT_UP_..
  if (StartsWith(bank, "AMB_")) {
    if (StartsWith(name, "HECKLE_")) return "[heckling]";
    if (StartsWith(name, "BOREDOM_")) return "[bored muttering]";
    if (StartsWith(name, "SHUT_UP_")) return "[spectator shouts]";
  }
  return {};
}

void Show(const std::string& text, int kind) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = Clock::now();
  const auto until = now + (kind ? kTagHold : kAnnouncerHold);
  // A line already on screen stays up longer instead of being shown twice (the crowd's front
  // and surround variants, a second fault in a row).
  for (Caption& c : g_captions) {
    if (c.text == text) {
      c.until = until;
      return;
    }
  }
  g_captions.push_back({text, kind, now, until});
  while (g_captions.size() > kMaxCaptions) g_captions.pop_front();
}

Overlay::Overlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
Overlay::~Overlay() = default;

void Overlay::OnDraw(ImGuiIO& io) {
  // Preview: TT_TEST_SUBTITLE=1 keeps a sample announcer line and crowd tag on screen.
  static const bool preview = [] {
    const char* v = std::getenv("TT_TEST_SUBTITLE");
    return v && *v && *v != '0';
  }();
  if (preview) {
    Show(AnnouncerText("ANNOUNCER:Announcer_Point_CHINESE"), 0);
    Show(CrowdText("SM_CROWD_RS:CHEER_LG_FRNT01"), 1);
  }
  std::deque<Caption> captions;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto now = Clock::now();
    g_captions.erase(std::remove_if(g_captions.begin(), g_captions.end(),
                                    [&](const Caption& c) { return now > c.until; }),
                     g_captions.end());
    if (Mode() == "off") g_captions.clear();
    captions = g_captions;
  }
  if (captions.empty()) return;

  // Bottom centre of the game picture, above the HUD's bottom edge.
  const float aspect = float(tabletennis::pc::OverlayAspect());
  const float pic_w = std::min(io.DisplaySize.x, io.DisplaySize.y * aspect);
  const float pic_h = pic_w / aspect;
  const float pic_x = (io.DisplaySize.x - pic_w) * 0.5f;
  const float pic_y = (io.DisplaySize.y - pic_h) * 0.5f;
  const float s = std::max(pic_h / 720.0f, 0.6f);
  ImFont* font = achievement_ui::HeadlineFont(28.0f * s);
  const float px = 28.0f * s;
  const float pad_x = 16.0f * s, pad_y = 6.0f * s, gap = 6.0f * s;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float y = pic_y + pic_h - 70.0f * s;
  const auto now = Clock::now();
  for (auto it = captions.rbegin(); it != captions.rend(); ++it) {
    const float age = std::chrono::duration<float>(now - it->shown).count();
    const float left = std::chrono::duration<float>(it->until - now).count();
    const float alpha = std::clamp(std::min(age / 0.15f, left / 0.4f), 0.0f, 1.0f);
    const ImVec2 size = font->CalcTextSizeA(px, pic_w * 0.9f, 0.0f, it->text.c_str());
    const float x0 = pic_x + (pic_w - size.x) * 0.5f - pad_x;
    const float y0 = y - size.y - 2.0f * pad_y;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + size.x + 2.0f * pad_x, y),
                      ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.72f * alpha)), 6.0f * s);
    const ImVec4 colour = it->kind ? ImVec4(0.78f, 0.84f, 0.92f, alpha) : ImVec4(1.0f, 1.0f, 1.0f, alpha);
    dl->AddText(font, px, ImVec2(x0 + pad_x, y0 + pad_y), ImGui::GetColorU32(colour), it->text.c_str(),
                nullptr, pic_w * 0.9f);
    y = y0 - gap;
  }
}

}  // namespace tabletennis::subtitles

namespace {

std::string GuestString(uint8_t* base, uint32_t address) {
  if (!address) return {};
  const char* text = reinterpret_cast<const char*>(base + address);
  return std::string(text, strnlen(text, 128));
}

}  // namespace

// The announcer says a line (see the top of this file).
extern "C" void sub_821B3AE0(PPCContext& ctx, uint8_t* base) {
  const uint32_t name_address = ctx.r4.u32;
  const std::string mode = tabletennis::subtitles::Mode();
  const bool log = tabletennis::subtitles::LogLines();
  const std::string name = (mode != "off" || log) ? GuestString(base, name_address) : std::string();
  __imp__sub_821B3AE0(ctx, base);
  if (name.empty()) return;
  const bool spoken = ctx.r3.u32 != 0;
  const std::string text = tabletennis::subtitles::AnnouncerText(name);
  if (log) {
    REXLOG_INFO("Tabletennis: announcer {} {} -> \"{}\"", name, spoken ? "spoken" : "skipped", text);
  }
  if (spoken && mode != "off") tabletennis::subtitles::Show(text, 0);
}

// Named sound lookup (see the top of this file).
extern "C" void sub_82162FF8(PPCContext& ctx, uint8_t* base) {
  const uint32_t name_address = ctx.r4.u32;
  const bool log = tabletennis::subtitles::LogLines();
  if ((log || tabletennis::subtitles::Mode() == "all") && tabletennis::pc::InGameplay()) {
    const std::string name = GuestString(base, name_address);
    const std::string text = tabletennis::subtitles::CrowdText(name);
    if (log && !name.empty() && !text.empty()) {
      REXLOG_INFO("Tabletennis: sound {} -> \"{}\"", name, text);
    }
    if (tabletennis::subtitles::Mode() == "all") tabletennis::subtitles::Show(text, 1);
  }
  __imp__sub_82162FF8(ctx, base);
}
