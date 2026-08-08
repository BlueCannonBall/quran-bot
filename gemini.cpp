#include "gemini.hpp"
#include "Polyweb/polyweb.hpp"
#include <chrono>

std::expected<GeminiResponse, std::string> generate_content(const nlohmann::json& req, bool fast, const std::string& api_key) {
    std::string model_name = fast ? "gemini-3.6-flash" : "gemini-3.1-pro-preview";
    pw::URLInfo url_info("https://generativelanguage.googleapis.com/v1beta/models/" + model_name + ":generateContent");
    url_info.query_parameters->insert({"key", api_key});

    pw::ClientConfig config;
    config.tcp.recv_timeout = std::chrono::seconds(120);

    pw::Response resp;
    if (auto status = pw::fetch("POST", url_info.build(), resp, req.dump(), {{"Content-Type", "application/json"}}, config); !status) {
        return std::unexpected("Request to `generativelanguage.googleapis.com` failed!");
    } else if (resp.status_code_category() != 200) {
        return std::unexpected("Request to `generativelanguage.googleapis.com` failed with status code " + std::to_string(resp.status_code) + ":\n```\n" + resp.body_string() + "\n```");
    }

    nlohmann::json resp_json = nlohmann::json::parse(resp.body_string());
    double total_cost = 0.0;
    if (resp_json.contains("usageMetadata")) {
        if (fast) {
            total_cost = resp_json["usageMetadata"].value("promptTokenCount", 0) * 0.0000015 + resp_json["usageMetadata"].value("candidatesTokenCount", 0) * 0.000009;
        } else {
            total_cost = resp_json["usageMetadata"].value("promptTokenCount", 0) * 0.000002 + resp_json["usageMetadata"].value("candidatesTokenCount", 0) * 0.000012;
        }
    }

    GeminiResponse result;
    result.text = resp_json["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
    result.model_version = resp_json["modelVersion"].get<std::string>();
    result.cost = total_cost;
    
    return result;
}
