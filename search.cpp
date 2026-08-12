#include "search.hpp"
#include "Polyweb/polyweb.hpp"
#include "json.hpp"
#include <chrono>
#include <regex>
#include <stdlib.h>

namespace {
    const char user_agent[] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

    std::string strip_tags(const std::string& html) {
        const static std::regex tag_regex("<[^>]*>");
        return std::regex_replace(html, tag_regex, "");
    }

    std::string decode_entities(const std::string& text) {
        const static std::pair<const char*, const char*> named[] = {
            {"&amp;", "&"},
            {"&lt;", "<"},
            {"&gt;", ">"},
            {"&quot;", "\""},
            {"&apos;", "'"},
            {"&nbsp;", " "},
            {"&hellip;", "..."},
            {"&mdash;", "—"},
            {"&ndash;", "–"},
            {"&lsquo;", "‘"},
            {"&rsquo;", "’"},
            {"&ldquo;", "“"},
            {"&rdquo;", "”"},
            {"&rsaquo;", "›"},
            {"&lsaquo;", "‹"},
        };

        std::string ret;
        ret.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            if (text[i] != '&') {
                ret.push_back(text[i++]);
                continue;
            }

            size_t semicolon = text.find(';', i);
            if (semicolon == std::string::npos || semicolon - i > 10) {
                ret.push_back(text[i++]);
                continue;
            }

            std::string entity = text.substr(i, semicolon - i + 1);
            if (entity.size() > 3 && entity[1] == '#') {
                // Numeric entities are only decoded within the Basic Latin block
                int code;
                try {
                    code = entity[2] == 'x' || entity[2] == 'X'
                             ? std::stoi(entity.substr(3, entity.size() - 4), nullptr, 16)
                             : std::stoi(entity.substr(2, entity.size() - 3));
                } catch (const std::exception&) {
                    ret.push_back(text[i++]);
                    continue;
                }
                if (code >= 0x20 && code < 0x7F) {
                    ret.push_back((char) code);
                    i = semicolon + 1;
                    continue;
                }
            }

            bool matched = false;
            for (const auto& [name, replacement] : named) {
                if (entity == name) {
                    ret += replacement;
                    i = semicolon + 1;
                    matched = true;
                    break;
                }
            }
            if (!matched) ret.push_back(text[i++]);
        }
        return ret;
    }

    std::string clean(const std::string& html, size_t max_length = 400) {
        std::string ret = pw::string::trim_copy(decode_entities(strip_tags(html)));
        // Collapse the whitespace the markup left behind
        const static std::regex whitespace_regex("\\s+");
        ret = std::regex_replace(ret, whitespace_regex, " ");
        if (ret.size() > max_length) {
            // Back off the cut so it never lands inside a UTF-8 sequence
            size_t cut = max_length;
            while (cut > 0 && ((unsigned char) ret[cut] & 0xC0) == 0x80) --cut;
            ret = ret.substr(0, cut) + "...";
        }
        return ret;
    }

    pw::ClientConfig search_config() {
        pw::ClientConfig config;
        config.tcp.recv_timeout = std::chrono::seconds(20);
        return config;
    }

    std::vector<SearchResult> brave_search(const std::string& query, const std::string& api_key, size_t max_results) {
        pw::URLInfo url_info("https://api.search.brave.com/res/v1/web/search");
        url_info.query_parameters->insert({"q", query});
        url_info.query_parameters->insert({"count", std::to_string(max_results)});

        pw::Response resp;
        if (auto status = pw::fetch("GET",
                url_info.build(),
                resp,
                pw::Headers {
                    {"Accept", "application/json"},
                    {"X-Subscription-Token", api_key},
                },
                search_config());
            !status || resp.status_code_category() != 200) {
            return {};
        }

        std::vector<SearchResult> results;
        try {
            nlohmann::json resp_json = nlohmann::json::parse(resp.body_string());
            if (!resp_json.contains("web") || !resp_json["web"].contains("results")) return {};
            for (const auto& result : resp_json["web"]["results"]) {
                if (results.size() >= max_results) break;
                results.push_back({
                    clean(result.value("title", ""), 150),
                    result.value("url", ""),
                    clean(result.value("description", "")),
                });
            }
        } catch (const nlohmann::json::exception&) {
            return {};
        }
        return results;
    }

    std::vector<SearchResult> mojeek_search(const std::string& query, size_t max_results) {
        // Mojeek only treats '+' as a space and ignores "%20", which is what
        // QueryParameters builds by default
        pw::URLInfo url_info("https://www.mojeek.com/search");
        url_info.query_parameters->insert({"q", query});

        pw::Response resp;
        if (auto status = pw::fetch("GET",
                url_info.build(),
                resp,
                pw::Headers {
                    {"User-Agent", user_agent},
                    {"Accept", "text/html"},
                },
                search_config());
            !status || resp.status_code_category() != 200) {
            return {};
        }

        // Each result is <h2><a class="title" ... href="URL">TITLE</a></h2><p class="s">SNIPPET</p>
        const static std::regex result_regex(R"rx(<a class="title"[^>]*href="([^"]*)"[^>]*>([\s\S]*?)</a></h2>\s*<p class="s">([\s\S]*?)</p>)rx");

        std::vector<SearchResult> results;
        std::string body = resp.body_string();
        for (std::sregex_iterator it(body.begin(), body.end(), result_regex), end; it != end; ++it) {
            if (results.size() >= max_results) break;
            results.push_back({
                clean((*it)[2].str(), 150),
                decode_entities((*it)[1].str()),
                clean((*it)[3].str()),
            });
        }
        return results;
    }
    bool contains_arabic(const std::string& str) {
        // U+0600-U+06FF is encoded with a 0xD8-0xDB lead byte in UTF-8
        return std::any_of(str.begin(), str.end(), [](unsigned char c) {
            return c >= 0xD8 && c <= 0xDB;
        });
    }

    // Mojeek indexes English only, so Wikipedia backs up whatever it can't answer
    std::vector<SearchResult> wikipedia_search(const std::string& query, size_t max_results) {
        std::string hostname = contains_arabic(query) ? "ar.wikipedia.org" : "en.wikipedia.org";

        pw::URLInfo url_info("https://" + hostname + "/w/api.php");
        url_info.query_parameters->insert({"action", "query"});
        url_info.query_parameters->insert({"list", "search"});
        url_info.query_parameters->insert({"srsearch", query});
        url_info.query_parameters->insert({"srlimit", std::to_string(max_results)});
        url_info.query_parameters->insert({"format", "json"});

        pw::Response resp;
        if (auto status = pw::fetch("GET",
                url_info.build(),
                resp,
                pw::Headers {
                    {"Accept", "application/json"},
                    // Wikimedia's policy asks clients to identify themselves
                    {"User-Agent", "QuranBot/1.0 (https://github.com/BlueCannonBall/quran-bot)"},
                },
                search_config());
            !status || resp.status_code_category() != 200) {
            return {};
        }

        std::vector<SearchResult> results;
        try {
            nlohmann::json resp_json = nlohmann::json::parse(resp.body_string());
            if (!resp_json.contains("query") || !resp_json["query"].contains("search")) return {};
            for (const auto& result : resp_json["query"]["search"]) {
                if (results.size() >= max_results) break;
                std::string title = result.value("title", "");
                if (title.empty()) continue;

                std::string slug = title;
                std::replace(slug.begin(), slug.end(), ' ', '_');
                results.push_back({
                    clean(title, 150),
                    "https://" + hostname + "/wiki/" + pw::percent_encode(slug, false, false),
                    clean(result.value("snippet", "")),
                });
            }
        } catch (const nlohmann::json::exception&) {
            return {};
        }
        return results;
    }
} // namespace

std::vector<SearchResult> web_search(const std::string& query, size_t max_results) {
    if (query.empty()) return {};

    std::vector<SearchResult> results;
    if (const char* brave_key = getenv("QURAN_BRAVE_API_KEY"); brave_key && *brave_key) {
        results = brave_search(query, brave_key, max_results);
    } else {
        results = mojeek_search(query, max_results);
    }

    if (results.empty()) results = wikipedia_search(query, max_results);
    return results;
}

std::string format_search_results(const std::vector<SearchResult>& results) {
    if (results.empty()) {
        return "No results found. Answer from your own knowledge, and say so if you are unsure.";
    }

    std::string ret;
    for (size_t i = 0; i < results.size(); ++i) {
        ret += '[' + std::to_string(i + 1) + "] " + results[i].title + '\n' +
               results[i].url + '\n' +
               results[i].snippet + "\n\n";
    }
    return ret;
}
