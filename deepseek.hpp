#pragma once
#include <string>
#include <vector>
#include <expected>
#include "json.hpp"

struct DeepSeekResponse {
    std::string text;
    std::string model_version;
    double cost;
    std::vector<std::string> sources; // URLs the model consulted via web search
};

// Runs a chat completion to conclusion, servicing any web_search tool calls
// along the way when enable_search is set.
std::expected<DeepSeekResponse, std::string> generate_content(nlohmann::json req, bool fast, const std::string& api_key, bool enable_search = false);
