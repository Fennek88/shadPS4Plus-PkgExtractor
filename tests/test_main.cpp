// Dependency-free unit tests for the PKG/PSF parsers.
// Optional end-to-end test: set PKG_TEST_PKG (package) and PKG_TEST_REFERENCE (expected output).
// Optional DLC regression test: set PKG_TEST_DLC_PKG (a small DLC package).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "app/package_extraction.h"
#include "core/file_format/pkg.h"
#include "core/file_format/psf.h"

namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                                          \
    } while (0)

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("pkg_extractor_test_" + std::to_string(std::rand()) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void WriteFile(const fs::path& path, const std::vector<unsigned char>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<unsigned char> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

PSF MakePsf() {
    PSF psf;
    psf.AddString("TITLE_ID", "CUSA00001");
    psf.AddString("CONTENT_ID", "UP0000-CUSA00001_00-0000000000000000");
    psf.AddInteger("APP_TYPE", 1);
    psf.AddBinary("BLOB", std::vector<u8>{1, 2, 3, 4});
    return psf;
}

bool PsfOpens(const std::vector<u8>& data) {
    PSF psf;
    return psf.Open(data);
}

void TestPsfRoundTrip() {
    const auto encoded = MakePsf().Encode();
    PSF decoded;
    CHECK(decoded.Open(encoded));
    CHECK(decoded.GetString("TITLE_ID") == std::optional<std::string_view>("CUSA00001"));
    CHECK(decoded.GetInteger("APP_TYPE") == std::optional<s32>(1));
    const auto blob = decoded.GetBinary("BLOB");
    CHECK(blob.has_value() && blob->size() >= 4 && (*blob)[0] == 1 && (*blob)[3] == 4);
    CHECK(!decoded.GetString("MISSING").has_value());
}

void TestPsfRejectsMalformed() {
    CHECK(!PsfOpens({}));
    CHECK(!PsfOpens(std::vector<u8>(8, 0)));

    const auto data = MakePsf().Encode();

    auto badMagic = data;
    badMagic[1] ^= 0xFF;
    CHECK(!PsfOpens(badMagic));

    // Truncations must never crash.
    for (size_t size = 0; size < data.size(); size += 7) {
        (void)PsfOpens(std::vector<u8>(data.begin(), data.begin() + size));
    }
    CHECK(!PsfOpens(std::vector<u8>(data.begin(), data.begin() + 0x14)));

    auto hugeCount = data;
    hugeCount[0x10] = 0xFF;
    hugeCount[0x11] = 0xFF;
    hugeCount[0x12] = 0xFF;
    hugeCount[0x13] = 0x7F;
    CHECK(!PsfOpens(hugeCount));

    auto badOffset = data;
    badOffset[0x08] = 0xFF;
    badOffset[0x09] = 0xFF;
    badOffset[0x0A] = 0xFF;
    badOffset[0x0B] = 0x7F;
    CHECK(!PsfOpens(badOffset));
}

void TestPkgRejectsMalformed() {
    TempDir dir;
    std::string reason;

    PKG missing;
    CHECK(!missing.Open(dir.path / "does_not_exist.pkg", reason));
    CHECK(!reason.empty());

    const auto check = [&](const char* name, std::vector<unsigned char> content) {
        const auto path = dir.path / name;
        WriteFile(path, content);
        PKG pkg;
        std::string failure;
        CHECK(!pkg.Open(path, failure));
        CHECK(!failure.empty());
    };

    check("empty.pkg", {});
    check("truncated.pkg", {0x7F, 'C', 'N', 'T', 0, 0, 0, 1});
    check("bad_magic.pkg", std::vector<unsigned char>(sizeof(PKGHeader) + 64, 0xAB));

    // Valid magic with an otherwise zeroed header must fail validation, not crash.
    std::vector<unsigned char> zeroed(sizeof(PKGHeader) + 64, 0);
    zeroed[0] = 0x7F;
    zeroed[1] = 'C';
    zeroed[2] = 'N';
    zeroed[3] = 'T';
    check("zeroed.pkg", zeroed);

    PKG extractMissing;
    reason.clear();
    CHECK(!extractMissing.Extract(dir.path / "nope.pkg", dir.path / "out", reason));
}

void WriteText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void TestMergeIntoNewDestination() {
    TempDir dir;
    const auto source = dir.path / "staging" / "GAME";
    WriteText(source / "eboot.bin", "boot");
    WriteText(source / "sce_sys" / "param.sfo", "sfo");

    std::string reason;
    const auto destination = dir.path / "out" / "GAME";
    CHECK(PackageExtraction::MergeStagedTree(source, destination, {}, reason));
    CHECK(ReadText(destination / "eboot.bin") == "boot");
    CHECK(ReadText(destination / "sce_sys" / "param.sfo") == "sfo");
    CHECK(!fs::exists(source));
}

void TestMergeIntoExistingDestination() {
    TempDir dir;
    const auto source = dir.path / "staging" / "GAME";
    WriteText(source / "eboot.bin", "new");
    WriteText(source / "data" / "a.bin", "newdata");
    const auto destination = dir.path / "out" / "GAME";
    WriteText(destination / "eboot.bin", "old");
    WriteText(destination / "keep.txt", "keep");

    std::vector<int> progress;
    std::string reason;
    CHECK(PackageExtraction::MergeStagedTree(
        source, destination, [&](int percent) { progress.push_back(percent); }, reason));
    CHECK(ReadText(destination / "eboot.bin") == "new");
    CHECK(ReadText(destination / "data" / "a.bin") == "newdata");
    CHECK(ReadText(destination / "keep.txt") == "keep");
    CHECK(!progress.empty() && progress.front() == 0 && progress.back() == 100);
    for (size_t i = 1; i < progress.size(); ++i)
        CHECK(progress[i] >= progress[i - 1]);
}

void TestMergeMissingSourceFails() {
    TempDir dir;
    std::string reason;
    CHECK(!PackageExtraction::MergeStagedTree(dir.path / "missing", dir.path / "dest", {}, reason));
    CHECK(!reason.empty());
}

void TestCheckOutputWritable() {
    TempDir dir;
    std::string reason;
    CHECK(PackageExtraction::CheckOutputWritable(dir.path / "new" / "sub", reason));
    CHECK(reason.empty());
    // No probe files may be left behind.
    CHECK(fs::is_empty(dir.path / "new" / "sub"));

    const auto blocker = dir.path / "file.txt";
    WriteText(blocker, "x");
    reason.clear();
    CHECK(!PackageExtraction::CheckOutputWritable(blocker / "child", reason));
    CHECK(!reason.empty());
}

void TestPrepareRejectsNonPkg() {
    TempDir dir;
    const auto file = dir.path / "not.pkg";
    WriteText(file, "this is not a package");
    PKG pkg;
    PackageExtraction::PackageInfo info;
    std::string reason;
    CHECK(!PackageExtraction::PreparePackage(file, dir.path, pkg, info, reason));
    CHECK(!reason.empty());
}

std::map<std::string, std::uintmax_t> ListFiles(const fs::path& root) {
    std::map<std::string, std::uintmax_t> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files[entry.path().lexically_relative(root).generic_string()] = entry.file_size();
        }
    }
    return files;
}

