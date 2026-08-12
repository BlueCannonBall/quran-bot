#pragma once
#include <string>
#include <vector>

struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

struct SearchResults {
    std::vector<SearchResult> results;
    // Set when the backend refused or failed, as opposed to genuinely finding nothing.
    // The two must not be conflated: "no results" invites the model to conclude that
    // nothing on the subject exists.
    bool unavailable = false;
};

// Searches the web. Uses the Brave Search API if QURAN_BRAVE_API_KEY is set,
// and Mojeek otherwise, with Wikipedia covering the languages Mojeek lacks.
SearchResults web_search(const std::string& query, size_t max_results = 5);

// Renders results into the plain text handed back to the model
std::string format_search_results(const SearchResults& search_results);
