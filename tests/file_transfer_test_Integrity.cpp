#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <numeric>
#include "BVReceivedFile.hpp"

/*
 * file_transfer_test_Integrity
 *
 * Reproduces the file-corruption bug seen in the field: a received file came
 * out the right SIZE but the wrong CONTENT, made of several copies of the file
 * each restarting from byte 0 (verified with cmp/xxd on a 1 GB transfer).
 *
 * Root cause: the receive side writes chunks to <root>/data/<sender>/<name> in
 * append mode and never truncates a pre-existing file. A second transfer of the
 * same name (a retry after a partial/aborted transfer, or a second send) stacks
 * its bytes on top of the leftovers; the closing resize_file(fsize) then trims
 * the concatenation back to the advertised size, hiding the size mismatch while
 * leaving the content scrambled.
 *
 * The invariant under test: receiving a file always yields exactly the bytes
 * that were sent, regardless of what (if anything) was left in the destination.
 */

namespace
{
    // A deterministic, non-repeating-ish payload so any misplacement shows up.
    std::vector<char> MakePayload(std::size_t n, unsigned seed)
    {
        std::vector<char> v(n);
        unsigned x = seed * 2654435761u + 1u;
        for (std::size_t i = 0; i < n; ++i)
        {
            x = x * 1103515245u + 12345u;
            v[i] = static_cast<char>((x >> 16) & 0xFF);
        }
        return v;
    }

    std::vector<char> ReadAll(const std::filesystem::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
    }

    // Drive one whole transfer through BVReceivedFile the way the session does:
    // fixed-size chunks, last one zero-padded, then a size-trimming Finish.
    void ReceiveWholeFile(const std::filesystem::path& root,
                          const std::string& sender,
                          const std::string& name,
                          const std::vector<char>& content,
                          std::size_t csize)
    {
        ASSERT_EQ(BVReceivedFile::Begin(root, sender, name), BVStatus::BVSTATUS_OK);
        std::size_t off = 0;
        while (off < content.size())
        {
            const std::size_t remaining = content.size() - off;
            std::vector<char> chunk(csize, 0); // fixed-size, zero-padded
            const std::size_t take = std::min(csize, remaining);
            std::copy(content.begin() + off, content.begin() + off + take, chunk.begin());
            off += take;
            if (off >= content.size())
            {
                ASSERT_EQ(BVReceivedFile::Finish(root, sender, name, chunk, content.size()),
                          BVStatus::BVSTATUS_OK);
            }
            else
            {
                ASSERT_EQ(BVReceivedFile::AppendChunk(root, sender, name, chunk),
                          BVStatus::BVSTATUS_OK);
            }
        }
    }

    struct TmpDir
    {
        std::filesystem::path path;
        TmpDir()
        {
            path = std::filesystem::temp_directory_path() /
                   ("localchat_ft_test_" + std::to_string(::testing::UnitTest::GetInstance()
                        ->current_test_info()->line()));
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path, ec);
        }
        ~TmpDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };
}

// Baseline: a plain transfer into an empty destination reconstructs the file.
TEST(FileTransferIntegrity, CleanTransferReconstructsExactBytes)
{
    TmpDir tmp;
    const std::string sender = "mmmMac";
    const std::string name   = "clip.bin";
    const std::vector<char> content = MakePayload(1000, 7);
    ReceiveWholeFile(tmp.path, sender, name, content, /*csize*/ 256);

    const auto got = ReadAll(BVReceivedFile::PathFor(tmp.path, sender, name));
    EXPECT_EQ(got, content);
}

// The regression: a leftover partial file from an aborted earlier transfer of
// the SAME name must not survive into the next transfer. With the bug, the new
// chunks are appended after the stale bytes and the result is corrupt despite
// having the correct size.
TEST(FileTransferIntegrity, OverwritesLeftoverFromAbortedTransfer)
{
    TmpDir tmp;
    const std::string sender = "mmmMac";
    const std::string name   = "clip.bin";

    // Simulate the wreckage of an earlier transfer that died part-way: the
    // per-sender directory exists and holds a partial file of the same name.
    std::error_code ec;
    std::filesystem::create_directories(BVReceivedFile::DirFor(tmp.path, sender), ec);
    {
        const std::vector<char> stale = MakePayload(640, 99); // != real content
        std::ofstream f(BVReceivedFile::PathFor(tmp.path, sender, name), std::ios::binary);
        f.write(stale.data(), static_cast<std::streamsize>(stale.size()));
    }

    // Now receive the file for real.
    const std::vector<char> content = MakePayload(1000, 7);
    ReceiveWholeFile(tmp.path, sender, name, content, /*csize*/ 256);

    const auto got = ReadAll(BVReceivedFile::PathFor(tmp.path, sender, name));
    EXPECT_EQ(got.size(), content.size());      // size always looked fine
    EXPECT_EQ(got, content) << "received file is corrupt: leftover bytes from a "
                               "previous transfer were prepended instead of overwritten";
}
