#include "app/package_extraction.h"

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#include "common/string_util.h"
#include "core/file_format/pkg.h"
#include "core/file_format/psf.h"
#include "core/loader.h"

namespace
{
    struct StagedFile
    {
        std::filesystem::path source;
        std::filesystem::path relativePath;
        u64 size;
    };

    bool CopyStagedFile(const StagedFile& file,
                        const std::filesystem::path& destination,
                        u64& copiedBytes,
                        u64 totalBytes,
                        const std::function<void(int)>& onCopyProgress,
                        std::error_code& error)
    {
        std::ifstream input(file.source, std::ios::binary);
        if (!input)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        std::array<char, 1024 * 1024> buffer{};
        u64 fileBytesCopied = 0;
        int lastReportedPercent = totalBytes == 0
                                      ? 100
                                      : static_cast<int>(static_cast<long double>(copiedBytes) * 100 / totalBytes);
        while (input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytesRead = input.gcount();
            if (bytesRead > 0)
            {
                const auto chunkSize = static_cast<u64>(bytesRead);
                if (chunkSize > file.size - fileBytesCopied || chunkSize > totalBytes - copiedBytes)
                {
                    error = std::make_error_code(std::errc::file_too_large);
                    return false;
                }
                output.write(buffer.data(), bytesRead);
                if (!output)
                {
                    error = std::make_error_code(std::errc::io_error);
                    return false;
                }

                fileBytesCopied += chunkSize;
                copiedBytes += chunkSize;
                const int percent = static_cast<int>(static_cast<long double>(copiedBytes) * 100 / totalBytes);
                if (onCopyProgress && percent != lastReportedPercent)
                {
                    onCopyProgress(percent);
                    lastReportedPercent = percent;
                }
            }
        }
        if (input.bad() || fileBytesCopied != file.size)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        output.flush();
        if (!output)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        output.close();
        if (!output)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        const auto sourceStatus = std::filesystem::status(file.source, error);
        if (error)
            return false;
        std::filesystem::permissions(destination, sourceStatus.permissions(),
                                     std::filesystem::perm_options::replace, error);
        return !error;
    }
}

namespace PackageExtraction
{
    // Read the package metadata and choose the destination layout for a game, update, or DLC.
    bool PreparePackage(const std::filesystem::path& file,
                        const std::filesystem::path& outputBase,
                        PKG& pkg,
                        PackageInfo& info,
                        std::string& failreason)
    {
        if (Loader::DetectFileType(file) != Loader::FileTypes::Pkg)
        {
            failreason = "The selected file is not a valid PKG file.";
            return false;
        }

        if (!pkg.Open(file, failreason))
        {
            if (failreason.empty())
            {
                failreason = "Could not open the package.";
            }
            return false;
        }

        PSF psf;
        if (!psf.Open(pkg.sfo))
        {
            failreason = "Could not read the package SFO.";
            return false;
        }

        const auto category = psf.GetString("CATEGORY");
        if (!category.has_value())
        {
            failreason = "The package SFO does not contain a CATEGORY entry.";
            return false;
        }

        info.outputPath = outputBase / std::string{pkg.GetTitleID()};
        const std::string pkgType = pkg.GetPkgFlags();
        if (pkgType.contains("PATCH"))
        {
            info.type = 102;
            info.outputPath += "-patch";
        }
        else if (*category == "ac")
        {
            info.type = 103;
            const auto contentId = psf.GetString("CONTENT_ID");
            if (!contentId.has_value())
            {
                failreason = "The package SFO does not contain a CONTENT_ID entry.";
                return false;
            }
            const auto parts = Common::SplitString(std::string{*contentId}, '-');
            if (parts.size() <= 2 || parts[2].empty() || parts[2] == "." || parts[2] == ".." ||
                parts[2].find_first_of("/\\:") != std::string::npos ||
                parts[2].find('\0') != std::string::npos)
            {
                failreason = "The package CONTENT_ID is invalid.";
                return false;
            }
            info.outputPath /= parts[2];
        }
        else
        {
            info.type = 101;
        }

        return true;
    }

