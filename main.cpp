#include "Polyweb/polyweb.hpp"
#include "Polyweb/string.hpp"
#include "json.hpp"
#include "deepseek.hpp"
#include "system_instructions.hpp"
#include <algorithm>
#include <cstdio>
#include <assert.h>
#include <dpp/dpp.h>
#include <iostream>
#include <regex>
#include <sstream>
#include <stddef.h>
#include <stdlib.h>
#include <string>
#include <utility>
#include <vector>

#define TRANSLATION_COUNT 4

#include <chrono>
#include <map>
#include <mutex>

using nlohmann::json;

// A conversation belongs to one user in one channel, so that a thread started in one
// server is not silently continued in another
typedef std::pair<uint64_t, uint64_t> ConversationKey;

struct Conversation {
    json messages = json::array();
    std::chrono::steady_clock::time_point last_used;
};

constexpr size_t max_conversation_messages = 20; // Ten exchanges
constexpr size_t max_conversation_bytes = 24'000;
constexpr auto conversation_ttl = std::chrono::hours(2);

std::map<ConversationKey, Conversation> conversation_cache;
std::mutex conversation_mutex;

ConversationKey conversation_key(const dpp::slashcommand_t& event) {
    return {(uint64_t) event.command.usr.id, (uint64_t) event.command.channel_id};
}

// Both of these must be called with conversation_mutex held

// Drops the oldest exchanges in pairs, so the history still opens with a user message
void trim_conversation(json& messages) {
    auto too_long = [&messages] {
        return messages.size() > max_conversation_messages ||
               messages.dump().size() > max_conversation_bytes;
    };

    while (messages.size() > 2 && too_long()) {
        messages.erase(messages.begin());
        messages.erase(messages.begin());
    }
}

// Without this the cache would keep an entry for every user the bot ever served
void evict_stale_conversations(std::chrono::steady_clock::time_point now) {
    for (auto it = conversation_cache.begin(); it != conversation_cache.end();) {
        if (now - it->second.last_used > conversation_ttl) {
            it = conversation_cache.erase(it);
        } else {
            ++it;
        }
    }
}

std::string getenv_string(const std::string& var) {
    char* ret = getenv(var.c_str());
    assert(ret);
    return ret;
}

// Isolates the JSON value in a model response, tolerating code fences and stray prose
std::string extract_json(const std::string& text) {
    size_t start = text.find_first_of("[{");
    size_t end = text.find_last_of("]}");
    if (start == std::string::npos || end == std::string::npos || end < start) {
        return text;
    }
    return text.substr(start, end - start + 1);
}

// Discord rejects the whole message when any of these is exceeded
constexpr size_t embed_title_limit = 256;
constexpr size_t embed_description_limit = 4096;

// Cuts on a UTF-8 character boundary, counting the ellipsis against the limit, since
// half of a multi-byte character is invalid UTF-8 and is rejected just as a long one is
std::string truncate(const std::string& text, size_t max_length) {
    if (text.size() <= max_length) return text;

    const static std::string ellipsis = "...";
    bool room_for_ellipsis = max_length > ellipsis.size();

    size_t cut = room_for_ellipsis ? max_length - ellipsis.size() : max_length;
    while (cut > 0 && ((unsigned char) text[cut] & 0xC0) == 0x80) --cut;
    return room_for_ellipsis ? text.substr(0, cut) + ellipsis : text.substr(0, cut);
}

