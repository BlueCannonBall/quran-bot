#pragma once
#include <string>
#include <vector>

struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

// Searches the web. Uses the Brave Search API if QURAN_BRAVE_API_KEY is set,
// and falls back to Mojeek otherwise. Returns an empty vector on failure.
std::vector<SearchResult> web_search(const std::string& query, size_t max_results = 5);

// Renders results into the plain text handed back to the model
std::string format_search_results(const std::vector<SearchResult>& results);
