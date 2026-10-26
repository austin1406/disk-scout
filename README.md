# disk-scout

[![CI](https://github.com/austin1406/disk-scout/actions/workflows/ci.yml/badge.svg)](https://github.com/austin1406/disk-scout/actions/workflows/ci.yml)

A small C++17 command-line tool that scans a folder in parallel and tells you what is using
your disk, including which dev artifacts (`node_modules`, virtualenvs, caches) you could
regenerate. It is **report-only: it never deletes anything.**

I wrote it because my own C: drive hit 0.1 GB free while I was pulling a model, and I wanted
to know where the space had gone without clicking through folders.

## Build and run

Requires a C++17 compiler (MSVC 2019+, GCC 9+, or Clang 9+) and CMake 3.16+.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure      # run the unit tests

./build/disk-scout [path] [--threads N] [--top K] [--depth D] [--min-age-days N] [--json]
./build/disk-scout [path] --bench               # 1 thread vs N threads
```

## Example

Scanning one of my project folders (paths shortened):

```
Scanned ...\GitHub in 21.39 s using 10 thread(s)
  directories: 25693   files: 48721   total: 1.5 GB
  skipped: 0 link(s), 151312 cloud-only file(s); 0 unreadable

Largest directories (up to 2 level(s) below the root)
    1.3 GB  ...\GitHub\rag-streamflow-assistant
    1.3 GB  ...\GitHub\rag-streamflow-assistant\.venv
   96.3 MB  ...\GitHub\PneuVision
   ...

Reclaimable candidates (report only, nothing is deleted)
    1.3 GB  ...\rag-streamflow-assistant\.venv  [Python virtual environment, untouched 34 day(s)]
   31.0 KB  ...\rag-streamflow-assistant\tests\__pycache__  [Python bytecode cache, untouched 34 day(s)]
```

## Measured results

On a fully local folder (44,908 files, 5,004 directories, 1.3 GB), best of 3 warm runs on a
10-thread machine:

| Threads | Time |
|---|---|
| 1 | 3.738 s |
| 10 | 1.083 s (3.5x faster) |

Totals were identical across thread counts. These numbers are warm-cache and specific to my
machine, so treat the speedup as indicative, not a guarantee.

## How it works

- **Parallel walk.** Each directory is one task on a fixed-size `ThreadPool`. A task lists its
  directory, adds up its own files, and submits a task per subdirectory. `wait_idle()` returns
  only after the whole cascade of tasks has finished.
- **Subtree totals without locking per file.** Tasks only record their own files. After the
  walk, totals are rolled up deepest-first so every directory is complete before it is added
  to its parent. The result is deterministic regardless of thread count (there is a test for it).
- **Safe by default.** Symlinks and Windows junctions are not followed, so nothing is counted
  twice. On Windows, files that exist only in the cloud (OneDrive "online-only") are skipped,
  because they use no local space and reading them could trigger a download.
- **No double counting.** Nested artifact folders (a `node_modules` inside a `node_modules`)
  are folded into the outermost one.

## Tests

18 GoogleTest cases cover the thread pool (nested task submission, throwing tasks, shutdown
draining), the scanner (exact totals and rollups on known trees, identical results for 1, 2
and 16 threads, symlinks, empty and missing roots), reclaimable detection (case-insensitive,
nesting, age from the newest file), and the output formatting.

Built and tested locally with MSVC 19.44 on Windows. `.github/workflows/ci.yml` builds and
runs all the tests on every push on both Windows (MSVC) and Ubuntu (GCC); both pass.

## Limitations

- It keeps one record per directory in memory, which is fine for a dev machine but not for a
  filesystem with hundreds of millions of directories.
- The cloud-only check uses Windows file attributes, so on Linux and macOS that step is a no-op.
- Deletion is deliberately not implemented. Reviewing the report and removing things yourself
  is safer than a tool that guesses.

## License

MIT
