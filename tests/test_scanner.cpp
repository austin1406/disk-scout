#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <string>

#include "scanner.hpp"

namespace fs = std::filesystem;
using namespace diskscout;

namespace {

// Creates a unique scratch directory and removes it afterwards.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("disk_scout_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& path, std::size_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(size, 'x');
}

const DirStats* find_dir(const ScanResult& result, const fs::path& path) {
    const std::string key = path.generic_u8string();
    for (const DirStats& dir : result.dirs) {
        if (dir.key == key) return &dir;
    }
    return nullptr;
}

}  // namespace

TEST(Scanner, TotalsAndSubtreeRollupsMatchAKnownTree) {
    TempDir tmp;
    const fs::path root = tmp.path();
    write_file(root / "top.txt", 7);
    write_file(root / "a" / "f1.bin", 100);
    write_file(root / "a" / "b" / "f2.bin", 250);
    write_file(root / "c" / "f3.bin", 1000);

    const ScanResult result = scan(root, ScanOptions{4});

    EXPECT_EQ(result.total_bytes, 1357u);
    EXPECT_EQ(result.total_files, 4u);
    EXPECT_EQ(result.total_dirs, 4u);  // root, a, a/b, c
    EXPECT_EQ(result.errors, 0u);

    ASSERT_NE(find_dir(result, root / "a"), nullptr);
    EXPECT_EQ(find_dir(result, root / "a")->bytes, 350u);
    EXPECT_EQ(find_dir(result, root / "a" / "b")->bytes, 250u);
    EXPECT_EQ(find_dir(result, root / "c")->bytes, 1000u);
}

TEST(Scanner, EmptyDirectoryScansCleanly) {
    TempDir tmp;
    const ScanResult result = scan(tmp.path());
    EXPECT_EQ(result.total_bytes, 0u);
    EXPECT_EQ(result.total_files, 0u);
    EXPECT_EQ(result.total_dirs, 1u);
}

TEST(Scanner, MissingRootThrows) {
    TempDir tmp;
    EXPECT_THROW(scan(tmp.path() / "does_not_exist"), std::runtime_error);
}

TEST(Scanner, ResultsAreIdenticalForAnyThreadCount) {
    TempDir tmp;
    const fs::path root = tmp.path();
    for (int d = 0; d < 30; ++d) {
        for (int f = 0; f < 10; ++f) {
            write_file(root / ("dir" + std::to_string(d)) / ("sub" + std::to_string(d % 3)) /
                           ("file" + std::to_string(f)),
                       static_cast<std::size_t>(d * 13 + f * 7 + 1));
        }
    }

    auto snapshot = [](const ScanResult& r) {
        std::map<std::string, std::uint64_t> sizes;
        for (const DirStats& dir : r.dirs) sizes[dir.key] = dir.bytes;
        return sizes;
    };

    const ScanResult one = scan(root, ScanOptions{1});
    const ScanResult two = scan(root, ScanOptions{2});
    const ScanResult many = scan(root, ScanOptions{16});

    EXPECT_EQ(one.total_bytes, two.total_bytes);
    EXPECT_EQ(one.total_bytes, many.total_bytes);
    EXPECT_EQ(one.total_files, 300u);
    EXPECT_EQ(one.total_dirs, many.total_dirs);
    EXPECT_EQ(snapshot(one), snapshot(two));
    EXPECT_EQ(snapshot(one), snapshot(many));
}

TEST(Scanner, SymlinksAreSkippedInsteadOfDoubleCounted) {
    TempDir tmp;
    const fs::path root = tmp.path();
    write_file(root / "real" / "data.bin", 500);

    std::error_code ec;
    fs::create_directory_symlink(root / "real", root / "link", ec);
    if (ec) GTEST_SKIP() << "cannot create symlinks here: " << ec.message();

    const ScanResult result = scan(root);
    EXPECT_EQ(result.total_bytes, 500u);
    EXPECT_EQ(result.skipped_links, 1u);
}

TEST(Reclaimable, FindsArtifactsAndFoldsNestedOnesIntoTheOutermost) {
    TempDir tmp;
    const fs::path root = tmp.path();
    write_file(root / "proj" / "node_modules" / "pkg" / "node_modules" / "deep.js", 500);
    write_file(root / "proj" / "node_modules" / "other.js", 300);
    write_file(root / "proj" / "src" / "main.js", 10);
    write_file(root / "proj" / "__pycache__" / "x.pyc", 40);

    const ScanResult result = scan(root, ScanOptions{4});
    const std::vector<Candidate> candidates = find_reclaimable(result);

    ASSERT_EQ(candidates.size(), 2u);  // nested node_modules is not listed separately
    EXPECT_EQ(candidates[0].bytes, 800u);
    EXPECT_EQ(candidates[0].kind, "npm dependencies");
    EXPECT_EQ(candidates[1].bytes, 40u);
    EXPECT_EQ(candidates[1].kind, "Python bytecode cache");
}

TEST(Reclaimable, MatchingIsCaseInsensitiveAndIgnoresEmptyDirectories) {
    TempDir tmp;
    const fs::path root = tmp.path();
    write_file(root / "Node_Modules" / "a.js", 64);
    fs::create_directories(root / "venv");  // empty: nothing to reclaim

    const std::vector<Candidate> candidates = find_reclaimable(scan(root));
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].bytes, 64u);
}

TEST(Reclaimable, AgeComesFromTheNewestFileInTheSubtree) {
    TempDir tmp;
    const fs::path root = tmp.path();
    const fs::path old_file = root / "node_modules" / "old.js";
    const fs::path newer_file = root / "node_modules" / "nested" / "newer.js";
    write_file(old_file, 10);
    write_file(newer_file, 10);

    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(old_file, now - std::chrono::hours(24 * 200));
    fs::last_write_time(newer_file, now - std::chrono::hours(24 * 30));

    const std::vector<Candidate> candidates = find_reclaimable(scan(root));
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].days_untouched, 30);
}

TEST(LargestDirectories, RespectsDepthLimitAndSortsBySize) {
    TempDir tmp;
    const fs::path root = tmp.path();
    write_file(root / "small" / "f", 10);
    write_file(root / "big" / "f", 5000);
    write_file(root / "big" / "inner" / "f", 9000);   // depth 2 inside "big"
    write_file(root / "mid" / "f", 700);

    const ScanResult result = scan(root, ScanOptions{4});

    const auto depth1 = largest_directories(result, 1, 10);
    ASSERT_EQ(depth1.size(), 3u);
    EXPECT_EQ(depth1[0].name, "big");  // 14000 bytes including inner
    EXPECT_EQ(depth1[1].name, "mid");
    EXPECT_EQ(depth1[2].name, "small");

    const auto depth2 = largest_directories(result, 2, 10);
    EXPECT_EQ(depth2.size(), 4u);  // adds big/inner

    EXPECT_EQ(largest_directories(result, 2, 2).size(), 2u);  // top-K limit
}
