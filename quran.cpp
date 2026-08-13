#include "quran.hpp"
#include "Polyweb/string.hpp"
#include <algorithm>

std::pair<unsigned short, std::string> translations[TRANSLATION_COUNT] = {
    {85, "M.A.S. Abdel Haleem"},
    {20, "Saheeh International"},
    {19, "M. Pickthall"},
    {22, "A. Yusuf Ali"},
};
std::string surahs[114];
std::vector<std::string> ayahs[TRANSLATION_COUNT][114];

std::string to_superscript(int number) {
    const static std::string superscripts[] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};

    std::string ret;
    std::string number_string = std::to_string(number);
    ret.reserve(number_string.size() * 4);
    for (char digit : number_string) {
        if (digit >= '0' && digit <= '9') {
            ret += superscripts[digit - '0'];
        } else {
            ret += digit;
        }
    }
    return ret;
}

std::string verse_key(unsigned short surah, unsigned short ayah) {
    return std::to_string(surah) + ':' + std::to_string(ayah);
}

unsigned short clamp_surah(unsigned short surah) {
    return std::min<unsigned short>(std::max<unsigned short>(surah, 1), 114);
}

unsigned short clamp_ayah(unsigned short surah, unsigned short ayah, unsigned short minimum_ayah) {
    // clang-format off
    unsigned short surah_sizes[114] = {
        7, 286, 200, 176, 120, 165, 206, 75, 129, 109, 123, 111, 43, 52, 99, 128, 111, 110, 98, 135,
        112, 78, 118, 64, 77, 227, 93, 88, 69, 60, 34, 30, 73, 54, 45, 83, 182, 88, 75, 85,
        54, 53, 89, 59, 37, 35, 38, 29, 18, 45, 60, 49, 62, 55, 78, 96, 29, 22, 24, 13,
        14, 11, 11, 18, 12, 12, 30, 52, 52, 44, 28, 28, 20, 56, 40, 31, 50, 40, 46, 42,
        29, 19, 36, 25, 22, 17, 19, 26, 30, 20, 15, 21, 11, 8, 8, 19, 5, 8, 8, 11,
        11, 8, 3, 9, 5, 4, 7, 3, 6, 3, 5, 4, 5, 6
    };
    // clang-format on
    return std::min(std::max(ayah, minimum_ayah), surah_sizes[clamp_surah(surah) - 1]);
}

bool is_poetic_surah(unsigned short surah) {
    return surah == 1 ||
           (surah >= 50 && surah <= 56) ||
           (surah >= 67 && surah <= 77) ||
           (surah >= 78 && surah <= 114);
}

std::vector<std::pair<std::string, std::string>> search_quran(const std::string* surahs, const std::vector<std::string>* ayahs, std::string pattern, unsigned short& surah, unsigned short& ayah, unsigned short limit) {
    pw::string::to_lower(pattern);

    std::vector<std::pair<std::string, std::string>> ret;
    for (; surah <= 114; ++surah) {
        for (; ayah <= ayahs[surah - 1].size(); ++ayah) {
            if (size_t match_pos = pw::string::to_lower_copy(ayahs[surah - 1][ayah - 1]).find(pattern); match_pos != std::string::npos) {
                std::string verse = ayahs[surah - 1][ayah - 1];
                verse.insert(match_pos, "**");
                verse.insert(match_pos + pattern.size() + 2, "**");
                ret.emplace_back("Surah " + surahs[surah - 1] + " (" + verse_key(surah, ayah) + ')', verse);

                if (ret.size() >= limit) {
                    return ret;
                }
            }
        }
        ayah = 1;
    }
    return ret;
}

namespace {
    bool corpus_loaded(unsigned short translation) {
        return translation < TRANSLATION_COUNT && !ayahs[translation][0].empty();
    }
} // namespace

std::string quote_quran(unsigned short surah, unsigned short first_ayah, unsigned short last_ayah, unsigned short translation) {
    if (translation >= TRANSLATION_COUNT) translation = 0;
    if (!corpus_loaded(translation)) {
        return "The Qur'an has not finished loading. Do not quote from memory; say that you could not retrieve the verse.";
    }

    surah = clamp_surah(surah);
    const auto& verses = ayahs[translation][surah - 1];
    if (verses.empty()) {
        return "That surah did not load in this translation. Say that you could not retrieve it rather than quoting from memory.";
    }

    first_ayah = clamp_ayah(surah, first_ayah);
    last_ayah = last_ayah ? clamp_ayah(surah, last_ayah, first_ayah) : first_ayah;
    // clamp_ayah bounds by the canonical count, but what actually loaded is the real
    // limit, and the model chooses these numbers
    if (first_ayah > verses.size()) first_ayah = verses.size();
    if (last_ayah > verses.size()) last_ayah = verses.size();

    std::string ret = "Surah " + surahs[surah - 1] + ' ' +
                      (first_ayah == last_ayah
                              ? verse_key(surah, first_ayah)
                              : verse_key(surah, first_ayah) + '-' + std::to_string(last_ayah)) +
                      ", translated by " + translations[translation].second + ":\n";
    for (unsigned short ayah = first_ayah; ayah <= last_ayah; ++ayah) {
        ret += std::to_string(ayah) + ". " + verses[ayah - 1] + '\n';
    }
    return ret;
}

std::string search_quran_text(const std::string& pattern, unsigned short translation, unsigned short limit) {
    if (translation >= TRANSLATION_COUNT) translation = 0;
    if (!corpus_loaded(translation)) {
        return "The Qur'an has not finished loading. Do not quote from memory; say that you could not run the search.";
    }

    unsigned short surah = 1;
    unsigned short ayah = 1;
    auto results = search_quran(surahs, ayahs[translation], pattern, surah, ayah, limit);
    if (results.empty()) {
        return "No verse in this translation contains that wording. Try a different word, and do not invent a verse.";
    }

    std::string ret;
    for (const auto& result : results) {
        ret += result.first + '\n' + result.second + "\n\n";
    }
    return ret;
}
