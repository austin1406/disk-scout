#include "scanner.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "thread_pool.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace diskscout {
namespace {

// True for OneDrive-style placeholders whose contents are not on local disk. Reading
// their metadata is fine, but they take no local space, so counting them would be wrong.
bool is_cloud_only(const fs::path& path) {
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    constexpr DWORD kCloudMask =
        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;
    return (attrs & kCloudMask) != 0;
#else
    (void)path;
    return false;
#endif
}

bool is_link_like(const fs::file_status& status) {
    if (status.type() == fs::file_type::symlink) return true;
#if defined(_MSC_VER)
    if (status.type() == fs::file_type::junction) return true;
#endif
    return false;
}

class Walker {
public:
    explicit Walker(unsigned threads) : pool_(threads) {}

    void run(const fs::path& root) {
        submit_dir(root, std::string(), 0);
        pool_.wait_idle();
    }

    std::vector<DirStats> take_records() { return std::move(records_); }

    std::atomic<std::uint64_t> links{0};
    std::atomic<std::uint64_t> cloud{0};
    std::atomic<std::uint64_t> errors{0};

private:
    void submit_dir(fs::path dir, std::string parent_key, int depth) {
        pool_.submit([this, dir = std::move(dir), parent_key = std::move(parent_key), depth]() {
            visit(dir, parent_key, depth);
        });
    }

    void visit(const fs::path& dir, const std::string& parent_key, int depth) {
        DirStats record;
        record.key = dir.generic_u8string();
        record.parent_key = parent_key;
        record.path = dir.u8string();
        record.name = dir.filename().u8string();
        record.depth = depth;

        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            ++errors;
        } else {
            const fs::directory_iterator end;
            while (it != end) {
                handle_entry(*it, record, depth);
                it.increment(ec);
                if (ec) {
                    ++errors;
                    break;
                }
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(std::move(record));
    }

    void handle_entry(const fs::directory_entry& entry, DirStats& record, int depth) {
        std::error_code ec;
        const fs::file_status status = entry.symlink_status(ec);
        if (ec) {
            ++errors;
            return;
        }
        if (is_link_like(status)) {
            ++links;
            return;
        }
        if (is_cloud_only(entry.path())) {
            ++cloud;
            return;
        }

        if (fs::is_directory(status)) {
            submit_dir(entry.path(), record.key, depth + 1);
        } else if (fs::is_regular_file(status)) {
            const std::uintmax_t size = entry.file_size(ec);
            if (ec) {
                ++errors;
                return;
            }
            record.bytes += size;
            ++record.files;

            std::error_code time_ec;
            const auto mtime = entry.last_write_time(time_ec);
            if (!time_ec && (!record.has_mtime || mtime > record.newest)) {
                record.newest = mtime;
                record.has_mtime = true;
            }
        }
    }

    ThreadPool pool_;
    std::mutex mutex_;
    std::vector<DirStats> records_;
};

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const char* reclaimable_kind(const std::string& name) {
    static const std::unordered_map<std::string, const char*> kKinds = {
        {"node_modules", "npm dependencies"},
        {"__pycache__", "Python bytecode cache"},
        {".venv", "Python virtual environment"},
        {"venv", "Python virtual environment"},
        {".pytest_cache", "pytest cache"},
        {".mypy_cache", "mypy cache"},
        {".gradle", "Gradle cache"},
        {".next", "Next.js build output"},
        {".nuxt", "Nuxt build output"},
    };
    const auto it = kKinds.find(to_lower(name));
    return it == kKinds.end() ? nullptr : it->second;
}

bool has_flagged_ancestor(std::string key, const std::unordered_set<std::string>& flagged) {
    for (;;) {
        const auto slash = key.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return false;
        key.resize(slash);
        if (flagged.count(key) != 0) return true;
    }
}

}  // namespace

ScanResult scan(const fs::path& root, const ScanOptions& options) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        throw std::runtime_error("not a readable directory: " + root.u8string());
    }

    ScanResult result;
    result.root = root.u8string();
    result.threads = options.threads != 0 ? options.threads : std::max(1u, std::thread::hardware_concurrency());

    const auto start = std::chrono::steady_clock::now();

    Walker walker(result.threads);
    walker.run(root);
    std::vector<DirStats> records = walker.take_records();
    result.skipped_links = walker.links;
    result.skipped_cloud = walker.cloud;
    result.errors = walker.errors;

    // Each task only saw its own files. Roll subtree totals up deepest-first so a
    // directory is complete before it is added to its parent.
    std::unordered_map<std::string, std::size_t> index;
    index.reserve(records.size() * 2);
    for (std::size_t i = 0; i < records.size(); ++i) index[records[i].key] = i;

    std::vector<std::size_t> order(records.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(),
              [&records](std::size_t a, std::size_t b) { return records[a].depth > records[b].depth; });

    for (const std::size_t i : order) {
        const DirStats& child = records[i];
        if (child.parent_key.empty()) continue;
        const auto parent_it = index.find(child.parent_key);
        if (parent_it == index.end()) continue;

        DirStats& parent = records[parent_it->second];
        parent.bytes += child.bytes;
        parent.files += child.files;
        if (child.has_mtime && (!parent.has_mtime || child.newest > parent.newest)) {
            parent.newest = child.newest;
            parent.has_mtime = true;
        }
    }

    for (const DirStats& record : records) {
        if (record.depth == 0) {
            result.total_bytes = record.bytes;
            result.total_files = record.files;
        }
    }
    result.total_dirs = records.size();
    result.dirs = std::move(records);
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

std::vector<DirStats> largest_directories(const ScanResult& result, int max_depth, std::size_t top_k) {
    std::vector<DirStats> picked;
    for (const DirStats& dir : result.dirs) {
        if (dir.depth >= 1 && dir.depth <= max_depth) picked.push_back(dir);
    }
    std::sort(picked.begin(), picked.end(), [](const DirStats& a, const DirStats& b) {
        if (a.bytes != b.bytes) return a.bytes > b.bytes;
        return a.key < b.key;
    });
    if (picked.size() > top_k) picked.resize(top_k);
    return picked;
}

std::vector<Candidate> find_reclaimable(const ScanResult& result) {
    const auto now = fs::file_time_type::clock::now();

    std::unordered_set<std::string> flagged;
    for (const DirStats& dir : result.dirs) {
        if (dir.depth > 0 && reclaimable_kind(dir.name) != nullptr) flagged.insert(dir.key);
    }

    std::vector<Candidate> candidates;
    for (const DirStats& dir : result.dirs) {
        if (flagged.count(dir.key) == 0 || dir.bytes == 0) continue;
        if (has_flagged_ancestor(dir.key, flagged)) continue;

        Candidate candidate;
        candidate.path = dir.path;
        candidate.kind = reclaimable_kind(dir.name);
        candidate.bytes = dir.bytes;
        if (dir.has_mtime) {
            const auto age = std::chrono::duration_cast<std::chrono::hours>(now - dir.newest).count();
            candidate.days_untouched = std::max<long long>(0, age / 24);
        }
        candidates.push_back(std::move(candidate));
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.bytes != b.bytes) return a.bytes > b.bytes;
        return a.path < b.path;
    });
    return candidates;
}

}  // namespace diskscout