void TestRealPackage() {
    const char* pkgPath = std::getenv("PKG_TEST_PKG");
    const char* refPath = std::getenv("PKG_TEST_REFERENCE");
    if (!pkgPath || !refPath) {
        std::printf("  skipped (set PKG_TEST_PKG and PKG_TEST_REFERENCE to enable)\n");
        return;
    }

    TempDir out;
    std::string reason;
    PKG pkg;
    CHECK(pkg.Open(pkgPath, reason));
    if (!reason.empty())
        std::fprintf(stderr, "  open: %s\n", reason.c_str());
    reason.clear();
    // The extractor derives its PFS root from the output name (<TITLEID> or <TITLEID>-patch),
    // so reuse the reference directory's name.
    const fs::path outputPath = out.path / fs::path(refPath).filename();
    CHECK(pkg.Extract(pkgPath, outputPath, reason));
    if (!reason.empty())
        std::fprintf(stderr, "  extract: %s\n", reason.c_str());
    for (u32 i = 0; i < pkg.GetNumberOfFiles(); ++i) {
        reason.clear();
        const bool ok = pkg.ExtractFiles(static_cast<int>(i), reason);
        CHECK(ok);
        if (!ok)
            std::fprintf(stderr, "  file %u: %s\n", i, reason.c_str());
    }

    const fs::path ref = refPath;
    const auto expected = ListFiles(ref);
    const auto actual = ListFiles(outputPath);
    CHECK(expected.size() == actual.size());
    for (const auto& [name, size] : expected) {
        const auto it = actual.find(name);
        CHECK(it != actual.end());
        if (it == actual.end()) {
            std::fprintf(stderr, "  missing: %s\n", name.c_str());
            continue;
        }
        CHECK(it->second == size);
        // npbind.dat is known to differ in its trailing bytes from the reference extraction.
        if (name != "sce_sys/npbind.dat" && size <= (64u << 20)) {
            CHECK(ReadFile(ref / name) == ReadFile(outputPath / name));
        }
    }
}

// Regression: small DLC packages declare a PFS cache larger than their PFS image.
void TestDlcPackage() {
    const char* pkgPath = std::getenv("PKG_TEST_DLC_PKG");
    if (!pkgPath) {
        std::printf("  skipped (set PKG_TEST_DLC_PKG to enable)\n");
        return;
    }
    TempDir out;
    // The PFS root is derived from the layout <TITLEID>/<dlcname>, so the title id must be the parent.
    const std::string titleId = fs::path(pkgPath).filename().string().substr(0, 9);
    const fs::path dlcDir = out.path / titleId / "DLC";
    std::string reason;
    PKG pkg;
    CHECK(pkg.Open(pkgPath, reason));
    reason.clear();
    CHECK(pkg.Extract(pkgPath, dlcDir, reason));
    if (!reason.empty())
        std::fprintf(stderr, "  extract: %s\n", reason.c_str());
    for (u32 i = 0; i < pkg.GetNumberOfFiles(); ++i) {
        reason.clear();
        CHECK(pkg.ExtractFiles(static_cast<int>(i), reason));
    }
    CHECK(fs::exists(dlcDir / "sce_sys" / "param.sfo"));
    CHECK(!ListFiles(dlcDir).empty());
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"PSF round trip", TestPsfRoundTrip},
        {"PSF rejects malformed input", TestPsfRejectsMalformed},
        {"PKG rejects malformed input", TestPkgRejectsMalformed},
        {"Merge into new destination", TestMergeIntoNewDestination},
        {"Merge into existing destination", TestMergeIntoExistingDestination},
        {"Merge with missing source fails", TestMergeMissingSourceFails},
        {"Output writability check", TestCheckOutputWritable},
        {"Prepare rejects non-PKG", TestPrepareRejectsNonPkg},
        {"Real package extraction", TestRealPackage},
        {"DLC package extraction", TestDlcPackage},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", name);
        fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
