#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "format.hpp"
#include "scanner.hpp"

namespace fs = std::filesystem;
using namespace diskscout;

namespace {

struct Args {
    fs::path root = ".";
    unsigned threads = 0;
    std::size_t top = 15;
    int depth = 2;
    long long min_age_days = 0;
    bool json = false;
    bool bench = false;
    bool help = false;
};

void print_usage(std::ostream& out) {
    out << "disk-scout: find what is using your disk, in parallel. Report only, never deletes.\n\n"
           "usage: disk-scout [path] [options]\n\n"
           "  --threads N         worker threads (default: hardware concurrency)\n"
           "  --top K             rows in the largest-directories table (default: 15)\n"
           "  --depth D           how many levels below the root to list (default: 2)\n"
           "  --min-age-days N    only list reclaimable candidates untouched for N+ days\n"
           "  --json              machine-readable output\n"
           "  --bench             compare 1 thread against N threads on this path\n"
           "  --help              show this message\n";
}

bool parse_number(const std::string& text, long long& out) {
    try {
        std::size_t used = 0;
        out = std::stoll(text, &used);
        return used == text.size() && out >= 0;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_args(int argc, char** argv, Args& args, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_number = [&](long long& value) {
            if (i + 1 >= argc || !parse_number(argv[i + 1], value)) {
                error = "option " + arg + " needs a non-negative number";
                return false;
            }
            ++i;
            return true;
        };

        long long value = 0;
        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--json") {
            args.json = true;
        } else if (arg == "--bench") {
            args.bench = true;
        } else if (arg == "--threads") {
            if (!next_number(value)) return false;
            args.threads = static_cast<unsigned>(value);
        } else if (arg == "--top") {
            if (!next_number(value)) return false;
            args.top = static_cast<std::size_t>(value);
        } else if (arg == "--depth") {
            if (!next_number(value)) return false;
            args.depth = static_cast<int>(value);
        } else if (arg == "--min-age-days") {
            if (!next_number(value)) return false;
            args.min_age_days = value;
        } else if (!arg.empty() && arg[0] == '-') {
            error = "unknown option: " + arg;
            return false;
        } else {
            args.root = arg;
        }
    }
    return true;
}

std::vector<Candidate> filtered_candidates(const ScanResult& result, long long min_age_days) {
    std::vector<Candidate> all = find_reclaimable(result);
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](const Candidate& c) { return c.days_untouched < min_age_days; }),
              all.end());
    return all;
}

void print_human(const ScanResult& result, const Args& args) {
    std::cout << "Scanned " << result.root << " in " << std::fixed << std::setprecision(2)
              << result.seconds << " s using " << result.threads << " thread(s)\n";
    std::cout << "  directories: " << result.total_dirs << "   files: " << result.total_files
              << "   total: " << format_bytes(result.total_bytes) << "\n";
    std::cout << "  skipped: " << result.skipped_links << " link(s), " << result.skipped_cloud
              << " cloud-only file(s); " << result.errors << " unreadable\n\n";

    std::cout << "Largest directories (up to " << args.depth << " level(s) below the root)\n";
    for (const DirStats& dir : largest_directories(result, args.depth, args.top)) {
        std::cout << std::setw(10) << format_bytes(dir.bytes) << "  " << dir.path << "\n";
    }

    const std::vector<Candidate> candidates = filtered_candidates(result, args.min_age_days);
    std::uint64_t reclaimable = 0;
    for (const Candidate& c : candidates) reclaimable += c.bytes;

    std::cout << "\nReclaimable candidates (report only, nothing is deleted)\n";
    if (candidates.empty()) std::cout << "  none found\n";
    for (const Candidate& c : candidates) {
        std::cout << std::setw(10) << format_bytes(c.bytes) << "  " << c.path << "  [" << c.kind;
        if (c.days_untouched >= 0) std::cout << ", untouched " << c.days_untouched << " day(s)";
        std::cout << "]\n";
    }
    if (!candidates.empty()) {
        std::cout << "  total reclaimable if regenerated later: " << format_bytes(reclaimable) << "\n";
    }
}

