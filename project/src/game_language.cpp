// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// Portions adapted from davidptrssvea/Table-Tennis-Rockstar-PC (game language selection,
// v1.6.0), Copyright (c) 2026 David Petersson, MIT License.
// game_language.cpp - see game_language.h.
#include "game_language.h"

#include "l10n.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_STRING(game_language, "en", "UI", "Game language: en, fr, de, es, it (restart required)")
    .allowed({"en", "fr", "de", "es", "it"});

namespace tabletennis::language {

std::string Code() {
  const std::string code = rex::cvar::GetFlagByName("game_language");
  return XboxId(code) == 1 ? "en" : code;
}

void Apply() {
  const std::string code = Code();
  const uint32_t id = XboxId(code);
  rex::cvar::SetFlagByName("user_language", std::to_string(id));
  l10n::SetLanguage(code);
  REXLOG_INFO("Game language: {} (Xbox language {})", code, id);
}

}  // namespace tabletennis::language

// XGetLanguage, redirected here by a native edit in tabletennis_pch.h (direct calls) and through
// the __imp__ name (registered guest thunks).
extern "C" void Tabletennis_XGetLanguage_Hook(PPCContext& ctx, uint8_t*) {
  ctx.r3.u64 = tabletennis::language::XboxId(tabletennis::language::Code());
}
extern "C" void __imp__Tabletennis_XGetLanguage_Hook(PPCContext& ctx, uint8_t* base) {
  Tabletennis_XGetLanguage_Hook(ctx, base);
}
