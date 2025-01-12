#include "Polyweb/polyweb.hpp"
#include "json.hpp"
#include <dpp/dpp.h>
#include <iostream>
#include <regex>
#include <sstream>
#include <stddef.h>
#include <stdexcept>
#include <stdlib.h>
#include <string>

using nlohmann::json;

std::string to_superscript(int number) {
    const static std::string superscripts[] = {"\u2070", "\u00B9", "\u00B2", "\u00B3", "\u2074", "\u2075", "\u2076", "\u2077", "\u2078", "\u2079"};

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

std::string surah_name(int surah) {
    // clang-format off
    static const std::string surah_names[] = {
        {}, "Al-Fatiha", "Al-Baqarah", "Aal-E-Imran", "An-Nisa", "Al-Ma'idah", "Al-An'am",
        "Al-A'raf", "Al-Anfal", "At-Tawbah", "Yunus", "Hud", "Yusuf", "Ar-Ra'd", "Ibrahim",
        "Al-Hijr", "An-Nahl", "Al-Isra", "Al-Kahf", "Maryam", "Ta-Ha", "Al-Anbiya", "Al-Hajj",
        "Al-Mu'minun", "An-Nur", "Al-Furqan", "Ash-Shu'ara", "An-Naml", "Al-Qasas", "Al-Ankabut",
        "Ar-Rum", "Luqman", "As-Sajda", "Al-Ahzab", "Saba", "Fatir", "Ya-Sin", "As-Saffat",
        "Sad", "Az-Zumar", "Ghafir", "Fussilat", "Ash-Shura", "Az-Zukhruf", "Ad-Dukhan",
        "Al-Jathiya", "Al-Ahqaf", "Muhammad", "Al-Fath", "Al-Hujurat", "Qaf", "Adh-Dhariyat",
        "At-Tur", "An-Najm", "Al-Qamar", "Ar-Rahman", "Al-Waqia", "Al-Hadid", "Al-Mujadila",
        "Al-Hashr", "Al-Mumtahina", "As-Saff", "Al-Jumu'a", "Al-Munafiqun", "At-Taghabun",
        "At-Talaq", "At-Tahrim", "Al-Mulk", "Al-Qalam", "Al-Haqqa", "Al-Ma'arij", "Nuh",
        "Al-Jinn", "Al-Muzzammil", "Al-Muddaththir", "Al-Qiyama", "Al-Insan", "Al-Mursalat",
        "An-Naba", "An-Nazi'at", "Abasa", "At-Takwir", "Al-Infitar", "Al-Mutaffifin", "Al-Inshiqaq",
        "Al-Buruj", "At-Tariq", "Al-A'la", "Al-Ghashiya", "Al-Fajr", "Al-Balad", "Ash-Shams",
        "Al-Lail", "Ad-Duhaa", "Ash-Sharh", "At-Tin", "Al-Alaq", "Al-Qadr", "Al-Bayyina",
        "Az-Zalzala", "Al-Adiyat", "Al-Qari'a", "At-Takathur", "Al-Asr", "Al-Humaza", "Al-Fil",
        "Quraysh", "Al-Ma'un", "Al-Kawthar", "Al-Kafirun", "An-Nasr", "Al-Masad", "Al-Ikhlas",
        "Al-Falaq", "An-Nas",
    };
    // clang-format on

    if (surah >= 1 && surah <= 114) {
        return surah_names[surah];
    } else {
        throw std::invalid_argument("Invalid surah number");
    }
}

int main() {
    std::string quran[115][287];

    for (unsigned short i = 0; i < 116; ++i) {
        for (unsigned short j = 0; j < 287; ++j) {
            quran[i][j] = "⚠️ This verse does not exist. ⚠️";
        }
    }

    for (unsigned short surah = 1; surah <= 114; ++surah) {
        std::cout << "Downloading surah " << surah << "..." << std::endl;

        pw::HTTPResponse resp;
        if (pw::fetch("https://api.quran.com/api/v4/verses/by_chapter/" + std::to_string(surah) + "?translations=131&limit=286", resp, {{"Accept", "application/json"}}) == PN_ERROR) {
            std::cerr << "Error: Request to api.quran.com failed: " << pw::universal_strerror() << std::endl;
            return EXIT_FAILURE;
        } else if (resp.status_code_category() != 200) {
            std::cerr << "Error: Request to api.quran.com failed with status code " << resp.status_code << std::endl;
            return EXIT_FAILURE;
        }

        json resp_json = json::parse(resp.body_string());
        for (const auto& verse : resp_json["verses"].items()) {
            static std::regex footnote_regex(R"(<sup foot_note=\d+>\d+</sup>)");
            std::string text = std::regex_replace(verse.value()["translations"][0]["text"].get<std::string>(), footnote_regex, "");
            quran[surah][verse.value()["verse_number"].get<unsigned short>()] = text;
        }

        std::cout << "Downloaded surah " << surah << '!' << std::endl;
    }

    dpp::cluster bot(getenv("QURAN_BOT_TOKEN"));
    bot.on_log(dpp::utility::cout_logger());

    bot.on_ready([&bot](const auto& event) {
        if (dpp::run_once<struct RegisterBotCommands>()) {
            dpp::slashcommand quote_command("quote", "Quote the Holy Qur'an", bot.me.id);
            quote_command.add_option(dpp::command_option(dpp::co_string, "verses", "The verses to quote (e.g. 2:255 or 2:255-256)", true));
            quote_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});

            bot.global_bulk_command_create({quote_command});
        }
    });

    bot.register_command("quote", [&quran](const dpp::slashcommand_t& event) {
        std::istringstream verses(std::get<std::string>(event.get_parameter("verses")));

        unsigned short surah;
        unsigned short first_ayah;
        verses >> surah;
        verses.ignore(); // Ignore ':'
        verses >> first_ayah;

        if (surah > 114) {
            event.reply(dpp::message("Invalid `verses` parameter!").set_flags(dpp::m_ephemeral));
        } else if (first_ayah > 286) {
            event.reply(dpp::message("Invalid `verses` parameter!").set_flags(dpp::m_ephemeral));
        } else if (verses.get() == '-') {
            unsigned short last_ayah;
            verses >> last_ayah;
            if (last_ayah < first_ayah || last_ayah > 286) {
                event.reply(dpp::message("Invalid `verses` parameter!").set_flags(dpp::m_ephemeral));
                return;
            }

            std::string text;
            for (unsigned short ayah = first_ayah; ayah <= last_ayah; ++ayah) {
                text += to_superscript(ayah) + ' ' + quran[surah][first_ayah];
                if (ayah != last_ayah) text.push_back(' ');
            }

            dpp::embed embed = dpp::embed();
            embed.set_color(0x009736);
            embed.set_title("Surah " + surah_name(surah) + ' ' + verses.str());
            embed.set_description(text);
            event.reply(embed);
        } else {
            dpp::embed embed = dpp::embed();
            embed.set_color(0x009736);
            embed.set_title("Surah " + surah_name(surah) + ' ' + verses.str());
            embed.set_description(quran[surah][first_ayah]);
            event.reply(embed);
        }
    });

    bot.start(dpp::st_wait);
}