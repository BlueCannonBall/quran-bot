#pragma once
#include <string>
#include <expected>
#include "json.hpp"

struct GeminiResponse {
    std::string text;
    std::string model_version;
    double cost;
};

std::expected<GeminiResponse, std::string> generate_content(const nlohmann::json& req, bool fast, const std::string& api_key);
