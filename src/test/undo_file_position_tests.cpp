// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockfileinfostore.h"
#include "chain.h"
#include "config.h"
#include "consensus/validation.h"
#include "streams.h"
#include "test/test_bitcoin.h"
#include "txdb.h"
#include "util.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef WIN32
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#endif

namespace {

// CBlockTreeDB chooses its path through GetDataDir(), even for an in-memory DB.
// Keep that lookup inside a dedicated temporary directory as well.
struct IsolatedDataDir {
    fs::path path{fs::temp_directory_path() / fs::unique_path("undo-position-%%%%-%%%%")};
    std::optional<std::string> previous;

    IsolatedDataDir() {
        if (gArgs.IsArgSet("-datadir")) {
            previous = gArgs.GetArg("-datadir", "");
        }
        fs::create_directories(path);
        gArgs.ForceSetArg("-datadir", path.string());
        ClearDatadirCache();
    }

    ~IsolatedDataDir() {
        if (previous) {
            gArgs.ForceSetArg("-datadir", *previous);
        } else {
            gArgs.ClearArg("-datadir");
        }
        ClearDatadirCache();
        fs::remove_all(path);
    }
};

// FindBlockPos finalizes the production global store when rotating files.
// Exercise that existing contract while restoring the original store afterward.
struct ScopedGlobalBlockFileStore {
    std::unique_ptr<CBlockFileInfoStore> saved{
        std::make_unique<CBlockFileInfoStore>()};

    ScopedGlobalBlockFileStore() {
        pBlockFileInfoStore.swap(saved);
    }

