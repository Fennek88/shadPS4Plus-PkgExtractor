#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

class PKG;

namespace PackageExtraction
{
    struct PackageInfo
    {
        int type = 0; // 101 = base game, 102 = update, 103 = DLC
        std::filesystem::path outputPath;
    };

    // Read the package metadata and choose the destination layout for a game, update, or DLC.
    bool PreparePackage(const std::filesystem::path& file,
                        const std::filesystem::path& outputBase,
                        PKG& pkg,
                        PackageInfo& info,
                        std::string& failreason);

    // Verify that files can be created and written below outputBase.
    bool CheckOutputWritable(const std::filesystem::path& outputBase, std::string& failreason);

    // Move a staged tree to its final location: rename when the destination does not exist,
    // otherwise merge file by file, copying with progress when a rename is not possible.
    bool MergeStagedTree(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         const std::function<void(int)>& onCopyProgress,
                         std::string& failreason);

    // Extract into a private staging tree for GUI runs (stageOutput), then merge after all files succeed.
    bool ExtractPackage(PKG& pkg,
                        const std::filesystem::path& file,
                        const PackageInfo& info,
                        const std::filesystem::path& outputBase,
                        const std::function<void(int, int, int)>& onProgress,
                        const std::function<void(const std::string&, int)>& onSubpackageProgress,
                        const std::function<void()>& onFinalizing,
                        const std::function<void(int)>& onCopyProgress,
                        const std::atomic_bool* cancelRequested,
                        bool stageOutput,
                        bool& cancelled,
                        std::string& failreason);
}
