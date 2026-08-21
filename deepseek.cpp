#include "deepseek.hpp"
#include "quran.hpp"
#include "search.hpp"
#include "Polyweb/polyweb.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>

namespace {
    // The model is allowed this many rounds of tool use before it must answer. A round may
    // carry any number of calls, so this bounds how often it can look at results and then
    // fetch something else: finding a verse, quoting it, and checking a hadith is already
    // three, and running out leaves it answering from memory.
    constexpr int max_tool_rounds = 5;

    const nlohmann::json search_tool = {
        {"type", "function"},
        {
            "function",
            {
                {"name", "web_search"},
                {"description", "Search the web for current information, or to verify a claim, find a source, or look up a fatwa, hadith, or news event. Prefer it whenever a question depends on recent or checkable facts."},
                {
                    "parameters",
                    {
                        {"type", "object"},
                        {
                            "properties",
                            {
                                {"query", {{"type", "string"}, {"description", "The search query"}}},
                            },
                        },
                        {"required", {"query"}},
                    },
                },
            },
        },
    };

    // Reading from the loaded corpus costs nothing, so the model is told to reach for
    // these rather than quote the Qur'an from memory
    const nlohmann::json quote_tool = {
        {"type", "function"},
        {
            "function",
            {
                {"name", "quote_quran"},
                {"description", "Retrieve the exact text of a verse or range of verses of the Qur'an. Use this for every Qur'anic quotation instead of quoting from memory, and to confirm that a verse says what you believe it says."},
                {
                    "parameters",
                    {
                        {"type", "object"},
                        {
                            "properties",
                            {
                                {"surah", {{"type", "integer"}, {"description", "Number of the surah, 1 to 114"}}},
                                {"first_ayah", {{"type", "integer"}, {"description", "First ayah of the range"}}},
                                {"last_ayah", {{"type", "integer"}, {"description", "Last ayah of the range, omitted for a single verse"}}},
                            },
                        },
                        {"required", {"surah", "first_ayah"}},
                    },
                },
            },
        },
    };

    const nlohmann::json quran_search_tool = {
        {"type", "function"},
        {
            "function",
            {
                {"name", "search_quran"},
                {"description", "Find verses of the Qur'an whose translation contains a given wording. Use it to locate a verse whose reference you do not remember, before quoting it with quote_quran."},
                {
                    "parameters",
                    {
                        {"type", "object"},
                        {
                            "properties",
                            {
                                {"pattern", {{"type", "string"}, {"description", "Words to look for in the English translation"}}},
                            },
                        },
                        {"required", {"pattern"}},
                    },
                },
            },
        },
    };

    // The model occasionally writes a tool call out as literal markup in its content rather
    // than returning it in tool_calls. The cause is not understood, so this only keeps the
    // markup away from the reader and puts the offending text in the log to be diagnosed.
    // Returns whether anything was stripped.
    bool append_content(std::string& answer, const std::string& content) {
        // Only the bracketed token is markup. The bare word in prose is not, so an answer
        // that happens to discuss the format is left alone.
        size_t cut = std::string::npos;
        for (size_t marker = content.find("DSML"); marker != std::string::npos; marker = content.find("DSML", marker + 4)) {
            // The observed token is <||DSML||, whose bars are three bytes apiece, so the
            // bracket sits seven bytes back; the window allows for a closing slash too
            size_t start = content.rfind('<', marker);
            if (start != std::string::npos && marker - start <= 12) {
                cut = start;
                break;
            }
        }

        if (cut == std::string::npos) {
            answer += content;
            return false;
        }

        std::cerr << "[deepseek] tool call markup leaked into content, stripping it:\n"
                  << content << std::endl;

        answer.append(content, 0, cut);
        while (!answer.empty() && std::isspace((unsigned char) answer.back())) {
            answer.pop_back();
        }
        return true;
    }

    // The model writes these arguments, so a wrong type is its mistake to absorb, not
    // an exception to throw on a worker thread
    int int_argument(const nlohmann::json& arguments, const char* key) {
        if (auto it = arguments.find(key); it != arguments.end()) {
            if (it->is_number_integer()) return it->get<int>();
            if (it->is_string()) {
                try {
                    return std::stoi(it->get<std::string>());
                } catch (const std::exception&) {
                }
            }
        }
        return 0;
    }

    std::string string_argument(const nlohmann::json& arguments, const char* key) {
        if (auto it = arguments.find(key); it != arguments.end() && it->is_string()) {
            return it->get<std::string>();
        }
        return {};
    }

    struct Prices {
        double cache_hit;
        double cache_miss;
        double output;
    };

    // Per-token prices in USD (https://api-docs.deepseek.com/quick_start/pricing)
    constexpr Prices flash_prices = {0.0028e-6, 0.14e-6, 0.28e-6};
    constexpr Prices pro_prices = {0.003625e-6, 0.435e-6, 0.87e-6};

    double usage_cost(const nlohmann::json& resp_json, const Prices& prices) {
        if (!resp_json.contains("usage")) return 0.0;
        const auto& usage = resp_json["usage"];
        int prompt_tokens = usage.value("prompt_tokens", 0);
        int cache_hit_tokens = usage.value("prompt_cache_hit_tokens", 0);
        int cache_miss_tokens = usage.value("prompt_cache_miss_tokens", prompt_tokens - cache_hit_tokens);
        return cache_hit_tokens * prices.cache_hit +
               cache_miss_tokens * prices.cache_miss +
               usage.value("completion_tokens", 0) * prices.output;
    }
} // namespace

