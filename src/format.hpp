#pragma once

#include <cstdint>
#include <string>

namespace diskscout {

// 1536 -> "1.5 KB". Uses 1024-based units, matching what Windows Explorer shows.
std::string format_bytes(std::uint64_t bytes);

// Escapes a UTF-8 string for use inside a JSON string literal (no surrounding quotes).
std::string json_escape(const std::string& text);

}  // namespace diskscout