// Lists the pages the model consulted, within Discord's 1024 character field limit.
// Embed fields render masked links, which spares the reader a percent encoded URL.
void add_sources_field(dpp::embed& embed, const std::vector<Source>& sources) {
    std::string value;
    for (const auto& source : sources) {
        std::string line;
        if (source.title.empty()) {
            line = "- " + source.url + '\n';
        } else {
            // A bracket ends the link text early and a parenthesis ends the URL early
            std::string title;
            for (char c : truncate(source.title, 80)) {
                if (c == '[' || c == ']' || c == '\\') title.push_back('\\');
                title.push_back(c);
            }

            std::string url;
            for (char c : source.url) {
                switch (c) {
                case '(': url += "%28"; break;
                case ')': url += "%29"; break;
                default: url.push_back(c);
                }
            }

            line = "- [" + title + "](" + url + ")\n";
        }

        if (value.size() + line.size() > 1024) break;
        value += line;
    }
    if (!value.empty()) embed.add_field("Sources", value);
}

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
    pw::thread_pool.resize(0);

    std::pair<unsigned short, std::string> translations[] = {
        {85, "M.A.S. Abdel Haleem"},
        {20, "Saheeh International"},
        {19, "M. Pickthall"},
        {22, "A. Yusuf Ali"},
    };
    std::string surahs[114];
    std::vector<std::string> ayahs[TRANSLATION_COUNT][114];

    std::string access_token;
    std::string client_id = getenv_string("QURAN_CLIENT_ID");
    std::cout << "Requesting Qur'an API access token..." << std::endl;
    {
        pw::Response resp;
        if (auto status = pw::fetch("POST",
                "https://" + getenv_string("QURAN_CLIENT_ID") + ':' + getenv_string("QURAN_CLIENT_SECRET") + "@oauth2.quran.foundation/oauth2/token",
                resp,
                "grant_type=client_credentials&scope=content",
                {{"Content-Type", "application/x-www-form-urlencoded"}}); !status) {
            std::cerr << "Error: Request to oauth2.quran.foundation failed: " << status.error().message() << std::endl;
            return EXIT_FAILURE;
        } else if (resp.status_code_category() != 200) {
            std::cerr << "Error: Request to oauth2.quran.foundation failed with status code " << resp.status_code << std::endl;
            return EXIT_FAILURE;
        }

        json resp_json = json::parse(resp.body_string());
        access_token = resp_json["access_token"];
    }
    std::cout << "Got Qur'an API access token: " << access_token << std::endl;

    std::cout << "Downloading Qur'an..." << std::endl;
    {
        pw::Response resp;
        if (auto status = pw::fetch("https://apis.quran.foundation/content/api/v4/chapters", resp, {{"Accept", "application/json"}, {"x-auth-token", access_token}, {"x-client-id", client_id}}); !status) {
            std::cerr << "Error: Request to apis.quran.foundation failed: " << status.error().message() << std::endl;
            return EXIT_FAILURE;
        } else if (resp.status_code_category() != 200) {
            std::cerr << "Error: Request to apis.quran.foundation failed with status code " << resp.status_code << std::endl;
            return EXIT_FAILURE;
        }

        json resp_json = json::parse(resp.body_string());
        for (const auto& chapter : resp_json["chapters"].items()) {
            unsigned short surah = clamp_surah(chapter.value()["id"]);
            surahs[surah - 1] = chapter.value()["name_complex"];
            for (unsigned short translation = 0; translation < TRANSLATION_COUNT; ++translation) {
                ayahs[translation][surah - 1].resize(std::min<unsigned short>(chapter.value()["verses_count"].get<unsigned short>(), 286));
            }
        }
    }

    for (unsigned short translation = 0; translation < TRANSLATION_COUNT; ++translation) {
        pw::Response resp;
        if (auto status = pw::fetch(
                "https://apis.quran.foundation/content/api/v4/quran/translations/" + std::to_string(translations[translation].first) + "?fields=chapter_id%2Cverse_number",
                resp,
                {
                    {"Accept", "application/json"},
                    {"x-auth-token", access_token},
                    {"x-client-id", client_id},
                }); !status) {
            std::cerr << "Error: Request to apis.quran.foundation failed: " << status.error().message() << std::endl;
            return EXIT_FAILURE;
        } else if (resp.status_code_category() != 200) {
            std::cerr << "Error: Request to apis.quran.foundation failed with status code " << resp.status_code << std::endl;
            return EXIT_FAILURE;
        }

        json resp_json = json::parse(resp.body_string());
        for (const auto& verse : resp_json["translations"].items()) {
            const static std::regex footnote_regex(R"(<sup foot_note=\d+>\d+</sup>)");

            unsigned short surah = clamp_surah(verse.value()["chapter_id"]);
            unsigned short ayah = clamp_ayah(surah, verse.value()["verse_number"]);
            std::string text = std::regex_replace(verse.value()["text"].get<std::string>(), footnote_regex, "");
            ayahs[translation][surah - 1][ayah - 1] = text;
        }
    }
    std::cout << "Downloaded Qur'an!" << std::endl;

    dpp::cluster bot(getenv_string("QURAN_DISCORD_TOKEN"));
    bot.on_log(dpp::utility::cout_logger());

    bot.on_ready([translations, &bot](const auto& event) {
        if (dpp::run_once<struct RegisterBotCommands>()) {
            dpp::command_option translation_option(dpp::co_integer, "translation", "The translation to use", false);
            for (unsigned short translation = 0; translation < TRANSLATION_COUNT; ++translation) {
                translation_option.add_choice(dpp::command_option_choice(translations[translation].second, translation));
            }

            dpp::slashcommand quote_command("quote", "Quote the Holy Qur'an", bot.me.id);
            quote_command.add_option(dpp::command_option(dpp::co_string, "verses", "The verses to quote (e.g. 2:255 or 2:255-256)", true));
            quote_command.add_option(translation_option);
            quote_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (false by default)", false));
            quote_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});
            quote_command.set_integration_types({dpp::ait_guild_install, dpp::ait_user_install});

            dpp::slashcommand search_command("search", "Search for a pattern in the Holy Qur'an", bot.me.id);
            search_command.add_option(dpp::command_option(dpp::co_string, "pattern", "The string to look for", true));
            search_command.add_option(translation_option);
            search_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (true by default)", false));
            search_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});
            search_command.set_integration_types({dpp::ait_guild_install, dpp::ait_user_install});

            dpp::slashcommand ask_command("ask", "Ask Qur'an Bot a question about Islam", bot.me.id);
            ask_command.add_option(dpp::command_option(dpp::co_string, "query", "The question being asked", true));
            ask_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (false by default)", false));
            ask_command.add_option(dpp::command_option(dpp::co_boolean, "fast", "Use a faster, cheaper AI model (false by default)", false));
            ask_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});
            ask_command.set_integration_types({dpp::ait_guild_install, dpp::ait_user_install});

            dpp::slashcommand ai_search_command("aisearch", "Search for something in the Holy Qur'an using AI", bot.me.id);
            ai_search_command.add_option(dpp::command_option(dpp::co_string, "query", "Search description", true));
            ai_search_command.add_option(translation_option);
            ai_search_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (true by default)", false));
            ai_search_command.add_option(dpp::command_option(dpp::co_boolean, "fast", "Use a faster, cheaper AI model (false by default)", false));
            ai_search_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});
            ai_search_command.set_integration_types({dpp::ait_guild_install, dpp::ait_user_install});

            dpp::slashcommand reply_command("reply", "Continue your conversation with Qur'an Bot", bot.me.id);
            reply_command.add_option(dpp::command_option(dpp::co_string, "query", "The question being asked", true));
            reply_command.add_option(dpp::command_option(dpp::co_boolean, "ephemeral", "Whether or not the response is private and temporary (false by default)", false));
            reply_command.add_option(dpp::command_option(dpp::co_boolean, "fast", "Use a faster, cheaper AI model (false by default)", false));
            reply_command.set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel});
            reply_command.set_integration_types({dpp::ait_guild_install, dpp::ait_user_install});

            bot.global_bulk_command_create({quote_command, search_command, ask_command, reply_command, ai_search_command});
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

        unsigned short last_ayah = first_ayah;
        if (verses.get() == '-') {
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

        dpp::component action_row;

        dpp::component add_prev_verse_button;
        add_prev_verse_button.set_type(dpp::cot_button);
        add_prev_verse_button.set_emoji("⏪");
        add_prev_verse_button.set_style(dpp::cos_secondary);
        add_prev_verse_button.set_id(json(
            {
                {"type", "add_prev_verse"},
                {"translation", translation},
                {"surah", surah},
                {"first_ayah", first_ayah},
                {"last_ayah", last_ayah},
            })
                .dump());
        action_row.add_component(add_prev_verse_button);

        dpp::component add_verse_button;
        add_verse_button.set_type(dpp::cot_button);
        add_verse_button.set_emoji("⏩");
        add_verse_button.set_style(dpp::cos_secondary);
        add_verse_button.set_id(json(
            {
                {"type", "add_verse"},
                {"translation", translation},
                {"surah", surah},
                {"first_ayah", first_ayah},
                {"last_ayah", last_ayah},
            })
                .dump());

        dpp::message message(embed);
        message.add_component(action_row.add_component(add_verse_button));
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
                        {"type", "continue"},
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
        unsigned short translation = data["translation"];

        if (data["type"] == "add_verse") {
            unsigned short surah = clamp_surah(data["surah"]);
            unsigned short first_ayah = clamp_ayah(surah, data["first_ayah"]);
            unsigned short last_ayah = clamp_ayah(surah, data["last_ayah"].get<unsigned short>() + 1, first_ayah);

            std::string title = "Surah " + surahs[surah - 1] + " (" + verse_key(surah, first_ayah) + '-' + std::to_string(last_ayah) + ')';
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

            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(translations[translation].second, {}, {});
            embed.set_title(title);
            embed.set_description(text);
            embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());

            dpp::message message(embed);

            dpp::component action_row;

            dpp::component add_prev_verse_button;
            add_prev_verse_button.set_type(dpp::cot_button);
            add_prev_verse_button.set_emoji("⏪");
            add_prev_verse_button.set_style(dpp::cos_secondary);
            add_prev_verse_button.set_id(json(
                {
                    {"type", "add_prev_verse"},
                    {"translation", translation},
                    {"surah", surah},
                    {"first_ayah", first_ayah},
                    {"last_ayah", last_ayah},
                })
                    .dump());
            action_row.add_component(add_prev_verse_button);

            dpp::component add_verse_button;
            add_verse_button.set_type(dpp::cot_button);
            add_verse_button.set_emoji("⏩");
            add_verse_button.set_style(dpp::cos_secondary);
            add_verse_button.set_id(json(
                {
                    {"type", "add_verse"},
                    {"translation", translation},
                    {"surah", surah},
                    {"first_ayah", first_ayah},
                    {"last_ayah", last_ayah},
                })
                    .dump());
            action_row.add_component(add_verse_button);

            message.add_component(action_row);

            event.reply(dpp::ir_update_message, message);
        } else if (data["type"] == "add_prev_verse") {
            unsigned short surah = clamp_surah(data["surah"]);
            unsigned short first_ayah = clamp_ayah(surah, data["first_ayah"].get<unsigned short>() - 1);
            unsigned short last_ayah = clamp_ayah(surah, data["last_ayah"], first_ayah);

            std::string title = "Surah " + surahs[surah - 1] + " (" + verse_key(surah, first_ayah) + '-' + std::to_string(last_ayah) + ')';
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

            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(translations[translation].second, {}, {});
            embed.set_title(title);
            embed.set_description(text);
            embed.set_footer("Qur'an Bot by BlueCannonBall", bot.me.get_avatar_url());

            dpp::message message(embed);

            dpp::component action_row;

            dpp::component add_prev_verse_button;
            add_prev_verse_button.set_type(dpp::cot_button);
            add_prev_verse_button.set_emoji("⏪");
            add_prev_verse_button.set_style(dpp::cos_secondary);
            add_prev_verse_button.set_id(json(
                {
                    {"type", "add_prev_verse"},
                    {"translation", translation},
                    {"surah", surah},
                    {"first_ayah", first_ayah},
                    {"last_ayah", last_ayah},
                })
                    .dump());
            action_row.add_component(add_prev_verse_button);

            dpp::component add_verse_button;
            add_verse_button.set_type(dpp::cot_button);
            add_verse_button.set_emoji("⏩");
            add_verse_button.set_style(dpp::cos_secondary);
            add_verse_button.set_id(json(
                {
                    {"type", "add_verse"},
                    {"translation", translation},
                    {"surah", surah},
                    {"first_ayah", first_ayah},
                    {"last_ayah", last_ayah},
                })
                    .dump());
            action_row.add_component(add_verse_button);

            message.add_component(action_row);

            event.reply(dpp::ir_update_message, message);
        } else if (data["type"] == "continue") {
            std::string pattern = data["pattern"]; // Already trimmed
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
        }
    });

    bot.register_command("ask", [&bot](const dpp::slashcommand_t& event) {
        bool ephemeral;
        if (std::holds_alternative<bool>(event.get_parameter("ephemeral"))) {
            ephemeral = std::get<bool>(event.get_parameter("ephemeral"));
        } else {
            ephemeral = false;
        }
        bool fast = false;
        if (std::holds_alternative<bool>(event.get_parameter("fast"))) {
            fast = std::get<bool>(event.get_parameter("fast"));
        }

        event.thinking(ephemeral, [&bot, event, fast](const dpp::confirmation_callback_t& callback) {
            std::string query = pw::string::trim_copy(std::get<std::string>(event.get_parameter("query")));

            json req = {
                {
                    "messages",
                    {
                        {{"role", "system"}, {"content", ask_instructions}},
                        {{"role", "user"}, {"content", query}},
                    },
                },
            };

            auto result = generate_content(req, fast, getenv_string("QURAN_DEEPSEEK_API_KEY"), true);
            if (!result) {
                event.edit_original_response(dpp::message(result.error()));
                return;
            }

            char cost_str[64] = "";
            if (result->cost > 0.0) {
                snprintf(cost_str, sizeof(cost_str), " | Cost: $%.5f", result->cost);
            }

            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(result->model_version, {}, {});
            embed.set_title(truncate(query, embed_title_limit));
            std::string answer = result->text;
            {
                // Asking again starts the conversation over
                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(conversation_mutex);
                evict_stale_conversations(now);

                auto& conversation = conversation_cache[conversation_key(event)];
                conversation.messages = {
                    {{"role", "user"}, {"content", query}},
                    {{"role", "assistant"}, {"content", answer}}
                };
                conversation.last_used = now;
            }
            embed.set_description(truncate(answer, embed_description_limit));
            add_sources_field(embed, result->sources);
            embed.set_footer(std::string("Qur'an Bot by BlueCannonBall") + cost_str, bot.me.get_avatar_url());
            event.edit_original_response(embed);
        });
    });

    bot.register_command("reply", [&bot](const dpp::slashcommand_t& event) {
        bool ephemeral;
        if (std::holds_alternative<bool>(event.get_parameter("ephemeral"))) {
            ephemeral = std::get<bool>(event.get_parameter("ephemeral"));
        } else {
            ephemeral = false;
        }
        bool fast = false;
        if (std::holds_alternative<bool>(event.get_parameter("fast"))) {
            fast = std::get<bool>(event.get_parameter("fast"));
        }

        event.thinking(ephemeral, [&bot, event, fast](const dpp::confirmation_callback_t& callback) {
            std::string query = pw::string::trim_copy(std::get<std::string>(event.get_parameter("query")));
            json contents = json::array();
            {
                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(conversation_mutex);
                evict_stale_conversations(now);

                if (auto it = conversation_cache.find(conversation_key(event)); it != conversation_cache.end()) {
                    contents = it->second.messages;
                    it->second.last_used = now; // Keep it alive while the reply is in flight
                }
            }
            contents.push_back({{"role", "user"}, {"content", query}});

            json messages = json::array({{{"role", "system"}, {"content", ask_instructions}}});
            messages.insert(messages.end(), contents.begin(), contents.end());

            json req = {
                {"messages", messages},
            };

            auto result = generate_content(req, fast, getenv_string("QURAN_DEEPSEEK_API_KEY"), true);
            if (!result) {
                event.edit_original_response(dpp::message(result.error()));
                return;
            }

            char cost_str[64] = "";
            if (result->cost > 0.0) {
                snprintf(cost_str, sizeof(cost_str), " | Cost: $%.5f", result->cost);
            }

            dpp::embed embed;
            embed.set_color(0x009736);
            embed.set_author(result->model_version, {}, {});
            embed.set_title(truncate(query, embed_title_limit));
            std::string answer = result->text;
            {
                // Appended to whatever the conversation holds now, rather than writing back
                // the snapshot read before the request, which another reply may have grown
                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(conversation_mutex);

                auto& conversation = conversation_cache[conversation_key(event)];
                conversation.messages.push_back({{"role", "user"}, {"content", query}});
                conversation.messages.push_back({{"role", "assistant"}, {"content", answer}});
                trim_conversation(conversation.messages);
                conversation.last_used = now;
            }
            embed.set_description(truncate(answer, embed_description_limit));
            add_sources_field(embed, result->sources);
            embed.set_footer(std::string("Qur'an Bot by BlueCannonBall") + cost_str, bot.me.get_avatar_url());
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
        bool fast = false;
        if (std::holds_alternative<bool>(event.get_parameter("fast"))) {
            fast = std::get<bool>(event.get_parameter("fast"));
        }

        event.thinking(ephemeral, [translations, surahs, ayahs, &bot, event, fast](const dpp::confirmation_callback_t& callback) {
            std::string query = pw::string::trim_copy(std::get<std::string>(event.get_parameter("query")));
            unsigned short translation;
            if (std::holds_alternative<long>(event.get_parameter("translation"))) {
                translation = std::get<long>(event.get_parameter("translation"));
            } else {
                translation = 0;
            }

            json req = {
                {
                    "messages",
                    {
                        {{"role", "system"}, {"content", ai_search_instructions}},
                        {{"role", "user"}, {"content", query}},
                    },
                },
                {"response_format", {{"type", "json_object"}}},
            };

            auto result = generate_content(req, fast, getenv_string("QURAN_DEEPSEEK_API_KEY"));
            if (!result) {
                event.edit_original_response(dpp::message(result.error()));
                return;
            }

            char cost_str[64] = "";
            if (result->cost > 0.0) {
                snprintf(cost_str, sizeof(cost_str), " | Cost: $%.5f", result->cost);
            }

            json results_json;
            try {
                results_json = json::parse(extract_json(result->text));
            } catch (const json::exception&) {
                event.edit_original_response(dpp::message("The AI returned a malformed response. Please try again."));
                return;
            }
            if (results_json.is_object()) {
                // Tolerate the array being wrapped in an object
                for (const auto& member : results_json) {
                    if (member.is_array()) {
                        results_json = member;
                        break;
                    }
                }
            }
            if (!results_json.is_array()) {
                event.edit_original_response(dpp::message("The AI returned a malformed response. Please try again."));
                return;
            }

            if (results_json.empty()) {
                event.edit_original_response(dpp::message("No matches found."));
            } else {
                dpp::embed embed;
                embed.set_color(0x009736);
                embed.set_author(translations[translation].second, {}, {});
                embed.set_title("Search Results (Powered by " + result->model_version + ')');
                for (const auto& result : results_json.items()) {
                    // Without a response schema, entries the model got wrong are simply skipped
                    if (!result.value().is_object() ||
                        !result.value().value("surah", json()).is_number_integer() ||
                        !result.value().value("first_ayah", json()).is_number_integer()) {
                        continue;
                    }

                    unsigned short surah = clamp_surah(result.value()["surah"]);
                    unsigned short first_ayah = clamp_ayah(surah, result.value()["first_ayah"]);

                    json::iterator last_ayah_it;
                    unsigned short last_ayah;
                    if ((last_ayah_it = result.value().find("last_ayah")) != result.value().end() &&
                        last_ayah_it->is_number_integer() &&
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
                embed.set_footer(std::string("Qur'an Bot by BlueCannonBall") + cost_str, bot.me.get_avatar_url());
                event.edit_original_response(embed);
            }
        });
    });

    bot.start(dpp::st_wait);
}