void print_json(const ScanResult& result, const Args& args) {
    std::cout << "{\n";
    std::cout << "  \"root\": \"" << json_escape(result.root) << "\",\n";
    std::cout << "  \"threads\": " << result.threads << ",\n";
    std::cout << "  \"seconds\": " << std::fixed << std::setprecision(4) << result.seconds << ",\n";
    std::cout << "  \"total_bytes\": " << result.total_bytes << ",\n";
    std::cout << "  \"total_files\": " << result.total_files << ",\n";
    std::cout << "  \"total_dirs\": " << result.total_dirs << ",\n";
    std::cout << "  \"skipped_links\": " << result.skipped_links << ",\n";
    std::cout << "  \"skipped_cloud\": " << result.skipped_cloud << ",\n";
    std::cout << "  \"errors\": " << result.errors << ",\n";

    std::cout << "  \"largest\": [";
    const std::vector<DirStats> largest = largest_directories(result, args.depth, args.top);
    for (std::size_t i = 0; i < largest.size(); ++i) {
        std::cout << (i ? "," : "") << "\n    {\"path\": \"" << json_escape(largest[i].path)
                  << "\", \"bytes\": " << largest[i].bytes << "}";
    }
    std::cout << (largest.empty() ? "" : "\n  ") << "],\n";

    std::cout << "  \"reclaimable\": [";
    const std::vector<Candidate> candidates = filtered_candidates(result, args.min_age_days);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        std::cout << (i ? "," : "") << "\n    {\"path\": \"" << json_escape(candidates[i].path)
                  << "\", \"kind\": \"" << json_escape(candidates[i].kind)
                  << "\", \"bytes\": " << candidates[i].bytes
                  << ", \"days_untouched\": " << candidates[i].days_untouched << "}";
    }
    std::cout << (candidates.empty() ? "" : "\n  ") << "]\n}\n";
}

int run_bench(const fs::path& root, unsigned threads) {
    const unsigned many = threads != 0 ? threads : std::max(1u, std::thread::hardware_concurrency());
    const int rounds = 3;

    scan(root, ScanOptions{many});  // warm the OS file cache so no run pays the cold-disk cost

    double best_one = 1e18;
    double best_many = 1e18;
    ScanResult one_result;
    ScanResult many_result;
    for (int i = 0; i < rounds; ++i) {
        one_result = scan(root, ScanOptions{1});
        many_result = scan(root, ScanOptions{many});
        best_one = std::min(best_one, one_result.seconds);
        best_many = std::min(best_many, many_result.seconds);
    }

    const bool same = one_result.total_bytes == many_result.total_bytes &&
                      one_result.total_files == many_result.total_files &&
                      one_result.total_dirs == many_result.total_dirs;

    std::cout << "Benchmark on " << one_result.root << " (best of " << rounds << " warm runs)\n";
    std::cout << "  files: " << many_result.total_files << "   directories: " << many_result.total_dirs
              << "   total: " << format_bytes(many_result.total_bytes) << "\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  1 thread : " << best_one << " s\n";
    std::cout << "  " << many << " threads: " << best_many << " s   ("
              << std::setprecision(1) << (best_one / best_many) << "x faster)\n";
    std::cout << "  totals identical across thread counts: " << (same ? "yes" : "NO (files changed mid-run?)")
              << "\n";
    return same ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    std::string error;
    if (!parse_args(argc, argv, args, error)) {
        std::cerr << "error: " << error << "\n\n";
        print_usage(std::cerr);
        return 1;
    }
    if (args.help) {
        print_usage(std::cout);
        return 0;
    }

    try {
        if (args.bench) return run_bench(args.root, args.threads);

        const ScanResult result = scan(args.root, ScanOptions{args.threads});
        if (args.json) {
            print_json(result, args);
        } else {
            print_human(result, args);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
