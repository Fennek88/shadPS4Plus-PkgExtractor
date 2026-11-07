#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "app/package_extraction.h"
#include "core/file_format/pkg.h"
#include "core/loader.h"
#ifdef PKG_EXTRACTOR_WITH_QT
#include "app/extractor_window.h"
#endif
#ifdef _WIN32
#include "app/windows_update.h"
#endif
#ifdef __APPLE__
#include "app/mac_update.h"
#endif

using namespace PackageExtraction;

namespace
{
    enum ExitCode
    {
        ExitOk = 0,
        ExitFailure = 1,
        ExitUsage = 2,
    };

    bool StdinIsInteractive()
    {
#ifdef _WIN32
        return _isatty(_fileno(stdin)) != 0;
#else
        return isatty(fileno(stdin)) != 0;
#endif
    }

    bool StdoutIsInteractive()
    {
#ifdef _WIN32
        return _isatty(_fileno(stdout)) != 0;
#else
        return isatty(fileno(stdout)) != 0;
#endif
    }

    const char* TypeName(int type)
    {
        return type == 102 ? "update" : type == 103 ? "dlc" : "game";
    }

    void PrintUsage(std::ostream& out)
    {
        out << "Usage:\n"
               "  pkg_extractor                         Start the GUI\n"
               "  pkg_extractor <pkg> [output-dir]       Extract a package (default: next to the PKG)\n"
               "  pkg_extractor <pkg> --print-type      Print game, update or dlc and exit 0\n"
               "  pkg_extractor <pkg> --check-type      Legacy: exit with 101 (game), 102 (update), 103 (dlc)\n"
               "\nOptions:\n"
               "  --no-pause    Never wait for Enter before exiting\n"
               "  -h, --help    Show this help\n"
               "\nExit codes: 0 success, 1 failure, 2 invalid usage.\n"
               "Diagnostics go to stderr; stdout carries progress and results.\n";
    }
}

// Run the Qt interface without arguments; otherwise run the command-line extractor.
int main(int argc, char** argv)
{
#ifdef _WIN32
    if (argc > 1 && std::string(argv[1]) == "--windows-update-helper")
        return RunWindowsUpdateHelper(argc, argv);
#endif
#ifdef __APPLE__
    if (argc > 1 && std::string(argv[1]) == "--mac-update-helper")
        return RunMacUpdateHelper(argc, argv);
#endif
#ifdef PKG_EXTRACTOR_WITH_QT
    if (argc == 1)
    {
        return RunGui(argc, argv);
    }
#endif

    std::vector<std::string> positional;
    bool printType = false;
    bool checkType = false;
    bool noPause = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help")
        {
            PrintUsage(std::cout);
            return ExitOk;
        }
        if (argument == "--print-type")
            printType = true;
        else if (argument == "--check-type")
            checkType = true;
        else if (argument == "--no-pause")
            noPause = true;
        else if (argument.size() > 1 && argument[0] == '-' && argument[1] == '-')
        {
            std::cerr << "Unknown option: " << argument << "\n";
            PrintUsage(std::cerr);
            return ExitUsage;
        }
        else
            positional.push_back(argument);
    }

    const bool typeOnly = printType || checkType;
    if (positional.empty() || positional.size() > (typeOnly ? 1u : 2u) ||
        (printType && checkType))
    {
        PrintUsage(std::cerr);
        return ExitUsage;
    }

    const std::filesystem::path file = positional[0];
    const std::filesystem::path outputBase = positional.size() > 1 ? std::filesystem::path{positional[1]} : file.parent_path().empty() ? std::filesystem::path{"."} : file.parent_path();

    PKG pkg;
    PackageInfo info;
    std::string failreason;
    if (!PreparePackage(file, outputBase, pkg, info, failreason))
    {
        std::cerr << "Cannot open PKG file " << file << ": " << failreason << std::endl;
        return ExitFailure;
    }

    if (printType)
    {
        std::cout << TypeName(info.type) << std::endl;
        return ExitOk;
    }
    if (checkType)
    {
        return info.type;
    }

    std::cout << file << " is a valid PKG (" << TypeName(info.type) << ")" << std::endl;
    if (!CheckOutputWritable(outputBase, failreason))
    {
        std::cerr << "Cannot write to output directory " << outputBase << ": " << failreason << std::endl;
        return ExitFailure;
    }

    // Show the location relative to the game root: "/" for base games and updates,
    // "/<dlcname>" for DLC (the title id folder is omitted).
    std::string rootRelative = "/";
    if (info.type == 103)
        rootRelative += info.outputPath.filename().generic_string();
    std::cout << "Extracting pkg to " << info.outputPath << " (" << rootRelative << ")" << std::endl;
    const bool interactive = StdoutIsInteractive();
    std::string lastSubpackage;
    int lastPercent = -1;
    bool cancelled = false;
    const bool success = ExtractPackage(
        pkg, file, info, outputBase, [](int, int, int) {},
        [&](const std::string& name, int percent)
        {
            if (interactive)
            {
                std::cout << "\rExtracting subpackage " << name << ": " << percent << "%" << std::flush;
                return;
            }
            // Redirected output gets one line per 10% step instead of carriage-return updates.
            const int step = percent / 10 * 10;
            if (name != lastSubpackage || step != lastPercent)
            {
                lastSubpackage = name;
                lastPercent = step;
                std::cout << "Extracting subpackage " << name << ": " << step << "%" << std::endl;
            }
        },
        {}, nullptr, {}, false, cancelled, failreason);
    if (interactive)
        std::cout << std::endl;
    if (!success)
    {
        std::cerr << "Cannot extract PKG file: " << failreason << std::endl;
        return ExitFailure;
    }

    std::cout << "THE END " << file << std::endl;
    // Only pause for a double-click style launch; never block scripts or pipes.
    if (positional.size() == 1 && !noPause && StdinIsInteractive())
    {
        std::cout << "\nPress [enter] to exit." << std::endl;
        std::cin.get();
    }
    return ExitOk;
}
