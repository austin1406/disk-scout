#include "format.hpp"

#include <cstdio>

namespace diskscout {

std::string format_bytes(std::uint64_t bytes) {
    static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }

    char buffer[32];
    if (unit == 0) {
        std::snprintf(buffer, sizeof buffer, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.1f %s", value, kUnits[unit]);
    }
    return buffer;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
                    out += escaped;
                } else {
                    out += raw;
                }
        }
    }
    return out;
}

}  // namespace diskscout
