#include "deepseek.hpp"
#include "search.hpp"
#include "Polyweb/polyweb.hpp"
#include <algorithm>
#include <chrono>

namespace {
    // The model is allowed this many rounds of searching before it must answer
    constexpr int max_tool_rounds = 3;

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

    struct Prices {
        double cache_hit;
        double cache_miss;
        double output;
    };

    // Per-token prices in USD (https://api-docs.deepseek.com/quick_start/pricing)
    constexpr Prices flash_prices = {0.0028e-6, 0.14e-6, 0.28e-6};
    constexpr Prices pro_prices = {0.003625e-6, 0.435e-6, 0.87e-6};

    // The model sometimes writes a tool call out as literal markup instead of returning
    // it in tool_calls, and that markup must never reach the user as an answer
    bool is_leaked_tool_call(const std::string& text) {
        return (text.find("DSML") != std::string::npos && text.find("tool_call") != std::string::npos) ||
               text.find("invoke name=\"web_search\"") != std::string::npos;
    }

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

    bool answer_forced = false;
    for (int round = 0;; ++round) {
        // Searching is offered until the last round, which must produce an answer. The tool
        // stays declared either way, since taking it back mid conversation leaves the model
        // imitating its own tool call syntax in prose.
        bool tools_offered = enable_search && !answer_forced && round < max_tool_rounds;
        if (enable_search) {
            req["tools"] = nlohmann::json::array({search_tool});
            req["tool_choice"] = tools_offered ? "auto" : "none";
        }

        pw::Response resp;
        if (auto status = pw::fetch("POST",
                "https://api.deepseek.com/chat/completions",
                resp,
                req.dump(),
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
                assistant_message["content"] = message["content"];
            }
            req["messages"].push_back(std::move(assistant_message));
            for (const auto& tool_call : message["tool_calls"]) {
                std::string query;
                if (tool_call.contains("function") && tool_call["function"].contains("arguments")) {
                    try {
                        query = nlohmann::json::parse(tool_call["function"]["arguments"].get<std::string>()).value("query", "");
                    } catch (const nlohmann::json::exception&) {
                        // Leave the query empty, which web_search reports as no results
                    }
                }

                auto search_results = web_search(query);
                for (const auto& search_result : search_results.results) {
                    if (std::find(result.sources.begin(), result.sources.end(), search_result.url) == result.sources.end()) {
                        result.sources.push_back(search_result.url);
                    }
                }

                req["messages"].push_back({
                    {"role", "tool"},
                    {"tool_call_id", tool_call.value("id", "")},
                    {"content", format_search_results(search_results)},
                });
            }
            continue;
        }

        std::string text;
        if (message.contains("content") && message["content"].is_string()) {
            text = message["content"].get<std::string>();
        }

        if (text.empty() || is_leaked_tool_call(text)) {
            // One more round, with the tool forbidden rather than absent, to get prose back
            if (!answer_forced) {
                answer_forced = true;
                continue;
            }
            return std::unexpected("DeepSeek returned an empty response!");
        }

        result.text = std::move(text);
        return result;
    }
}
