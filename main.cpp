#include "Polyweb/polyweb.hpp"
#include "Polyweb/string.hpp"
#include "json.hpp"
#include <algorithm>
#include <dpp/dpp.h>
#include <iostream>
#include <regex>
#include <sstream>
#include <stddef.h>
#include <stdlib.h>
#include <string>
#include <utility>

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

std::string verse_key(unsigned short surah, unsigned short ayah) {
    return std::to_string(surah) + ':' + std::to_string(ayah);
}

std::vector<std::pair<std::string, std::string>> search_quran(const std::string surahs[114], const std::vector<std::string> ayahs[114], std::string pattern, unsigned short& surah, unsigned short& ayah, unsigned short limit = 8) {
    pw::string::to_lower(pattern);

    std::vector<std::pair<std::string, std::string>> ret;
    for (; surah <= 114; ++surah) {
        for (; ayah <= ayahs[surah - 1].size(); ++ayah) {
            size_t match_pos;
            if ((match_pos = pw::string::to_lower_copy(ayahs[surah - 1][ayah - 1]).find(pattern)) != std::string::npos) {
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

int main() {
    pn::init();
    pw::threadpool.resize(0);

    std::pair<unsigned short, std::string> translations[] = {
        {131, "Dr. Mustafa Khattab, The Clear Quran"},
        {20, "Saheeh International"},
        {19, "M. Pickthall"},
        {22, "A. Yusuf Ali"},
    };
    std::string surahs[114];
    std::vector<std::string> ayahs[4][114];

    std::cout << "Downloading Qur'an..." << std::endl;
    {
        pw::HTTPResponse resp;
        if (pw::fetch("https://api.quran.com/api/v4/chapters", resp, {{"Accept", "application/json"}}) == PN_ERROR) {
            std::cerr << "Error: Request to api.quran.com failed: " << pw::universal_strerror() << std::endl;
            return EXIT_FAILURE;
        } else if (resp.status_code_category() != 200) {
            std::cerr << "Error: Request to api.quran.com failed with status code " << resp.status_code << std::endl;
            return EXIT_FAILURE;
        }

        json resp_json = json::parse(resp.body_string());
        for (const auto& surah : resp_json["chapters"].items()) {
            surahs[surah.value()["id"].get<unsigned short>() - 1] = surah.value()["name_complex"].get<std::string>();
            for (unsigned short translation = 0; translation < 4; ++translation) {
                ayahs[translation][surah.value()["id"].get<unsigned short>() - 1].resize(surah.value()["verses_count"].get<unsigned short>());
            }
        }
    }

    for (unsigned short translation = 0; translation < 4; ++translation) {
        pw::HTTPResponse resp;
        if (pw::fetch("https://api.quran.com/api/v4/quran/translations/" + std::to_string(translations[translation].first) + "?fields=chapter_id%2Cverse_number", resp, {{"Accept", "application/json"}}) == PN_ERROR) {
            std::cerr << "Error: Request to api.quran.com failed: " << pw::universal_strerror() << std::endl;
            return EXIT_FAILURE;
        } else if (resp.status_code_category() != 200) {
            std::cerr << "Error: Request to api.quran.com failed with status code " << resp.status_code << std::endl;
            return EXIT_FAILURE;
        }

        json resp_json = json::parse(resp.body_string());
        for (const auto& verse : resp_json["translations"].items()) {
            static std::regex footnote_regex(R"(<sup foot_note=\d+>\d+</sup>)");
            std::string text = std::regex_replace(verse.value()["text"].get<std::string>(), footnote_regex, "");
            ayahs[translation][verse.value()["chapter_id"].get<unsigned short>() - 1][verse.value()["verse_number"].get<unsigned short>() - 1] = text;
        }
    }
    std::cout << "Downloaded Qur'an!" << std::endl;

    dpp::cluster bot(getenv("QURAN_BOT_TOKEN"));
    bot.on_log(dpp::utility::cout_logger());

    bot.on_ready([translations, &bot](const auto& event) {
        if (dpp::run_once<struct RegisterBotCommands>()) {
            dpp::command_option translation_option(dpp::co_integer, "translation", "The translation to use.", false);
            for (unsigned short translation = 0; translation < 4; ++translation) {
                translation_option.add_choice(dpp::command_option_choice(translations[translation].second, translation));
            }

            dpp::slashcommand quote_command("quote", "Quote the Holy Qur'an", bot.me.id);
            quote_command.add_option(dpp::command_option(dpp::co_string, "verses", "The verses to quote (e.g. 2:255 or 2:255-256)", true));
            quote_command.add_option(translation_option);
            quote_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});

            dpp::slashcommand search_command("search", "Search for a pattern in the Holy Qur'an", bot.me.id);
            search_command.add_option(dpp::command_option(dpp::co_string, "pattern", "The string to look for.", true));
            search_command.add_option(translation_option);
            search_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});

            bot.global_bulk_command_create({quote_command, search_command});
        }
        std::cout << "Qur'an Bot is ready for dawah!" << std::endl;
    });

    bot.register_command("quote", [translations, surahs, ayahs, &bot](const dpp::slashcommand_t& event) {
        std::istringstream verses(std::get<std::string>(event.get_parameter("verses")));
        unsigned short translation;
        try {
            translation = std::get<long>(event.get_parameter("translation"));
        } catch (...) {
            translation = 0;
        }

        unsigned short surah;
        unsigned short first_ayah;
        verses >> surah;
        verses.ignore(); // Ignore ':'
        verses >> first_ayah;
        surah = std::min<unsigned short>(std::max<unsigned short>(surah, 1), 114);
        first_ayah = std::min<unsigned short>(std::max<unsigned short>(first_ayah, 1), ayahs[translation][surah - 1].size());

        std::string title;
        std::string text;

        if (verses.get() == '-') {
            unsigned short last_ayah;
            verses >> last_ayah;
            last_ayah = std::min<unsigned short>(std::max<unsigned short>(last_ayah, 1), ayahs[translation][surah - 1].size());
            title = "Surah " + surahs[surah - 1] + " (" + verse_key(surah, first_ayah) + '-' + std::to_string(last_ayah) + ')';

            for (unsigned short ayah = first_ayah; ayah <= last_ayah; ++ayah) {
                if (surah == 1 || surah >= 80) {
                    text += std::to_string(ayah) + ". " + ayahs[translation][surah - 1][ayah - 1];
                    if (ayah != last_ayah) text.push_back('\n');
                } else {
                    text += to_superscript(ayah) + ' ' + ayahs[translation][surah - 1][ayah - 1];
                    if (ayah != last_ayah) text.push_back(' ');
                }
            }
        } else {
            title = "Surah " + surahs[surah - 1] + " (" + verse_key(surah, first_ayah) + ')';
            text = ayahs[translation][surah - 1][first_ayah - 1];
        }

        dpp::embed embed;
        embed.set_color(0x009736);
        embed.set_author(translations[translation].second, {}, {});
        embed.set_title(title);
        embed.set_description(text);
        embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());
        event.reply(embed);
    });

    bot.register_command("search", [translations, surahs, ayahs, &bot](const dpp::slashcommand_t& event) {
        std::string pattern = pw::string::trim_copy(std::get<std::string>(event.get_parameter("pattern")));
        unsigned short translation;
        try {
            translation = std::get<long>(event.get_parameter("translation"));
        } catch (...) {
            translation = 0;
        }
        unsigned short surah = 1;
        unsigned short ayah = 1;
        auto results = search_quran(surahs, ayahs[translation], pattern, surah, ayah);

        if (results.empty()) {
            event.reply(dpp::message("No matches found.").set_flags(dpp::m_ephemeral));
        } else {
            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(translations[translation].second, {}, {});
            embed.set_title("Search Results");
            embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());
            for (const auto& result : results) {
                embed.add_field(result.first, result.second);
            }

            dpp::message message(embed);
            if (surah < 114 || (surah == 114 && ayah < 6)) {
                dpp::component action_row;
                dpp::component continue_button;
                continue_button.set_type(dpp::cot_button);
                continue_button.set_label("Keep looking");
                continue_button.set_emoji("🔍");
                continue_button.set_id(json(
                    {
                        {"pattern", pattern},
                        {"translation", translation},
                        {"surah", surah},
                        {"ayah", ayah},
                    })
                        .dump());
                message.add_component(action_row.add_component(continue_button));
            }
            message.set_flags(dpp::m_ephemeral);

            event.reply(message);
        }
    });

    bot.on_button_click([translations, surahs, ayahs, &bot](const dpp::button_click_t& event) {
        json data = json::parse(event.custom_id);

        std::string pattern = data["pattern"]; // Already trimmed
        unsigned short translation = data["translation"];
        unsigned short surah = std::max<unsigned short>(data["surah"].get<unsigned short>(), 1);
        unsigned short ayah = data["ayah"].get<unsigned short>() + 1;
        auto results = search_quran(surahs, ayahs[translation], pattern, surah, ayah);

        if (results.empty()) {
            event.reply(dpp::message("No matches found.").set_flags(dpp::m_ephemeral));
        } else {
            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(translations[translation].second, {}, {});
            embed.set_title("Search Results");
            embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());
            for (const auto& result : results) {
                embed.add_field(result.first, result.second);
            }

            dpp::message message(embed);
            if (surah < 114 || (surah == 114 && ayah < 6)) {
                dpp::component action_row;
                dpp::component continue_button;
                continue_button.set_type(dpp::cot_button);
                continue_button.set_label("Keep looking");
                continue_button.set_emoji("🔍");
                continue_button.set_id(json(
                    {
                        {"pattern", pattern},
                        {"translation", translation},
                        {"surah", surah},
                        {"ayah", ayah},
                    })
                        .dump());
                message.add_component(action_row.add_component(continue_button));
            }
            message.set_flags(dpp::m_ephemeral);

            event.reply(message);
        }
    });

    bot.start(dpp::st_wait);
}