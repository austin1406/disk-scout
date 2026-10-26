#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace diskscout {

// One scanned directory. bytes/files/newest cover the whole subtree after scan() returns.
struct DirStats {
    std::string key;         // generic ("/") path, used for lookups
    std::string parent_key;  // empty for the scan root
    std::string path;        // native path, for display
    std::string name;
    int depth = 0;           // 0 = scan root
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    bool has_mtime = false;
    std::filesystem::file_time_type newest{};  // newest file modification time in the subtree
};

struct ScanOptions {
    unsigned threads = 0;  // 0 = hardware concurrency
};

struct ScanResult {
    std::string root;
    unsigned threads = 0;
    double seconds = 0.0;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_files = 0;
    std::uint64_t total_dirs = 0;  // includes the root
    std::uint64_t skipped_links = 0;
    std::uint64_t skipped_cloud = 0;
    std::uint64_t errors = 0;
    std::vector<DirStats> dirs;
};

struct Candidate {
    std::string path;
    std::string kind;
    std::uint64_t bytes = 0;
    long long days_untouched = -1;  // -1 when the directory holds no readable files
};

// Walks `root` in parallel. Symlinks/junctions are not followed, and on Windows files that
// exist only in the cloud (OneDrive "online-only") are skipped so scanning never triggers
// a download. Throws std::runtime_error if `root` is not a readable directory.
ScanResult scan(const std::filesystem::path& root, const ScanOptions& options = {});

// Largest directories with 1 <= depth <= max_depth, biggest first.
std::vector<DirStats> largest_directories(const ScanResult& result, int max_depth, std::size_t top_k);

// Directories that look like regenerable dev artifacts (node_modules, virtualenvs, caches).
// Nested matches are folded into the outermost one so nothing is counted twice.
// Report only: this library never deletes anything.
std::vector<Candidate> find_reclaimable(const ScanResult& result);

}  // namespace diskscout