std::expected<DeepSeekResponse, std::string> generate_content(nlohmann::json req, bool fast, const std::string& api_key, bool enable_search) {
    const Prices& prices = fast ? flash_prices : pro_prices;

    req["model"] = fast ? "deepseek-v4-flash" : "deepseek-v4-pro";
    req["stream"] = false;

    pw::ClientConfig config;
    config.tcp.recv_timeout = std::chrono::seconds(120);

    DeepSeekResponse result;
    result.cost = 0.0;

    // Built up across rounds, since an answer may be written in pieces either side of a
    // tool call rather than all at once in the final message
    std::string answer;
    bool answer_forced = false;
    for (int round = 0;; ++round) {
        // Searching is offered until the last round, which must produce an answer. The tool
        // stays declared either way, since taking it back mid conversation leaves the model
        // imitating its own tool call syntax in prose.
        bool tools_offered = enable_search && !answer_forced && round < max_tool_rounds;
        if (enable_search) {
            req["tools"] = nlohmann::json::array({quote_tool, quran_search_tool, search_tool});
            req["tool_choice"] = tools_offered ? "auto" : "none";
        }

        // Forbidding the tool does not stop the model wanting it: told only "none", it writes
        // the call it wanted out as prose instead. Saying so plainly is what stops it, and the
        // instruction is left out of the stored conversation so it colours only this request.
        nlohmann::json body = req;
        if (enable_search && !tools_offered) {
            body["messages"].push_back({
                {"role", "user"},
                {"content", "No further tool calls are available to you. Answer now, in prose, using only what you have already gathered."},
            });
        }

        pw::Response resp;
        if (auto status = pw::fetch("POST",
                "https://api.deepseek.com/chat/completions",
                resp,
                body.dump(),
                pw::Headers {
                    {"Content-Type", "application/json"},
                    {"Authorization", "Bearer " + api_key},
                },
                config);
            !status) {
            return std::unexpected("Request to `api.deepseek.com` failed!");
        } else if (resp.status_code_category() != 200) {
            return std::unexpected("Request to `api.deepseek.com` failed with status code " + std::to_string(resp.status_code) + ":\n```\n" + resp.body_string() + "\n```");
        }

        nlohmann::json resp_json;
        try {
            resp_json = nlohmann::json::parse(resp.body_string());
        } catch (const nlohmann::json::exception&) {
            return std::unexpected("DeepSeek returned a malformed response!");
        }
        if (!resp_json.contains("choices") || resp_json["choices"].empty()) {
            return std::unexpected("DeepSeek returned no choices:\n```\n" + resp.body_string() + "\n```");
        }

        result.cost += usage_cost(resp_json, prices);
        result.model_version = resp_json.value("model", req["model"].get<std::string>());

        const auto& message = resp_json["choices"][0]["message"];
        // Servicing tool calls the last round didn't offer would loop forever
        if (tools_offered && message.contains("tool_calls") && message["tool_calls"].is_array() && !message["tool_calls"].empty()) {
            // Only the fields DeepSeek takes back: reasoning_content is an input to the
            // beta prefix completion feature, not something an ordinary request accepts
            nlohmann::json assistant_message = {
                {"role", "assistant"},
                {"tool_calls", message["tool_calls"]},
            };
            if (message.contains("content") && message["content"].is_string()) {
                // The model often begins answering before it calls a tool, and carries on
                // where it left off afterwards, so this half of the answer must be kept
                append_content(answer, message["content"].get<std::string>());
                assistant_message["content"] = message["content"];
            }
            req["messages"].push_back(std::move(assistant_message));
            for (const auto& tool_call : message["tool_calls"]) {
                std::string name;
                nlohmann::json arguments = nlohmann::json::object();
                if (tool_call.contains("function")) {
                    name = tool_call["function"].value("name", "");
                    try {
                        arguments = nlohmann::json::parse(tool_call["function"].value("arguments", "{}"));
                        if (!arguments.is_object()) arguments = nlohmann::json::object();
                    } catch (const nlohmann::json::exception&) {
                        // Leave the arguments empty, which each tool reports in its own way
                    }
                }

                std::string content;
                if (name == "quote_quran") {
                    content = quote_quran(int_argument(arguments, "surah"),
                        int_argument(arguments, "first_ayah"),
                        int_argument(arguments, "last_ayah"));
                } else if (name == "search_quran") {
                    content = search_quran_text(string_argument(arguments, "pattern"));
                } else {
                    auto search_results = web_search(string_argument(arguments, "query"));
                    for (const auto& search_result : search_results.results) {
                        if (std::none_of(result.sources.begin(), result.sources.end(), [&search_result](const Source& source) {
                                return source.url == search_result.url;
                            })) {
                            result.sources.push_back({search_result.title, search_result.url});
                        }
                    }
                    content = format_search_results(search_results);
                }

                req["messages"].push_back({
                    {"role", "tool"},
                    {"tool_call_id", tool_call.value("id", "")},
                    {"content", content},
                });
            }
            continue;
        }

        bool leaked = false;
        if (message.contains("content") && message["content"].is_string()) {
            leaked = append_content(answer, message["content"].get<std::string>());
        }

        // One more round, with the tool forbidden rather than absent, to finish in prose
        if ((leaked || answer.empty()) && !answer_forced) {
            answer_forced = true;
            continue;
        }
        if (answer.empty()) {
            return std::unexpected("DeepSeek returned an empty response!");
        }

        // Logged so that an answer which arrives in Discord looking wrong can be compared
        // against what the model actually returned, which is otherwise unrecoverable
        std::cerr << "[deepseek] answer after " << (round + 1) << " round(s), "
                  << answer.size() << " bytes:\n"
                  << answer << "\n[deepseek] end of answer" << std::endl;

        result.text = std::move(answer);
        return result;
    }
}
