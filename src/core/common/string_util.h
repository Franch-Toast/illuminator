#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace illuminator {

template <typename T>
inline std::vector<T> ParseCommaSeparated(std::string_view input) {
    std::vector<T> result;
    size_t pos = 0;
    while (pos < input.size()) {
        // skip whitespace
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t')) ++pos;
        if (pos >= input.size()) break;

        size_t end = input.find(',', pos);
        if (end == std::string_view::npos) end = input.size();

        std::string_view token = input.substr(pos, end - pos);
        // trim trailing whitespace
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);

        if (!token.empty()) {
            try {
                if constexpr (std::is_same_v<T, uint32_t>) {
                    result.push_back(static_cast<uint32_t>(std::stoul(std::string(token))));
                } else if constexpr (std::is_same_v<T, int>) {
                    result.push_back(std::stoi(std::string(token)));
                } else {
                    result.push_back(static_cast<T>(std::stoll(std::string(token))));
                }
            } catch (...) {}
        }
        pos = (end == input.size()) ? end : end + 1;
    }
    return result;
}

}  // namespace illuminator
