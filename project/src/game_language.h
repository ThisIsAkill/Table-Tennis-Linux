// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// Portions adapted from davidptrssvea/Table-Tennis-Rockstar-PC (game language selection,
// v1.6.0), Copyright (c) 2026 David Petersson, MIT License.
//
// GAME LANGUAGE: the retail USA/Europe disc carries English, French, German, Spanish and Italian
// text. The game picks one from XGetLanguage, which the stock engine answers with English; the
// game_language setting (en, fr, de, es, it; restart required) chooses instead. The engine's
// user_language is set to the same Xbox language so the rest of the system agrees.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tabletennis::language {

// Xbox system language IDs (not the game's internal localization indices).
constexpr uint32_t XboxId(std::string_view code) {
  if (code == "de") return 3;
  if (code == "fr") return 4;
  if (code == "es") return 5;
  if (code == "it") return 6;
  return 1;  // English, also for anything unknown
}

// The chosen language code: "en", "fr", "de", "es" or "it".
std::string Code();

// Sets the engine's user_language from game_language and logs the choice (startup).
void Apply();

}  // namespace tabletennis::language
