// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// l10n.cpp - see l10n.h.
#include "l10n.h"

#include <atomic>
#include <string>
#include <unordered_map>

namespace tabletennis::l10n {
namespace {

struct Entry {
  const char* text[5];  // en, fr, de, es, it
};

constexpr Entry kEntries[] = {
#include "localization_data.inc"
};

std::atomic<int> g_language{0};

const std::unordered_map<std::string_view, const Entry*>& Table() {
  static const auto* table = [] {
    auto* map = new std::unordered_map<std::string_view, const Entry*>();
    map->reserve(sizeof(kEntries) / sizeof(kEntries[0]));
    for (const Entry& entry : kEntries) map->emplace(entry.text[0], &entry);
    return map;
  }();
  return *table;
}

}  // namespace

void SetLanguage(std::string_view code) {
  g_language.store(code == "fr" ? 1 : code == "de" ? 2 : code == "es" ? 3 : code == "it" ? 4 : 0);
}

int Language() { return g_language.load(std::memory_order_relaxed); }

const char* T(const char* english) {
  const int language = Language();
  if (!english || language == 0) return english;
  const auto& table = Table();
  const auto it = table.find(english);
  if (it == table.end()) return english;
  const char* text = it->second->text[language];
  return text && *text ? text : english;
}

std::string F(const char* english, std::initializer_list<std::string_view> values) {
  const std::string_view pattern = T(english);
  std::string out;
  out.reserve(pattern.size() + 16);
  auto value = values.begin();
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
      if (value != values.end()) out += *value++;
      ++i;
    } else {
      out += pattern[i];
    }
  }
  return out;
}

}  // namespace tabletennis::l10n
