// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// Translations of this port's own text (PC SETTINGS, the ONLINE prompt, achievements, photo mode,
// captions, notices) into the game's other languages, following GAME LANGUAGE.
//
// Text stays written in English in the code; T("English") returns the translation for the
// current language, or the English text when there is none. Text with values in it uses {}:
// F("UPDATE TO {}", version). N("English") marks text kept in tables and translated where it is
// drawn (T(row.label)); it is the text itself.
//
// The translations live in project/localization/*.json as rows [en, fr, de, es, it];
// project/tools/localization.py turns them into localization_data.inc and checks that every
// T/F/N text in the sources has a row.
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

#define N(text) text

namespace tabletennis::l10n {

// "en", "fr", "de", "es" or "it"; anything else is English. Call once at startup.
void SetLanguage(std::string_view code);
// 0 English, 1 French, 2 German, 3 Spanish, 4 Italian.
int Language();

// The translation of `english`, or `english` itself. The result lives as long as the program.
const char* T(const char* english);
// T(english) with each {} replaced by the next value.
std::string F(const char* english, std::initializer_list<std::string_view> values);

}  // namespace tabletennis::l10n
