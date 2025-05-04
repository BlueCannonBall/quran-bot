#include "Polyweb/polyweb.hpp"
#include "Polyweb/string.hpp"
#include "json.hpp"
#include "system_instructions.hpp"
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

unsigned short clamp_surah(unsigned short surah) {
    return std::min<unsigned short>(std::max<unsigned short>(surah, 1), 114);
}

unsigned short clamp_ayah(unsigned short surah, unsigned short ayah, unsigned short minimum_ayah = 1) {
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

std::vector<std::pair<std::string, std::string>> search_quran(const std::string* surahs, const std::vector<std::string>* ayahs, std::string pattern, unsigned short& surah, unsigned short& ayah, unsigned short limit = 8) {
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
        for (const auto& chapter : resp_json["chapters"].items()) {
            unsigned short surah = clamp_surah(chapter.value()["id"]);
            surahs[surah - 1] = chapter.value()["name_complex"];
            for (unsigned short translation = 0; translation < 4; ++translation) {
                ayahs[translation][surah - 1].resize(std::min<unsigned short>(chapter.value()["verses_count"].get<unsigned short>(), 286));
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

            unsigned short surah = clamp_surah(verse.value()["chapter_id"]);
            unsigned short ayah = clamp_ayah(surah, verse.value()["verse_number"]);
            std::string text = std::regex_replace(verse.value()["text"].get<std::string>(), footnote_regex, "");
            ayahs[translation][surah - 1][ayah - 1] = text;
        }
    }
    std::cout << "Downloaded Qur'an!" << std::endl;

    dpp::cluster bot(getenv("QURAN_BOT_TOKEN"));
    bot.on_log(dpp::utility::cout_logger());

    bot.on_ready([translations, &bot](const auto& event) {
        if (dpp::run_once<struct RegisterBotCommands>()) {
            dpp::command_option translation_option(dpp::co_integer, "translation", "The translation to use", false);
            for (unsigned short translation = 0; translation < 4; ++translation) {
                translation_option.add_choice(dpp::command_option_choice(translations[translation].second, translation));
            }

            dpp::slashcommand quote_command("quote", "Quote the Holy Qur'an", bot.me.id);
            quote_command.add_option(dpp::command_option(dpp::co_string, "verses", "The verses to quote (e.g. 2:255 or 2:255-256)", true));
            quote_command.add_option(translation_option);
            quote_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (false by default)", false));
            quote_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});

            dpp::slashcommand search_command("search", "Search for a pattern in the Holy Qur'an", bot.me.id);
            search_command.add_option(dpp::command_option(dpp::co_string, "pattern", "The string to look for", true));
            search_command.add_option(translation_option);
            search_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (true by default)", false));
            search_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});

            dpp::slashcommand ask_command("ask", "Ask Qur'an Bot a question about Islam", bot.me.id);
            ask_command.add_option(dpp::command_option(dpp::co_string, "query", "The question being asked", true));
            ask_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (false by default)", false));
            ask_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});

            dpp::slashcommand ai_search_command("aisearch", "Search for something in the Holy Qur'an using AI", bot.me.id);
            ai_search_command.add_option(dpp::command_option(dpp::co_string, "query", "Search description", true));
            ai_search_command.add_option(translation_option);
            ai_search_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (true by default)", false));
            ai_search_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});

            bot.global_bulk_command_create({quote_command, search_command, ask_command, ai_search_command});
        }
        std::cout << "Qur'an Bot is ready for da'wah!" << std::endl;
    });

    bot.register_command("quote", [translations, surahs, ayahs, &bot](const dpp::slashcommand_t& event) {
        std::istringstream verses(std::get<std::string>(event.get_parameter("verses")));
        unsigned short translation;
        if (std::holds_alternative<long>(event.get_parameter("translation"))) {
            translation = std::get<long>(event.get_parameter("translation"));
        } else {
            translation = 0;
        }
        bool ephemeral;
        if (std::holds_alternative<bool>(event.get_parameter("ephemeral"))) {
            ephemeral = std::get<bool>(event.get_parameter("ephemeral"));
        } else {
            ephemeral = false;
        }

        unsigned short surah;
        unsigned short first_ayah;
        verses >> surah;
        verses.ignore(); // Ignore ':'
        verses >> first_ayah;
        surah = clamp_surah(surah);
        first_ayah = clamp_ayah(surah, first_ayah);

        std::string title;
        std::string text;

        if (verses.get() == '-') {
            unsigned short last_ayah;
            verses >> last_ayah;
            last_ayah = clamp_ayah(surah, last_ayah, first_ayah);
            title = "Surah " + surahs[surah - 1] + " (" + verse_key(surah, first_ayah) + '-' + std::to_string(last_ayah) + ')';

            for (unsigned short ayah = first_ayah; ayah <= last_ayah; ++ayah) {
                if (is_poetic_surah(surah)) {
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

        dpp::message message(embed);
        if (ephemeral) message.set_flags(dpp::m_ephemeral);
        event.reply(message);
    });

    bot.register_command("search", [translations, surahs, ayahs, &bot](const dpp::slashcommand_t& event) {
        std::string pattern = pw::string::trim_copy(std::get<std::string>(event.get_parameter("pattern")));
        unsigned short translation;
        if (std::holds_alternative<long>(event.get_parameter("translation"))) {
            translation = std::get<long>(event.get_parameter("translation"));
        } else {
            translation = 0;
        }
        bool ephemeral;
        if (std::holds_alternative<bool>(event.get_parameter("ephemeral"))) {
            ephemeral = std::get<bool>(event.get_parameter("ephemeral"));
        } else {
            ephemeral = true;
        }
        unsigned short surah = 1;
        unsigned short ayah = 1;
        auto results = search_quran(surahs, ayahs[translation], pattern, surah, ayah);

        if (results.empty()) {
            dpp::message message("No matches found.");
            if (ephemeral) message.set_flags(dpp::m_ephemeral);
            event.reply(message);
        } else {
            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(translations[translation].second, {}, {});
            embed.set_title("Search Results");
            for (const auto& result : results) {
                embed.add_field(result.first, result.second);
            }
            embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());

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
                        {"ephemeral", ephemeral},
                        {"surah", surah},
                        {"ayah", ayah},
                    })
                        .dump());
                message.add_component(action_row.add_component(continue_button));
            }
            if (ephemeral) message.set_flags(dpp::m_ephemeral);
            event.reply(message);
        }
    });

    bot.on_button_click([translations, surahs, ayahs, &bot](const dpp::button_click_t& event) {
        json data = json::parse(event.custom_id);

        std::string pattern = data["pattern"]; // Already trimmed
        unsigned short translation = data["translation"];
        bool ephemeral = data["ephemeral"];
        unsigned short surah = std::max<unsigned short>(data["surah"].get<unsigned short>(), 1);
        unsigned short ayah = data["ayah"].get<unsigned short>() + 1;
        auto results = search_quran(surahs, ayahs[translation], pattern, surah, ayah);

        if (results.empty()) {
            dpp::message message("No matches found.");
            if (ephemeral) message.set_flags(dpp::m_ephemeral);
            event.reply(message);
        } else {
            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(translations[translation].second, {}, {});
            embed.set_title("Search Results");
            for (const auto& result : results) {
                embed.add_field(result.first, result.second);
            }
            embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());

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
                        {"ephemeral", ephemeral},
                        {"surah", surah},
                        {"ayah", ayah},
                    })
                        .dump());
                message.add_component(action_row.add_component(continue_button));
            }
            if (ephemeral) message.set_flags(dpp::m_ephemeral);
            event.reply(message);
        }
    });

    bot.register_command("ask", [&bot](const dpp::slashcommand_t& event) {
        bool ephemeral;
        if (std::holds_alternative<bool>(event.get_parameter("ephemeral"))) {
            ephemeral = std::get<bool>(event.get_parameter("ephemeral"));
        } else {
            ephemeral = false;
        }

        event.thinking(ephemeral, [&bot, event](const dpp::confirmation_callback_t& callback) {
            std::string query = pw::string::trim_copy(std::get<std::string>(event.get_parameter("query")));

            json req = {
                {"system_instruction", {{"parts", {{{"text", ask_instructions}}}}}},
                {"contents", {{"parts", {{{"text", query}}}}}},
                {
                    "generationConfig",
                    {
                        {"maxOutputTokens", 750},
                    },
                },
            };

            pw::URLInfo url_info("https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent");
            url_info.query_parameters->insert({"key", getenv("GOOGLE_API_KEY")});

            pw::HTTPResponse resp;
            if (pw::fetch("POST", url_info.build(), resp, req.dump(), {{"Content-Type", "application/json"}}) == PN_ERROR) {
                event.edit_original_response(dpp::message("Request to `generativelanguage.googleapis.com` failed!"));
                return;
            } else if (resp.status_code_category() != 200) {
                event.edit_original_response(dpp::message("Request to `generativelanguage.googleapis.com` failed with status code " + std::to_string(resp.status_code) + '!'));
                return;
            }

            json resp_json = json::parse(resp.body_string());
            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(resp_json["modelVersion"].get<std::string>(), {}, {});
            embed.set_title("AI Response");
            embed.set_description("__**Question:** " + query + "__\n**Answer:** " + resp_json["candidates"][0]["content"]["parts"][0]["text"].get<std::string>());
            embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());
            event.edit_original_response(embed);
        });
    });

    bot.register_command("aisearch", [translations, surahs, ayahs, &bot](const dpp::slashcommand_t& event) {
        bool ephemeral;
        if (std::holds_alternative<bool>(event.get_parameter("ephemeral"))) {
            ephemeral = std::get<bool>(event.get_parameter("ephemeral"));
        } else {
            ephemeral = true;
        }

        event.thinking(ephemeral, [translations, surahs, ayahs, &bot, event](const dpp::confirmation_callback_t& callback) {
            std::string query = pw::string::trim_copy(std::get<std::string>(event.get_parameter("query")));
            unsigned short translation;
            if (std::holds_alternative<long>(event.get_parameter("translation"))) {
                translation = std::get<long>(event.get_parameter("translation"));
            } else {
                translation = 0;
            }

            json req = {
                {"system_instruction", {{"parts", {{{"text", ai_search_instructions}}}}}},
                {"contents", {{"parts", {{{"text", query}}}}}},
                {
                    "generationConfig",
                    {
                        {"response_mime_type", "application/json"},
                        {
                            "response_schema",
                            {
                                {"type", "ARRAY"},
                                {
                                    "items",
                                    {
                                        {"type", "OBJECT"},
                                        {
                                            "properties",
                                            {
                                                {"surah", {{"type", "INTEGER"}}},
                                                {"first_ayah", {{"type", "INTEGER"}}},
                                                {"last_ayah", {{"type", "INTEGER"}, {"nullable", true}}},
                                            },
                                        },
                                        {"required", {"surah", "first_ayah"}},
                                    },
                                },
                            },
                        },
                    },
                },
            };

            pw::URLInfo url_info("https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent");
            url_info.query_parameters->insert({"key", getenv("GOOGLE_API_KEY")});

            pw::HTTPResponse resp;
            if (pw::fetch("POST", url_info.build(), resp, req.dump(), {{"Content-Type", "application/json"}}) == PN_ERROR) {
                event.edit_original_response(dpp::message("Request to `generativelanguage.googleapis.com` failed!"));
                return;
            } else if (resp.status_code_category() != 200) {
                event.edit_original_response(dpp::message("Request to `generativelanguage.googleapis.com` failed with status code " + std::to_string(resp.status_code) + '!'));
                return;
            }

            json resp_json = json::parse(resp.body_string());
            json results_json = json::parse(resp_json["candidates"][0]["content"]["parts"][0]["text"].get<std::string>());
            if (results_json.empty()) {
                event.edit_original_response(dpp::message("No matches found."));
            } else {
                dpp::embed embed;
                embed.set_color(0x009736);
                embed.set_author(translations[translation].second, {}, {});
                embed.set_title("Search Results (Powered by " + resp_json["modelVersion"].get<std::string>() + ')');
                for (const auto& result : results_json.items()) {
                    unsigned short surah = clamp_surah(result.value()["surah"]);
                    unsigned short first_ayah = clamp_ayah(surah, result.value()["first_ayah"]);

                    json::iterator last_ayah_it;
                    unsigned short last_ayah;
                    if ((last_ayah_it = result.value().find("last_ayah")) != result.value().end() &&
                        !last_ayah_it->is_null() &&
                        (last_ayah = clamp_ayah(surah, *last_ayah_it)) != first_ayah) {
                        std::string text;
                        for (unsigned short ayah = first_ayah; ayah <= last_ayah; ++ayah) {
                            if (is_poetic_surah(surah)) {
                                text += std::to_string(ayah) + ". " + ayahs[translation][surah - 1][ayah - 1];
                                if (ayah != last_ayah) text.push_back('\n');
                            } else {
                                text += to_superscript(ayah) + ' ' + ayahs[translation][surah - 1][ayah - 1];
                                if (ayah != last_ayah) text.push_back(' ');
                            }
                        }
                        embed.add_field("Surah " + surahs[surah - 1] + " (" + verse_key(surah, first_ayah) + '-' + std::to_string(last_ayah) + ')', text);
                    } else {
                        embed.add_field("Surah " + surahs[surah - 1] + " (" + verse_key(surah, first_ayah) + ')', ayahs[translation][surah - 1][first_ayah - 1]);
                    }
                }
                embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());
                event.edit_original_response(embed);
            }
        });
    });

    bot.start(dpp::st_wait);
}
