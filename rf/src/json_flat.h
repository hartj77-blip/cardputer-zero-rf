// Minimal reader for one-line JSON objects such as rtl_433 prints:
// top-level keys and their values as text. Strings are unescaped; numbers,
// true/false/null and nested arrays or objects are returned as written.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace zrf {

using JsonFields = std::vector<std::pair<std::string, std::string>>;

// Empty result if the line is not a JSON object.
inline JsonFields parse_flat_json(const std::string& s) {
    JsonFields out;
    size_t i = 0;
    auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; };
    auto str = [&](std::string& v) {
        if (i >= s.size() || s[i] != '"') return false;
        for (++i; i < s.size() && s[i] != '"'; ++i) {
            if (s[i] != '\\') { v += s[i]; continue; }
            if (++i >= s.size()) return false;
            switch (s[i]) {
            case 'n': v += '\n'; break;
            case 't': v += '\t'; break;
            case 'u': v += '?'; i += 4; break;   // non-ASCII: not needed for a small screen
            default: v += s[i];
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return {};
    ++i;
    for (;;) {
        ws();
        if (i < s.size() && s[i] == '}') return out;
        std::string key, value;
        if (!str(key)) return {};
        ws();
        if (i >= s.size() || s[i] != ':') return {};
        ++i;
        ws();
        if (i < s.size() && s[i] == '"') {
            if (!str(value)) return {};
        } else {
            // Bare value; nested brackets are copied whole.
            int depth = 0;
            bool quoted = false;
            for (; i < s.size(); ++i) {
                char c = s[i];
                if (quoted) { if (c == '\\') value += s[i++]; else if (c == '"') quoted = false; }
                else if (c == '"') quoted = true;
                else if (c == '[' || c == '{') ++depth;
                else if (c == ']' || c == '}') { if (depth == 0) break; --depth; }
                else if (c == ',' && depth == 0) break;
                if (i < s.size()) value += s[i];
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
            if (value.empty()) return {};
        }
        out.emplace_back(std::move(key), std::move(value));
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') return out;
        return {};
    }
}

inline const std::string* json_get(const JsonFields& f, const char* key) {
    for (auto& kv : f)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

}  // namespace zrf
