#pragma once
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <system_error>
#include "BV.hpp" // BVStatus

// Assembles an incoming file from received chunks.
//
// Extracted from BVApp_ConsoleClient so the "every transfer starts from a clean
// file" invariant can be unit-tested without standing up the whole app (the
// console client needs a TTY to construct). Chunks are appended in arrival
// order; the file is trimmed to the advertised size on completion (chunks are
// fixed-size and the last one is zero-padded).
//
// Layout matches the rest of the app: <root>/data/<serviceName>/<fname>.
class BVReceivedFile
{
public:
    static std::filesystem::path DirFor(const std::filesystem::path& root,
                                        const std::string& serviceName)
    {
        return root / "data" / serviceName;
    }

    static std::filesystem::path PathFor(const std::filesystem::path& root,
                                         const std::string& serviceName,
                                         const std::string& fname)
    {
        return DirFor(root, serviceName) / fname;
    }

    // Prepare the destination for an incoming transfer: make sure the per-sender
    // directory exists and start the target file EMPTY.
    //
    // Truncating is the crucial bit: chunks are written in append mode, so any
    // bytes left over from a previous aborted/duplicate transfer of the same
    // name would otherwise be prepended to the new data. The closing
    // resize_file(fsize) would then trim the concatenation back to the right
    // size, yielding a correctly-sized but corrupt file. Starting clean here
    // makes the receive correct no matter what the sender does.
    static BVStatus Begin(const std::filesystem::path& root,
                          const std::string& serviceName,
                          const std::string& fname)
    {
        const std::filesystem::path dirpath  = DirFor(root, serviceName);
        const std::filesystem::path filepath = PathFor(root, serviceName, fname);
        std::error_code ec;
        std::filesystem::create_directories(dirpath, ec); // no-op if it exists
        if (ec)
        {
            return BVStatus::BVSTATUS_FATAL_ERROR;
        }
        // Open with trunc to discard any leftover file at this path.
        std::ofstream f(filepath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!f)
        {
            return BVStatus::BVSTATUS_FATAL_ERROR;
        }
        return BVStatus::BVSTATUS_OK;
    }

    // Append one received chunk's payload to the destination file.
    static BVStatus AppendChunk(const std::filesystem::path& root,
                                const std::string& serviceName,
                                const std::string& fname,
                                const std::vector<char>& data)
    {
        std::ofstream f(PathFor(root, serviceName, fname),
                        std::ios::binary | std::ios::app);
        if (!f)
        {
            return BVStatus::BVSTATUS_FATAL_ERROR;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        return f ? BVStatus::BVSTATUS_OK : BVStatus::BVSTATUS_NOK;
    }

    // Append the final chunk and trim the file to the advertised size, undoing
    // the zero-padding of the last fixed-size chunk.
    static BVStatus Finish(const std::filesystem::path& root,
                           const std::string& serviceName,
                           const std::string& fname,
                           const std::vector<char>& finalData,
                           std::uint64_t fsize)
    {
        const std::filesystem::path filepath = PathFor(root, serviceName, fname);
        {
            std::ofstream f(filepath, std::ios::binary | std::ios::app);
            if (!f)
            {
                return BVStatus::BVSTATUS_FATAL_ERROR;
            }
            f.write(finalData.data(), static_cast<std::streamsize>(finalData.size()));
        }
        std::error_code ec;
        std::filesystem::resize_file(filepath, fsize, ec);
        if (ec)
        {
            return BVStatus::BVSTATUS_NOK;
        }
        return BVStatus::BVSTATUS_OK;
    }
};