    ~ScopedGlobalBlockFileStore() {
        pBlockFileInfoStore.swap(saved);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(undo_file_position_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(position_varint_round_trip_and_legacy_encoding) {
    const std::array<uint64_t, 10> offsets{{
        0, 1, (uint64_t{1} << 31) - 1, uint64_t{1} << 31,
        (uint64_t{1} << 31) + 1, (uint64_t{1} << 32) - 1,
        uint64_t{1} << 32, (uint64_t{1} << 32) + 1,
        (uint64_t{1} << 32) + 64, MAX_DISK_FILE_OFFSET}};
    for (const auto offset : offsets) {
        const CDiskBlockPos original(7, offset);
        BOOST_REQUIRE_EQUAL(original.nPos, offset);
        CDataStream encoded(SER_DISK, CLIENT_VERSION);
        encoded << original;
        CDataStream input(encoded);
        CDiskBlockPos decoded;
        input >> decoded;
        BOOST_CHECK_EQUAL(decoded.nFile, 7);
        BOOST_CHECK_EQUAL(decoded.nPos, offset);
        BOOST_CHECK(input.empty());

        if (offset <= std::numeric_limits<uint32_t>::max()) {
            // Encoding a small old uint32_t position must produce identical bytes.
            CDataStream legacy(SER_DISK, CLIENT_VERSION);
            int oldFile{7};
            uint32_t oldOffset = static_cast<uint32_t>(offset);
            legacy << VARINT(oldFile) << VARINT(oldOffset);
            BOOST_CHECK_EQUAL_COLLECTIONS(encoded.begin(), encoded.end(),
                                          legacy.begin(), legacy.end());
        } else {
            CDataStream legacyInput(encoded);
            int oldFile{0};
            uint32_t oldOffset{0};
            legacyInput >> VARINT(oldFile);
            BOOST_CHECK_THROW(legacyInput >> VARINT(oldOffset), std::runtime_error);
        }
    }
}

BOOST_AUTO_TEST_CASE(block_index_preserves_large_positions) {
    for (const uint64_t offset : {uint64_t{0}, (uint64_t{1} << 32) - 1,
                                 uint64_t{1} << 32, (uint64_t{1} << 32) + 72}) {
        CDiskBlockIndex original;
        original.nFile = 7;
        original.nHeight = 3;
        original.nTx = 1;
        original.nStatus = original.nStatus.withData().withUndo();
        original.nDataPos = offset;
        original.nUndoPos = offset;
        CDataStream stream(SER_DISK, CLIENT_VERSION);
        stream << original;
        CDiskBlockIndex decoded;
        stream >> decoded;
        BOOST_CHECK_EQUAL(decoded.nDataPos, offset);
        BOOST_CHECK_EQUAL(decoded.nUndoPos, offset);
        BOOST_CHECK_EQUAL(decoded.GetBlockPos().nPos, offset);
        BOOST_CHECK_EQUAL(decoded.GetUndoPos().nPos, offset);
        BOOST_CHECK(stream.empty());
    }
}

BOOST_AUTO_TEST_CASE(file_info_extended_size_round_trip) {
    const uint64_t sentinel = std::numeric_limits<uint32_t>::max();
    for (const uint64_t undoSize : {uint64_t{0}, sentinel - 1, sentinel,
                                    sentinel + 1, sentinel + 65}) {
        CBlockFileInfo original;
        original.AddBlock(3, 100);
        original.nSize = sentinel + 100;
        original.nUndoSize = undoSize;
        CDataStream encoded(SER_DISK, CLIENT_VERSION);
        encoded << original;
        BOOST_CHECK_EQUAL(original.nSize, sentinel + 100);
        BOOST_CHECK_EQUAL(original.nUndoSize, undoSize);

        for (const uint64_t initial : {uint64_t{0}, uint64_t{99}, sentinel}) {
            CBlockFileInfo decoded;
            decoded.nSize = initial;
            decoded.nUndoSize = initial;
            CDataStream input(encoded);
            BOOST_REQUIRE_NO_THROW(input >> decoded);
            BOOST_CHECK_EQUAL(decoded.nSize, sentinel + 100);
            BOOST_CHECK_EQUAL(decoded.nUndoSize, undoSize);
            BOOST_CHECK_EQUAL(decoded.nBlocks, original.nBlocks);
            BOOST_CHECK(input.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(sparse_file_seek_tell_and_old_prefix) {
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::tmpfile(), &std::fclose);
    BOOST_REQUIRE(file.get() != nullptr);
#ifdef WIN32
    // Avoid allocating a physical 4 GiB file when extending it on Windows.
    DWORD returned{0};
    BOOST_REQUIRE(DeviceIoControl(
        reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file.get()))),
        FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
#endif
    const std::array<char, 8> prefix{{'o', 'l', 'd', '-', 'u', 'n', 'd', 'o'}};
    BOOST_REQUIRE_EQUAL(std::fwrite(prefix.data(), 1, prefix.size(), file.get()), prefix.size());
    const std::array<uint64_t, 6> offsets{{
        (uint64_t{1} << 31) - 1, uint64_t{1} << 31, (uint64_t{1} << 31) + 1,
        (uint64_t{1} << 32) - 1, uint64_t{1} << 32, (uint64_t{1} << 32) + 64}};
    for (size_t i = 0; i < offsets.size(); ++i) {
        BOOST_REQUIRE(SeekFileAbsolute64(file.get(), offsets[i]));
        uint64_t actual{0};
        BOOST_REQUIRE(TellFile64(file.get(), actual));
        BOOST_CHECK_EQUAL(actual, offsets[i]);
        BOOST_REQUIRE(std::fputc(static_cast<int>('A' + i), file.get()) != EOF);
        BOOST_REQUIRE(TellFile64(file.get(), actual));
        BOOST_CHECK_EQUAL(actual, offsets[i] + 1);
    }
    BOOST_REQUIRE_EQUAL(std::fflush(file.get()), 0);
    for (size_t i = 0; i < offsets.size(); ++i) {
        BOOST_REQUIRE(SeekFileAbsolute64(file.get(), offsets[i]));
        BOOST_CHECK_EQUAL(std::fgetc(file.get()), static_cast<int>('A' + i));
    }
    BOOST_REQUIRE(SeekFileAbsolute64(file.get(), 0));
    std::array<char, 8> readPrefix{};
    BOOST_REQUIRE_EQUAL(std::fread(readPrefix.data(), 1, readPrefix.size(), file.get()), readPrefix.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(prefix.begin(), prefix.end(), readPrefix.begin(), readPrefix.end());
    BOOST_CHECK(!SeekFileAbsolute64(file.get(), MAX_DISK_FILE_OFFSET + 1));
    uint64_t unchanged{0};
    BOOST_REQUIRE(TellFile64(file.get(), unchanged));
    BOOST_CHECK_EQUAL(unchanged, prefix.size());
}

#ifndef WIN32
BOOST_AUTO_TEST_CASE(tell_failure_keeps_output_value) {
    int descriptors[2];
    BOOST_REQUIRE_EQUAL(pipe(descriptors), 0);
    FILE* reader = fdopen(descriptors[0], "rb");
    close(descriptors[1]);
    if (!reader) {
        close(descriptors[0]);
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(reader, &std::fclose);
    BOOST_REQUIRE(file.get() != nullptr);
    uint64_t output{123};
    BOOST_CHECK(!TellFile64(file.get(), output));
    BOOST_CHECK_EQUAL(output, 123U);
}
#endif

BOOST_AUTO_TEST_CASE(file_info_database_preserves_large_undo_size) {
    IsolatedDataDir isolation;
    CBlockTreeDB db(1 << 20, true);
    for (const uint64_t undoSize : {
             uint64_t{0}, (uint64_t{1} << 32) - 1,
             (uint64_t{1} << 32) + 64}) {
        CBlockFileInfo original;
        original.AddBlock(3, 100);
        original.nSize = 100;
        original.nUndoSize = undoSize;
        BOOST_REQUIRE(db.Write(std::make_pair('f', 0), original));

        CBlockFileInfo decoded;
        BOOST_REQUIRE(db.ReadBlockFileInfo(0, decoded));
        BOOST_CHECK_EQUAL(decoded.nUndoSize, undoSize);
        BOOST_CHECK_EQUAL(decoded.nSize, original.nSize);

        // A fresh store models loading the persisted size after restart.
        CBlockFileInfoStore reloaded;
        reloaded.LoadBlockFileInfo(0, db);
        BOOST_CHECK_EQUAL(reloaded.GetBlockFileInfo(0)->nUndoSize, undoSize);
        BOOST_CHECK_EQUAL(reloaded.GetBlockFileInfo(0)->nSize, original.nSize);
    }
}

BOOST_AUTO_TEST_CASE(undo_allocation_range_failure_keeps_metadata) {
    IsolatedDataDir isolation;
    CBlockTreeDB db(1 << 20, true);
    for (const uint64_t oldSize : {MAX_DISK_FILE_OFFSET, MAX_DISK_FILE_OFFSET + 1,
                                   std::numeric_limits<uint64_t>::max()}) {
        CBlockFileInfo info;
        BOOST_REQUIRE(db.Write(std::make_pair('f', 0), info));
        CBlockFileInfoStore store;
        store.LoadBlockFileInfo(0, db);
        // Exercise the new file-position range guard directly, without
        // imposing additional validation policy on the existing DB loader.
        store.GetBlockFileInfo(0)->nUndoSize = oldSize;
        CValidationState state;
        CDiskBlockPos position(0, 0);
        bool pruning{false};
        BOOST_CHECK(!store.FindUndoPos(state, 0, position, 1, pruning));
        BOOST_CHECK(state.IsError());
        BOOST_CHECK_EQUAL(store.GetBlockFileInfo(0)->nUndoSize, oldSize);
        BOOST_CHECK(store.GetAndClearDirtyFileInfo().empty());
        BOOST_CHECK(!pruning);
    }
}

BOOST_AUTO_TEST_CASE(block_allocation_range_failure_keeps_state) {
    IsolatedDataDir isolation;
    CBlockTreeDB db(1 << 20, true);
    GlobalConfig config;
    for (const bool known : {false, true}) {
        CBlockFileInfo info;
        info.AddBlock(3, 100);
        info.nSize = 100;
        info.nUndoSize = 50;
        BOOST_REQUIRE(db.Write(std::make_pair('f', 0), info));
        CBlockFileInfoStore store;
        store.LoadBlockFileInfo(0, db);

        CDiskBlockPos position(0, known ? MAX_DISK_FILE_OFFSET : 99);
        const CDiskBlockPos beforePosition = position;
        CDataStream before(SER_DISK, CLIENT_VERSION);
        before << *store.GetBlockFileInfo(0);
        CValidationState state;
        bool pruning{true};
        const uint64_t add = known ? 1 : MAX_DISK_FILE_OFFSET + 1;
        BOOST_CHECK(!store.FindBlockPos(config, state, position, add, 4, 200,
                                       pruning, known));
        BOOST_CHECK(state.IsError());
        BOOST_CHECK(position == beforePosition);
        BOOST_CHECK_EQUAL(store.GetnLastBlockFile(), 0);
        CDataStream after(SER_DISK, CLIENT_VERSION);
        after << *store.GetBlockFileInfo(0);
        BOOST_CHECK_EQUAL_COLLECTIONS(before.begin(), before.end(),
                                      after.begin(), after.end());
        BOOST_CHECK(store.GetAndClearDirtyFileInfo().empty());
        BOOST_CHECK(pruning);
        BOOST_CHECK(!fs::exists(GetBlockPosFilename(CDiskBlockPos(0, 0), "blk")));
    }
}

BOOST_AUTO_TEST_CASE(block_file_rotation_finalizes_old_tails) {
    IsolatedDataDir isolation;
    CBlockTreeDB db(1 << 20, true);
    CBlockFileInfo original;
    original.AddBlock(1, 100);
    original.nSize = 64;
    original.nUndoSize = 8;
    BOOST_REQUIRE(db.Write(std::make_pair('f', 0), original));
    // A look-ahead entry keeps both allocations inside an existing chunk,
    // so this test needs only small files, not a new 16 MiB block allocation.
    CBlockFileInfo next;
    next.AddBlock(2, 200);
    next.nSize = 1;
    BOOST_REQUIRE(db.Write(std::make_pair('f', 1), next));
    ScopedGlobalBlockFileStore globalStore;
    CBlockFileInfoStore& store = *pBlockFileInfoStore;
    store.LoadBlockFileInfo(0, db);
    GlobalConfig config;
    config.SetPreferredBlockFileSize(96);

    const std::array<char, 64> blockPrefix = [] {
        std::array<char, 64> value{};
        value.fill('B');
        return value;
    }();
    const std::array<char, 8> undoPrefix{{'o', 'l', 'd', '-', 'u', 'n', 'd', 'o'}};
    {
        std::unique_ptr<FILE, decltype(&std::fclose)> block(
            CDiskFiles::OpenBlockFile(CDiskBlockPos(0, 0)), &std::fclose);
        std::unique_ptr<FILE, decltype(&std::fclose)> undo(
            CDiskFiles::OpenUndoFile(CDiskBlockPos(0, 0)), &std::fclose);
        BOOST_REQUIRE(block.get() != nullptr);
        BOOST_REQUIRE(undo.get() != nullptr);
        BOOST_REQUIRE_EQUAL(std::fwrite(blockPrefix.data(), 1, blockPrefix.size(),
                                       block.get()), blockPrefix.size());
        BOOST_REQUIRE_EQUAL(std::fwrite(undoPrefix.data(), 1, undoPrefix.size(),
                                       undo.get()), undoPrefix.size());
        for (FILE* file : {block.get(), undo.get()}) {
            BOOST_REQUIRE(SeekFileAbsolute64(file, 4095));
            BOOST_REQUIRE(std::fputc(0, file) != EOF);
            BOOST_REQUIRE_EQUAL(std::fflush(file), 0);
        }
        std::unique_ptr<FILE, decltype(&std::fclose)> nextBlock(
            CDiskFiles::OpenBlockFile(CDiskBlockPos(1, 0)), &std::fclose);
        BOOST_REQUIRE(nextBlock.get() != nullptr);
        BOOST_REQUIRE(std::fputc('N', nextBlock.get()) != EOF);
        BOOST_REQUIRE_EQUAL(std::fflush(nextBlock.get()), 0);
    }
    const auto oldBlock = GetBlockPosFilename(CDiskBlockPos(0, 0), "blk");
    const auto oldUndo = GetBlockPosFilename(CDiskBlockPos(0, 0), "rev");
    BOOST_REQUIRE_EQUAL(fs::file_size(oldBlock), 4096U);
    BOOST_REQUIRE_EQUAL(fs::file_size(oldUndo), 4096U);

    CDiskBlockPos below;
    CValidationState belowState;
    bool pruning{false};
    BOOST_REQUIRE(store.FindBlockPos(config, belowState, below, 31, 3, 300, pruning));
    BOOST_CHECK_EQUAL(below.nFile, 0);
    BOOST_CHECK_EQUAL(below.nPos, 64U);
    BOOST_CHECK_EQUAL(store.GetBlockFileInfo(0)->nSize, 95U);
    BOOST_CHECK_EQUAL(store.GetnLastBlockFile(), 0);
    BOOST_CHECK_EQUAL(fs::file_size(oldBlock), 4096U);
    const auto belowDirty = store.GetAndClearDirtyFileInfo();
    BOOST_REQUIRE_EQUAL(belowDirty.size(), 1U);
    BOOST_CHECK_EQUAL(belowDirty.front().first, 0);

    // Exactly reaching the preferred size must retain the legacy >= rotation.
    CDiskBlockPos atLimit;
    CValidationState atLimitState;
    BOOST_REQUIRE(store.FindBlockPos(config, atLimitState, atLimit, 1, 4, 400, pruning));
    BOOST_CHECK_EQUAL(atLimit.nFile, 1);
    BOOST_CHECK_EQUAL(atLimit.nPos, 1U);
    BOOST_CHECK_EQUAL(store.GetnLastBlockFile(), 1);
    BOOST_CHECK_EQUAL(store.GetBlockFileInfo(0)->nSize, 95U);
    BOOST_CHECK_EQUAL(store.GetBlockFileInfo(0)->nUndoSize, undoPrefix.size());
    BOOST_CHECK_EQUAL(store.GetBlockFileInfo(1)->nSize, 2U);
    BOOST_CHECK_EQUAL(fs::file_size(oldBlock), 95U);
    BOOST_CHECK_EQUAL(fs::file_size(oldUndo), undoPrefix.size());
    const auto rotatedDirty = store.GetAndClearDirtyFileInfo();
    BOOST_REQUIRE_EQUAL(rotatedDirty.size(), 1U);
    BOOST_CHECK_EQUAL(rotatedDirty.front().first, 1);
    BOOST_CHECK(!pruning);
    {
        std::unique_ptr<FILE, decltype(&std::fclose)> block(
            CDiskFiles::OpenBlockFile(CDiskBlockPos(0, 0), true), &std::fclose);
        std::unique_ptr<FILE, decltype(&std::fclose)> undo(
            CDiskFiles::OpenUndoFile(CDiskBlockPos(0, 0), true), &std::fclose);
        BOOST_REQUIRE(block.get() != nullptr);
        BOOST_REQUIRE(undo.get() != nullptr);
        std::array<char, 64> actualBlock{};
        std::array<char, 8> actualUndo{};
        BOOST_REQUIRE_EQUAL(std::fread(actualBlock.data(), 1, actualBlock.size(),
                                      block.get()), actualBlock.size());
        BOOST_REQUIRE_EQUAL(std::fread(actualUndo.data(), 1, actualUndo.size(),
                                      undo.get()), actualUndo.size());
        BOOST_CHECK_EQUAL_COLLECTIONS(blockPrefix.begin(), blockPrefix.end(),
                                      actualBlock.begin(), actualBlock.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(undoPrefix.begin(), undoPrefix.end(),
                                      actualUndo.begin(), actualUndo.end());
    }
}

BOOST_AUTO_TEST_CASE(undo_allocation_crosses_chunk_without_changing_prefix) {
    IsolatedDataDir isolation;
    CBlockTreeDB db(1 << 20, true);
    const uint64_t oldSize = UNDOFILE_CHUNK_SIZE - 8;
    CBlockFileInfo info;
    info.AddBlock(3, 100);
    info.nSize = 100;
    info.nUndoSize = oldSize;
    BOOST_REQUIRE(db.Write(std::make_pair('f', 0), info));
    CBlockFileInfoStore store;
    store.LoadBlockFileInfo(0, db);
    const std::array<char, 8> prefix{{'o', 'l', 'd', '-', 'u', 'n', 'd', 'o'}};
    {
        std::unique_ptr<FILE, decltype(&std::fclose)> file(
            CDiskFiles::OpenUndoFile(CDiskBlockPos(0, 0)), &std::fclose);
        BOOST_REQUIRE(file.get() != nullptr);
        BOOST_REQUIRE_EQUAL(std::fwrite(prefix.data(), 1, prefix.size(), file.get()),
                            prefix.size());
        BOOST_REQUIRE(SeekFileAbsolute64(file.get(), oldSize - 1));
        BOOST_REQUIRE(std::fputc(0, file.get()) != EOF);
        BOOST_REQUIRE_EQUAL(std::fflush(file.get()), 0);
    }

    constexpr uint64_t recordSize{32};
    CDiskBlockPos position;
    CValidationState state;
    bool pruning{false};
    BOOST_REQUIRE(store.FindUndoPos(state, 0, position, recordSize, pruning));
    BOOST_CHECK_EQUAL(position.nFile, 0);
    BOOST_CHECK_EQUAL(position.nPos, oldSize);
    BOOST_CHECK_EQUAL(store.GetBlockFileInfo(0)->nSize, info.nSize);
    BOOST_CHECK_EQUAL(store.GetBlockFileInfo(0)->nUndoSize, oldSize + recordSize);
    BOOST_CHECK_EQUAL(store.GetnLastBlockFile(), 0);
    BOOST_CHECK_EQUAL(pruning, fPruneMode);
    const auto dirty = store.GetAndClearDirtyFileInfo();
    BOOST_REQUIRE_EQUAL(dirty.size(), 1U);
    BOOST_CHECK_EQUAL(dirty.front().first, 0);

    // Preallocation remains advisory. Verify that an actual write at the
    // reserved position succeeds and leaves the already-used prefix intact.
    std::unique_ptr<FILE, decltype(&std::fclose)> file(
        CDiskFiles::OpenUndoFile(position), &std::fclose);
    BOOST_REQUIRE(file.get() != nullptr);
    std::array<char, recordSize> record{};
    record.fill('N');
    BOOST_REQUIRE_EQUAL(std::fwrite(record.data(), 1, record.size(), file.get()), record.size());
    BOOST_REQUIRE_EQUAL(std::fflush(file.get()), 0);
    BOOST_REQUIRE(SeekFileAbsolute64(file.get(), 0));
    std::array<char, 8> actualPrefix{};
    BOOST_REQUIRE_EQUAL(std::fread(actualPrefix.data(), 1, actualPrefix.size(), file.get()),
                        actualPrefix.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(prefix.begin(), prefix.end(),
                                  actualPrefix.begin(), actualPrefix.end());
    BOOST_REQUIRE(SeekFileAbsolute64(file.get(), oldSize));
    std::array<char, recordSize> actualRecord{};
    BOOST_REQUIRE_EQUAL(std::fread(actualRecord.data(), 1, actualRecord.size(), file.get()),
                        actualRecord.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(record.begin(), record.end(),
                                  actualRecord.begin(), actualRecord.end());
}

BOOST_AUTO_TEST_SUITE_END()