    // Create and write a temporary file under the output base, then remove only the probe created here.
    bool CheckOutputWritable(const std::filesystem::path& outputBase, std::string& failreason)
    {
        const auto directory = outputBase.empty() ? std::filesystem::path{"."} : outputBase;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            failreason = error.message();
            return false;
        }

        static std::atomic_uint64_t nextId{0};
        std::filesystem::path probeDirectory;
        bool created = false;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto id = nextId.fetch_add(1, std::memory_order_relaxed);
            probeDirectory =
                directory /
                (".pkg-extractor-write-test-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(id));
            if (std::filesystem::create_directory(probeDirectory, error))
            {
                created = true;
                break;
            }
            if (error)
            {
                failreason = error.message();
                return false;
            }
        }
        if (!created)
        {
            failreason = "Could not create a temporary write-test directory.";
            return false;
        }

        {
            std::ofstream testFile(probeDirectory / "write-test", std::ios::binary);
            if (!testFile)
            {
                failreason = "Could not create a temporary file in the output directory.";
            }
            else
            {
                testFile.put('x');
                testFile.flush();
                if (!testFile)
                {
                    failreason = "Could not write to the output directory.";
                }
            }
        }

        std::filesystem::remove_all(probeDirectory, error);
        if (error)
        {
            failreason = error.message();
            return false;
        }
        return failreason.empty();
    }

    // Move the staged tree into place, falling back to copying across filesystems or into existing folders.
    bool MergeStagedTree(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         const std::function<void(int)>& onCopyProgress,
                         std::string& failreason)
    {
        std::error_code error;

        // Fast path: the staging tree lives on the destination filesystem, so a rename is instant.
        if (!std::filesystem::exists(destination, error) && !error)
        {
            std::filesystem::create_directories(destination.parent_path(), error);
            if (!error)
            {
                std::filesystem::rename(source, destination, error);
            }
            if (!error)
            {
                return true;
            }
        }
        error.clear();

        std::vector<std::filesystem::path> directories;
        std::vector<StagedFile> files;
        u64 totalCopyBytes = 0;
        for (std::filesystem::recursive_directory_iterator it(source, error), end;
             !error && it != end; it.increment(error))
        {
            const auto relativePath = it->path().lexically_relative(source);
            if (it->is_directory(error))
            {
                directories.push_back(relativePath);
            }
            else if (!error && it->is_regular_file(error))
            {
                const u64 fileSize = it->file_size(error);
                if (!error)
                {
                    if (fileSize > std::numeric_limits<u64>::max() - totalCopyBytes)
                    {
                        error = std::make_error_code(std::errc::file_too_large);
                        break;
                    }
                    totalCopyBytes += fileSize;
                    files.push_back({it->path(), relativePath, fileSize});
                }
            }
            if (error)
                break;
        }
        if (error)
        {
            failreason = error.message();
            return false;
        }

        if (onCopyProgress)
            onCopyProgress(0);
        for (const auto& relativePath : directories)
        {
            std::filesystem::create_directories(destination / relativePath, error);
            if (error)
                break;
        }

        u64 copiedBytes = 0;
        for (const auto& stagedFile : files)
        {
            if (error)
                break;
            const auto target = destination / stagedFile.relativePath;
            std::filesystem::create_directories(target.parent_path(), error);
            if (!error)
            {
                std::error_code renameError;
                std::filesystem::rename(stagedFile.source, target, renameError);
                if (!renameError)
                {
                    copiedBytes += stagedFile.size;
                }
                else
                {
                    CopyStagedFile(stagedFile, target, copiedBytes, totalCopyBytes,
                                   onCopyProgress, error);
                }
            }
        }
        if (error)
        {
            failreason = error.message();
            return false;
        }
        if (onCopyProgress)
            onCopyProgress(100);
        return true;
    }

    // Extract into a private staging tree for GUI runs, then merge only after all files succeed.
    // CLI runs retain their direct-to-output behavior because they do not enable staging.
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
                        std::string& failreason)
    {
        std::filesystem::path stagingDirectory;
        std::filesystem::path extractionPath = info.outputPath;
        if (stageOutput)
        {
            // Create a unique child directory so concurrent or repeated runs never share partial files.
            std::error_code error;
            std::filesystem::create_directories(outputBase, error);
            if (error)
            {
                failreason = error.message();
                return false;
            }

            static std::atomic_uint64_t nextId{0};
            bool created = false;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                const auto id = nextId.fetch_add(1, std::memory_order_relaxed);
                // Exclusive creation gives each run a private tree, even when final output paths match.
                stagingDirectory =
                    outputBase /
                    (".pkg-extractor-staging-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                        std::to_string(id));
                if (std::filesystem::create_directory(stagingDirectory, error))
                {
                    created = true;
                    break;
                }
                if (error)
                {
                    failreason = error.message();
                    return false;
                }
            }
            if (!created)
            {
                failreason = "Could not create an isolated staging directory.";
                return false;
            }

            const auto relativeOutput = info.outputPath.lexically_relative(outputBase);
            if (relativeOutput.empty() || relativeOutput.is_absolute() ||
                *relativeOutput.begin() == "..")
            {
                std::filesystem::remove_all(stagingDirectory, error);
                failreason = "The output path is not inside the selected output directory.";
                return false;
            }
            extractionPath = stagingDirectory / relativeOutput;
        }

        // PKG::Extract prepares package metadata and the file table before individual payloads are read.
        if (!pkg.Extract(file, extractionPath, failreason))
        {
            if (failreason.empty())
            {
                failreason = "Failed to prepare the package for extraction.";
            }
            if (stageOutput)
            {
                std::error_code ignored;
                std::filesystem::remove_all(stagingDirectory, ignored);
            }
            return false;
        }

        // Check cancellation between sectors so progress remains responsive for large files.
        const int fileCount = static_cast<int>(pkg.GetNumberOfFiles());
        int lastReportedPercent = -1;
        const auto reportProgress = [&](int current, int percent)
        {
            if (onProgress && percent != lastReportedPercent)
            {
                onProgress(current, fileCount, percent);
                lastReportedPercent = percent;
            }
        };
        reportProgress(0, 0);
        for (int i = 0; i < fileCount; ++i)
        {
            if (cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed))
            {
                cancelled = true;
                std::error_code error;
                std::filesystem::remove_all(stagingDirectory, error);
                if (error)
                {
                    cancelled = false;
                    failreason = error.message();
                }
                return false;
            }
            if (!pkg.ExtractFiles(i, failreason, cancelRequested, [&](const std::string& name, int completedSectors, int totalSectors)
                {
                    const double fileProgress =
                        totalSectors == 0 ? 1.0
                                          : static_cast<double>(completedSectors) / totalSectors;
                    const int percent = fileCount == 0
                                            ? 100
                                            : static_cast<int>((i + fileProgress) * 100 / fileCount);
                    reportProgress(i + 1, percent);
                    if (onSubpackageProgress)
                    {
                        onSubpackageProgress(
                            name, totalSectors == 0 ? 100 : completedSectors * 100 / totalSectors);
                    }
                }))
            {
                cancelled = failreason.empty();
                std::error_code error;
                if (stageOutput)
                {
                    std::filesystem::remove_all(stagingDirectory, error);
                }
                if (error)
                {
                    cancelled = false;
                    failreason = error.message();
                }
                return false;
            }
            reportProgress(i + 1, (i + 1) * 100 / fileCount);
        }

        if (cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed))
        {
            cancelled = true;
            std::error_code error;
            std::filesystem::remove_all(stagingDirectory, error);
            if (error)
            {
                cancelled = false;
                failreason = error.message();
            }
            return false;
        }

        reportProgress(fileCount, 100);

        if (stageOutput)
        {
            if (onFinalizing)
            {
                onFinalizing();
            }
            // Once merging begins, cancellation is disabled because output may already be replacing files.
            if (cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed))
            {
                cancelled = true;
                std::error_code error;
                std::filesystem::remove_all(stagingDirectory, error);
                if (error)
                {
                    cancelled = false;
                    failreason = error.message();
                }
                return false;
            }

            if (!MergeStagedTree(extractionPath, info.outputPath, onCopyProgress, failreason))
            {
                std::error_code ignored;
                std::filesystem::remove_all(stagingDirectory, ignored);
                return false;
            }

            std::error_code error;
            std::filesystem::remove_all(stagingDirectory, error);
            if (error)
            {
                failreason = error.message();
                return false;
            }
        }
        return true;
    }
}
