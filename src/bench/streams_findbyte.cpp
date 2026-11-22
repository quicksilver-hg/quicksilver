// Copyright (c) 2023 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>

#include <streams.h>
#include <random.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>

static void FindByte(benchmark::Bench& bench)
{
    // Setup: a uniquely named file in the temp directory, never a fixed name in the cwd.
    // The guard removes it on every exit path; it is declared before the file, so the
    // file is closed first.
    const fs::path path{fs::path{fs::temp_directory_path()} / fs::u8path("quicksilver_bench_findbyte_" + GetRandHash().ToString())};
    struct RemoveOnExit {
        const fs::path& p;
        ~RemoveOnExit() { std::error_code ec; fs::remove(p, ec); }
    } remove_on_exit{path};
    AutoFile file{fsbridge::fopen(path, "w+b")};
    if (file.IsNull()) throw std::runtime_error("FindByte: cannot create " + fs::PathToString(path));
    const size_t file_size = 200;
    uint8_t data[file_size] = {0};
    data[file_size-1] = 1;
    file << data;
    file.seek(0, SEEK_SET);
    BufferedFile bf{file, /*nBufSize=*/file_size + 1, /*nRewindIn=*/file_size};

    bench.run([&] {
        bf.SetPos(0);
        bf.FindByte(std::byte(1));
    });

    // Cleanup: close the file; remove_on_exit deletes it.
    file.fclose();
}

BENCHMARK(FindByte, benchmark::PriorityLevel::HIGH);
