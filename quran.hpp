#pragma once
#include <string>
#include <utility>
#include <vector>

#define TRANSLATION_COUNT 4

// The corpus lives here rather than in main, so that the command handlers and the
// model's tools share one copy of it instead of each capturing the whole Qur'an
extern std::pair<unsigned short, std::string> translations[TRANSLATION_COUNT];
extern std::string surahs[114];
extern std::vector<std::string> ayahs[TRANSLATION_COUNT][114];

std::string to_superscript(int number);
std::string verse_key(unsigned short surah, unsigned short ayah);
unsigned short clamp_surah(unsigned short surah);
unsigned short clamp_ayah(unsigned short surah, unsigned short ayah, unsigned short minimum_ayah = 1);
bool is_poetic_surah(unsigned short surah);

std::vector<std::pair<std::string, std::string>> search_quran(const std::string* surahs, const std::vector<std::string>* ayahs, std::string pattern, unsigned short& surah, unsigned short& ayah, unsigned short limit = 8);

// Backs the model's tools. Both render plain text for the model to read, and both
// clamp their arguments rather than failing, since the model supplies them.
std::string quote_quran(unsigned short surah, unsigned short first_ayah, unsigned short last_ayah, unsigned short translation = 0);
std::string search_quran_text(const std::string& pattern, unsigned short translation = 0, unsigned short limit = 8);
