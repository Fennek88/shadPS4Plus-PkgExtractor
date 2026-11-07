// Dependency-free unit tests for the PKG/PSF parsers.
// Optional end-to-end test: set PKG_TEST_PKG (package) and PKG_TEST_REFERENCE (expected output).
// Optional DLC regression test: set PKG_TEST_DLC_PKG (a small DLC package).
// Optional Windows update test: set PKG_TEST_WINDOWS_ZIP (CI-format release ZIP).

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
#include "app/linux_update.h"
#include "app/release_info.h"
#include "app/update_settings.h"
#include "core/file_format/pkg.h"
#include "core/file_format/psf.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QCoreApplication>
#include <QSysInfo>
#ifdef Q_OS_MACOS
#include "app/mac_update.h"
#include "app/mac_update_zip.h"
#include <archive.h>
#include <archive_entry.h>
#include <QProcess>
#endif
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
#define PKG_TEST_ARCHIVE 1
#include <archive.h>
#include <archive_entry.h>
#include "app/windows_update_archive.h"
#endif

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

void TestReleaseInfo() {
    ReleaseInfo release;
    QString error;
    CHECK(ParseReleaseInfo(R"({"tag_name":"Pkg-Extractor-v0.9.1","draft":false,"prerelease":false})",
                           release, error));
    CHECK(release.version == QVersionNumber(0, 9, 1));
    CHECK(release.url.toString() ==
          QStringLiteral("https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/tag/Pkg-Extractor-v0.9.1"));
    CHECK(QVersionNumber::compare(release.version.normalized(), QVersionNumber(0, 9, 0).normalized()) > 0);
    CHECK(QVersionNumber::compare(release.version.normalized(), QVersionNumber(1, 0, 0).normalized()) < 0);
    CHECK(ParseReleaseInfo(R"({"tag_name":"0.9.2","draft":false,"prerelease":false})", release, error));
    CHECK(release.tag == QStringLiteral("0.9.2"));
    CHECK(release.version == QVersionNumber(0, 9, 2));
    CHECK(release.url.toString() ==
          QStringLiteral("https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/tag/0.9.2"));
    CHECK(QVersionNumber::compare(release.version.normalized(), QVersionNumber(0, 9, 1).normalized()) > 0);
    CHECK(ParseReleaseInfo(R"({"tag_name":"v0.9","draft":false,"prerelease":false})", release, error));
    CHECK(QVersionNumber::compare(release.version.normalized(), QVersionNumber(0, 9, 0).normalized()) == 0);
    CHECK(!ParseReleaseInfo("not json", release, error));
    CHECK(!ParseReleaseInfo(R"({"tag_name":"v0.9.1"})", release, error));
    CHECK(!ParseReleaseInfo(R"({"tag_name":"v0.9.1","draft":true,"prerelease":false})", release, error));
    CHECK(!ParseReleaseInfo(R"({"tag_name":"v0.9.1","draft":false,"prerelease":true})", release, error));
    CHECK(!ParseReleaseInfo(R"({"tag_name":"v0.9.1-beta","draft":false,"prerelease":false})", release, error));
    CHECK(!ParseReleaseInfo(R"({"tag_name":"v9999999999999999999.1","draft":false,"prerelease":false})",
                            release, error));
    CHECK(!ParseReleaseInfo(R"({"tag_name":"v0.9.1/../../other","draft":false,"prerelease":false})",
                            release, error));
    CHECK(!error.isEmpty());
}

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

void TestUpdateSettings() {
    TempDir dir;
    const QString path = QString::fromStdString((dir.path / "settings" / "update.ini").string());
    QVersionNumber skipped;
    QString error;
    CHECK(ReadSkippedVersion(path, skipped, error));
    CHECK(skipped.isNull());
    CHECK(WriteSkippedVersion(path, QVersionNumber(1, 0, 0), error));
    CHECK(ReadSkippedVersion(path, skipped, error));
    CHECK(QVersionNumber::compare(skipped.normalized(), QVersionNumber(1, 0).normalized()) == 0);
    CHECK(QVersionNumber::compare(skipped.normalized(), QVersionNumber(1, 0, 1).normalized()) != 0);
    CHECK(!ShouldNotifyUpdate(QVersionNumber(0, 9, 0), QVersionNumber(1, 0), skipped, true));
    CHECK(ShouldNotifyUpdate(QVersionNumber(0, 9, 0), QVersionNumber(1, 0), skipped, false));
    CHECK(ShouldNotifyUpdate(QVersionNumber(0, 9, 0), QVersionNumber(1, 0, 1), skipped, true));
    CHECK(!ShouldNotifyUpdate(QVersionNumber(1, 0, 0), QVersionNumber(1, 0), {}, false));
    CHECK(!ShouldNotifyUpdate(QVersionNumber(2, 0), QVersionNumber(1, 0), {}, true));
    CHECK(WriteSkippedVersion(path, QVersionNumber(0, 9, 1), error));
    CHECK(ReadSkippedVersion(path, skipped, error));
    CHECK(skipped == QVersionNumber(0, 9, 1));
    CHECK(ClearMatchingSkippedVersion(path, QVersionNumber(0, 9, 2), error));
    CHECK(ReadSkippedVersion(path, skipped, error));
    CHECK(skipped == QVersionNumber(0, 9, 1));
    CHECK(ClearMatchingSkippedVersion(path, QVersionNumber({0, 9, 1, 0}), error));
    CHECK(ReadSkippedVersion(path, skipped, error));
    CHECK(skipped.isNull());
    CHECK(ClearMatchingSkippedVersion(path, QVersionNumber(0, 9, 1), error));
    CHECK(!ClearMatchingSkippedVersion({}, QVersionNumber(0, 9, 1), error));
    CHECK(!error.isEmpty());
    CHECK(WriteSkippedVersion(path, {}, error));
    CHECK(ReadSkippedVersion(path, skipped, error));
    CHECK(skipped.isNull());
    CHECK(!ReadSkippedVersion({}, skipped, error));
    CHECK(!error.isEmpty());
    CHECK(!WriteSkippedVersion({}, QVersionNumber(1, 0), error));
    CHECK(!error.isEmpty());
    WriteFile(dir.path / "bad.ini", {'[', 'u', 'p', 'd', 'a', 't', 'e', 's', ']', '\n',
                                       's', 'k', 'i', 'p', 'p', 'e', 'd', 'V', 'e', 'r', 's', 'i', 'o', 'n',
                                       '=', 'b', 'a', 'd', '\n'});
    CHECK(!ReadSkippedVersion(QString::fromStdString((dir.path / "bad.ini").string()), skipped, error));
    CHECK(!error.isEmpty());
    CHECK(!ClearMatchingSkippedVersion(QString::fromStdString((dir.path / "bad.ini").string()),
                                       QVersionNumber(0, 9, 1), error));
    CHECK(!error.isEmpty());
    const QString notDir = QString::fromStdString((dir.path / "bad.ini" / "update.ini").string());
    CHECK(!WriteSkippedVersion(notDir, QVersionNumber(1, 0), error));
}

#if defined(Q_OS_LINUX)
QByteArray MakeUpdateZip(const std::vector<std::pair<QByteArray, QByteArray>>& files) {
    QByteArray buffer(1024 * 1024, '\0');
    size_t written = 0;
    archive* writer = archive_write_new();
    if (!writer || archive_write_set_format_zip(writer) != ARCHIVE_OK ||
        archive_write_open_memory(writer, buffer.data(), static_cast<size_t>(buffer.size()), &written) != ARCHIVE_OK)
        std::abort();
    for (const auto& [name, content] : files) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0755);
        archive_entry_set_size(entry, content.size());
        if (archive_write_header(writer, entry) != ARCHIVE_OK ||
            archive_write_data(writer, content.constData(), static_cast<size_t>(content.size())) != content.size())
            std::abort();
        archive_entry_free(entry);
    }
    if (archive_write_close(writer) != ARCHIVE_OK)
        std::abort();
    archive_write_free(writer);
    buffer.resize(static_cast<qsizetype>(written));
    return buffer;
}
#endif

void TestLinuxUpdate() {
    ReleaseInfo release;
    QString error;
    CHECK(ParseReleaseInfo(R"({"tag_name":"0.9.2","draft":false,"prerelease":false})", release, error));
    QByteArray elf(64, '\0');
    elf.replace(0, 6, QByteArray("\x7f" "ELF\x02\x01", 6));
    elf[18] = 0x3e;
    const QByteArray hash = QCryptographicHash::hash(elf, QCryptographicHash::Sha256);
    QJsonObject entry{
        {QStringLiteral("name"), QStringLiteral("pkg-extractor-linux-0.9.2")},
        {QStringLiteral("state"), QStringLiteral("uploaded")},
        {QStringLiteral("size"), elf.size()},
        {QStringLiteral("digest"), QStringLiteral("sha256:") + QString::fromLatin1(hash.toHex())},
        {QStringLiteral("browser_download_url"), QStringLiteral(
            "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/0.9.2/pkg-extractor-linux-0.9.2")}
    };
    auto response = [](const QJsonArray& assets) {
        return QJsonDocument(QJsonObject{{QStringLiteral("assets"), assets}}).toJson();
    };
    LinuxReleaseAsset asset;
    CHECK(FindLinuxReleaseAsset(response(QJsonArray{entry}), release, asset, error));
    CHECK(asset.sha256 == hash && asset.size == elf.size());
    CHECK(!FindLinuxReleaseAsset(response(QJsonArray{entry, entry}), release, asset, error));
    QJsonObject bad = entry;
    bad.insert(QStringLiteral("browser_download_url"), QStringLiteral("https://example.com/update"));
    CHECK(!FindLinuxReleaseAsset(response(QJsonArray{bad}), release, asset, error));
    bad = entry;
    bad.insert(QStringLiteral("digest"), QStringLiteral("sha256:bad"));
    CHECK(!FindLinuxReleaseAsset(response(QJsonArray{bad}), release, asset, error));

#ifdef Q_OS_LINUX
    const QByteArray zip = MakeUpdateZip({{"readme.txt", "readme"}, {"pkg_extractor", elf}});
    QJsonObject zipEntry = entry;
    zipEntry.insert(QStringLiteral("name"), QStringLiteral("pkg-extractor-linux-0.9.2.zip"));
    zipEntry.insert(QStringLiteral("browser_download_url"), entry.value(QStringLiteral("browser_download_url")).toString() + QStringLiteral(".zip"));
    zipEntry.insert(QStringLiteral("size"), zip.size());
    zipEntry.insert(QStringLiteral("digest"), QStringLiteral("sha256:") + QString::fromLatin1(
        QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex()));
    LinuxReleaseAsset zipAsset;
    CHECK(FindLinuxReleaseAsset(response(QJsonArray{zipEntry}), release, zipAsset, error));
    CHECK(zipAsset.format == LinuxReleaseAsset::Format::Zip && zipAsset.size == zip.size());
    LinuxReleaseAsset preferred;
    CHECK(FindLinuxReleaseAsset(response(QJsonArray{zipEntry, entry}), release, preferred, error));
    CHECK(preferred.format == LinuxReleaseAsset::Format::Elf);
    CHECK(!FindLinuxReleaseAsset(response(QJsonArray{zipEntry, zipEntry}), release, preferred, error));
    ReleaseInfo oldRelease;
    CHECK(ParseReleaseInfo(R"({"tag_name":"Pkg-Extractor-v0.9.1","draft":false,"prerelease":false})",
                           oldRelease, error));
    QJsonObject oldEntry = zipEntry;
    oldEntry.insert(QStringLiteral("name"), QStringLiteral("pkg-extractor-linux-v0.9.1.zip"));
    oldEntry.insert(QStringLiteral("browser_download_url"), QStringLiteral(
        "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/"
        "Pkg-Extractor-v0.9.1/pkg-extractor-linux-v0.9.1.zip"));
    CHECK(FindLinuxReleaseAsset(response(QJsonArray{oldEntry}), oldRelease, preferred, error));
    CHECK(preferred.format == LinuxReleaseAsset::Format::Zip);
#endif
    TempDir dir;
    const fs::path target = dir.path / "pkg_extractor";
    WriteFile(target, {'o', 'l', 'd'});
    const QString path = QString::fromStdString(target.string());
    QString backup;
#ifdef Q_OS_LINUX
    CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner));
    fs::create_symlink(target, dir.path / "link");
    CHECK(!InstallLinuxUpdate(QString::fromStdString((dir.path / "link").string()), elf, asset, backup, error));
#endif
    CHECK(!InstallLinuxUpdate(path, elf + 'x', asset, backup, error));
    CHECK(ReadFile(target) == std::vector<unsigned char>({'o', 'l', 'd'}));
    QByteArray corrupt = elf;
    corrupt[30] = 1;
    CHECK(!InstallLinuxUpdate(path, corrupt, asset, backup, error));
    CHECK(ReadFile(target) == std::vector<unsigned char>({'o', 'l', 'd'}));
    QByteArray badElf = elf;
    badElf[18] = 0x28;
    LinuxReleaseAsset badAsset = asset;
    badAsset.sha256 = QCryptographicHash::hash(badElf, QCryptographicHash::Sha256);
    CHECK(!InstallLinuxUpdate(path, badElf, badAsset, backup, error));
    CHECK(ReadFile(target) == std::vector<unsigned char>({'o', 'l', 'd'}));
    CHECK(InstallLinuxUpdate(path, elf, asset, backup, error));
#ifdef Q_OS_LINUX
    CHECK(QFile::permissions(path) & QFileDevice::ExeOwner);
#endif
    QFile old(backup);
    CHECK(old.open(QIODevice::ReadOnly));
    CHECK(old.readAll() == QByteArray("old"));
    QFile updated(path);
    CHECK(updated.open(QIODevice::ReadOnly));
    CHECK(updated.readAll() == elf);
    CHECK(!InstallLinuxUpdate(QString::fromStdString((dir.path / "missing").string()), elf, asset, backup, error));
#ifdef Q_OS_LINUX
    CHECK(InstallLinuxUpdate(path, zip, zipAsset, backup, error));
    QFile zipUpdated(path);
    CHECK(zipUpdated.open(QIODevice::ReadOnly));
    CHECK(zipUpdated.readAll() == elf);
    auto rejectZip = [&](const QByteArray& badZip) {
        LinuxReleaseAsset invalid = zipAsset;
        invalid.size = badZip.size();
        invalid.sha256 = QCryptographicHash::hash(badZip, QCryptographicHash::Sha256);
        CHECK(!InstallLinuxUpdate(path, badZip, invalid, backup, error));
        QFile installed(path);
        CHECK(installed.open(QIODevice::ReadOnly));
        CHECK(installed.readAll() == elf);
    };
    rejectZip(MakeUpdateZip({{"../pkg_extractor", elf}}));
    rejectZip(MakeUpdateZip({{"pkg_extractor", elf}, {"other/pkg_extractor", elf}}));
    rejectZip(MakeUpdateZip({{"readme.txt", "no executable"}}));
    rejectZip(zip.left(zip.size() / 2));
    QByteArray tampered = zip;
    tampered[30] ^= 1;
    CHECK(!InstallLinuxUpdate(path, tampered, zipAsset, backup, error));
    if (const char* fixture = std::getenv("PKG_TEST_UPDATE_ZIP")) {
        QFile published(QString::fromLocal8Bit(fixture));
        CHECK(published.open(QIODevice::ReadOnly));
        const QByteArray publishedZip = published.readAll();
        LinuxReleaseAsset publishedAsset;
        publishedAsset.size = publishedZip.size();
        publishedAsset.sha256 = QByteArray::fromHex(
            "2d433159481228fb877df597d1f41889f72d8530a51001d19c8e79a032febbcb");
        publishedAsset.format = LinuxReleaseAsset::Format::Zip;
        CHECK(InstallLinuxUpdate(path, publishedZip, publishedAsset, backup, error));
        QFile publishedElf(path);
        CHECK(publishedElf.open(QIODevice::ReadOnly));
        CHECK(publishedElf.read(4) == QByteArray("\x7f" "ELF", 4));
    }
#endif
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

#ifdef PKG_TEST_ARCHIVE
struct ZipItem {
    QByteArray name;
    QByteArray data;
    unsigned type = AE_IFREG;
    QByteArray symlink = {};
    ZipItem(QByteArray n, QByteArray d, unsigned t = AE_IFREG, QByteArray l = {})
        : name(std::move(n)), data(std::move(d)), type(t), symlink(std::move(l)) {}
};

QByteArray MakeZip(const std::vector<ZipItem>& items) {
    QByteArray buffer(4 * 1024 * 1024, '\0');
    size_t written = 0;
    archive* writer = archive_write_new();
    if (!writer || archive_write_set_format_zip(writer) != ARCHIVE_OK ||
        archive_write_open_memory(writer, buffer.data(), static_cast<size_t>(buffer.size()), &written) != ARCHIVE_OK)
        std::abort();
    for (const auto& item : items) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.name.constData());
        archive_entry_set_filetype(entry, item.type);
        archive_entry_set_perm(entry, 0755);
        if (!item.symlink.isEmpty())
            archive_entry_set_symlink(entry, item.symlink.constData());
        archive_entry_set_size(entry, item.type == AE_IFREG ? item.data.size() : 0);
        if (archive_write_header(writer, entry) != ARCHIVE_OK)
            std::abort();
        if (item.type == AE_IFREG && !item.data.isEmpty())
            archive_write_data(writer, item.data.constData(), static_cast<size_t>(item.data.size()));
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    buffer.resize(static_cast<qsizetype>(written));
    return buffer;
}

QByteArray FakePe(bool dll, quint16 machine = 0x8664) {
    QByteArray d(512, '\0');
    d[0] = 'M'; d[1] = 'Z';
    d[0x3c] = 0x80;
    d.replace(0x80, 4, QByteArray("PE\0\0", 4));
    d[0x84] = static_cast<char>(machine & 0xff);
    d[0x85] = static_cast<char>(machine >> 8);
    d[0x94] = static_cast<char>(0xf0);
    const quint16 ch = dll ? 0x2022 : 0x0022;
    d[0x96] = static_cast<char>(ch & 0xff);
    d[0x97] = static_cast<char>(ch >> 8);
    d[0x98] = 0x0b; d[0x99] = 0x02;
    return d;
}

std::vector<ZipItem> ValidItems() {
    const QByteArray p = "pkg-extractor-windows/";
    return {
        {"Start Pkg Extractor.bat", "start"},
        {p, {}, AE_IFDIR},
        {p + "pkg_extractor.exe", FakePe(false)},
        {p + "Qt6Core.dll", FakePe(true)},
        {p + "Qt6Gui.dll", FakePe(true)},
        {p + "Qt6Widgets.dll", FakePe(true)},
        {p + "Qt6Network.dll", FakePe(true)},
        {p + "platforms/", {}, AE_IFDIR},
        {p + "platforms/qwindows.dll", FakePe(true)},
        {p + "install_pkg.bat", "user script"},
        {p + "readme_windows.txt", "readme"},
        {p + "user_notes.txt", "data"},
        {p + "config/settings.ini", "data"},
        {p + "deep/a/ignored.dll", "not a pe"},
    };
}

bool Stage(const std::vector<ZipItem>& items, const QString& dir, QStringList& files, QString& error,
           bool corruptHash = false) {
    const QByteArray zip = MakeZip(items);
    WindowsReleaseAsset asset;
    asset.size = zip.size();
    asset.sha256 = QCryptographicHash::hash(zip, QCryptographicHash::Sha256);
    if (corruptHash)
        asset.sha256[0] = static_cast<char>(asset.sha256[0] ^ 1);
    return StageWindowsUpdate(zip, asset, dir, files, error);
}

#endif

void TestWindowsReleaseAsset() {
    ReleaseInfo release;
    QString error;
    CHECK(ParseReleaseInfo(R"({"tag_name":"0.9.2","draft":false,"prerelease":false})", release, error));
    const QString digest = QStringLiteral("sha256:") + QString(64, QLatin1Char('a'));
    QJsonObject entry{
        {QStringLiteral("name"), QStringLiteral("pkg-extractor-windows-0.9.2.zip")},
        {QStringLiteral("state"), QStringLiteral("uploaded")},
        {QStringLiteral("size"), 1000},
        {QStringLiteral("digest"), digest},
        {QStringLiteral("browser_download_url"), QStringLiteral(
            "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/0.9.2/pkg-extractor-windows-0.9.2.zip")}};
    auto response = [](const QJsonArray& assets) {
        return QJsonDocument(QJsonObject{{QStringLiteral("assets"), assets}}).toJson();
    };
    WindowsReleaseAsset asset;
    CHECK(FindWindowsReleaseAsset(response(QJsonArray{entry}), release, asset, error));
    CHECK(asset.size == 1000 && asset.sha256.size() == 32);
    CHECK(!FindWindowsReleaseAsset(response(QJsonArray{entry, entry}), release, asset, error));
    CHECK(!error.isEmpty());
    QJsonObject bad = entry;
    bad[QStringLiteral("digest")] = QStringLiteral("md5:abc");
    CHECK(!FindWindowsReleaseAsset(response(QJsonArray{bad}), release, asset, error));
    bad = entry;
    bad[QStringLiteral("browser_download_url")] = QStringLiteral("https://example.com/x.zip");
    CHECK(!FindWindowsReleaseAsset(response(QJsonArray{bad}), release, asset, error));
    bad = entry;
    bad[QStringLiteral("name")] = QStringLiteral("pkg-extractor-linux-0.9.2");
    CHECK(!FindWindowsReleaseAsset(response(QJsonArray{bad}), release, asset, error));
}

void TestMacReleaseAsset() {
    ReleaseInfo release;
    QString error;
    CHECK(ParseReleaseInfo(R"({"tag_name":"0.9.2","draft":false,"prerelease":false})", release, error));
    QJsonObject entry{
        {QStringLiteral("name"), QStringLiteral("pkg-extractor-macos-0.9.2.dmg")},
        {QStringLiteral("state"), QStringLiteral("uploaded")},
        {QStringLiteral("size"), 1000},
        {QStringLiteral("digest"), QStringLiteral("sha256:") + QString(64, QLatin1Char('a'))},
        {QStringLiteral("browser_download_url"), QStringLiteral(
            "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/0.9.2/pkg-extractor-macos-0.9.2.dmg")}};
    auto response = [](const QJsonArray& assets) {
        return QJsonDocument(QJsonObject{{QStringLiteral("assets"), assets}}).toJson();
    };
    MacReleaseAsset asset;
    CHECK(FindMacReleaseAsset(response(QJsonArray{entry}), release, asset, error));
    CHECK(asset.size == 1000 && asset.sha256.size() == 32);
    CHECK(asset.format == MacReleaseAsset::Format::Dmg);
    CHECK(!FindMacReleaseAsset(response(QJsonArray{entry, entry}), release, asset, error));
    QJsonObject bad = entry;
    bad[QStringLiteral("digest")] = QStringLiteral("md5:abc");
    CHECK(!FindMacReleaseAsset(response(QJsonArray{bad}), release, asset, error));
    bad = entry;
    bad[QStringLiteral("browser_download_url")] = QStringLiteral("https://example.com/x.dmg");
    CHECK(!FindMacReleaseAsset(response(QJsonArray{bad}), release, asset, error));
    bad = entry;
    bad[QStringLiteral("size")] = 128 * 1024 * 1024 + 1;
    CHECK(!FindMacReleaseAsset(response(QJsonArray{bad}), release, asset, error));
    bad = entry;
    bad[QStringLiteral("state")] = QStringLiteral("new");
    CHECK(!FindMacReleaseAsset(response(QJsonArray{bad}), release, asset, error));
    CHECK(ParseReleaseInfo(R"({"tag_name":"Pkg-Extractor-v0.9.1","draft":false,"prerelease":false})",
                           release, error));
    QJsonObject zip = entry;
    zip[QStringLiteral("name")] = QStringLiteral("pkg-extractor-macos-v0.9.1.zip");
    zip[QStringLiteral("browser_download_url")] = QStringLiteral(
        "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/Pkg-Extractor-v0.9.1/pkg-extractor-macos-v0.9.1.zip");
    CHECK(FindMacReleaseAsset(response(QJsonArray{zip}), release, asset, error));
    CHECK(asset.format == MacReleaseAsset::Format::Zip);
    CHECK(!FindMacReleaseAsset(response(QJsonArray{zip, zip}), release, asset, error));
    QJsonObject dmg = zip;
    dmg[QStringLiteral("name")] = QStringLiteral("pkg-extractor-macos-Pkg-Extractor-v0.9.1.dmg");
    dmg[QStringLiteral("browser_download_url")] = QStringLiteral(
        "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/Pkg-Extractor-v0.9.1/pkg-extractor-macos-Pkg-Extractor-v0.9.1.dmg");
    CHECK(FindMacReleaseAsset(response(QJsonArray{zip, dmg}), release, asset, error));
    CHECK(asset.format == MacReleaseAsset::Format::Dmg);
}

#ifdef Q_OS_MACOS
void TestMacUpdateStaging() {
    TempDir dir;
    const QString stage = QString::fromStdString(dir.path.string());
    QString error;
    const QByteArray invalid("not a disk image");
    MacReleaseAsset asset{{}, QByteArray(32, 'x'), invalid.size()};
    CHECK(!StageMacUpdate(invalid, asset, stage, error));
    CHECK(!error.isEmpty());
    CHECK(!QFile::exists(stage + QStringLiteral("/update.dmg")));
    const char* bundlePath = std::getenv("PKG_TEST_MAC_BUNDLE");
    if (bundlePath) {
        QString bundle;
        const QString path = QString::fromLocal8Bit(bundlePath);
        CHECK(CanUpdateMacInstallation(path + QStringLiteral("/Contents/MacOS/pkg_extractor"), bundle, error));
        CHECK(bundle == path);
        CHECK(QSysInfo::currentCpuArchitecture() == QStringLiteral("arm64"));
    }
    const char* imagePath = std::getenv("PKG_TEST_MAC_DMG");
    if (!imagePath) {
        std::printf("  skipped real image (set PKG_TEST_MAC_DMG to enable)\n");
        return;
    }
    QFile image(QString::fromLocal8Bit(imagePath));
    CHECK(image.open(QIODevice::ReadOnly));
    const QByteArray data = image.readAll();
    CHECK(!data.isEmpty());
    asset = {{}, QCryptographicHash::hash(data, QCryptographicHash::Sha256), data.size()};
    CHECK(StageMacUpdate(data, asset, stage, error));
    if (!error.isEmpty())
        std::fprintf(stderr, "  staging error: %s\n", qPrintable(error));
    CHECK(QFile::exists(stage + QStringLiteral("/payload.app/Contents/MacOS/pkg_extractor")));
}

void TestMacZipStaging() {
    auto zipWith = [](const char* path, const char* symlink) {
        QByteArray data(4096, '\0');
        size_t used = 0;
        archive* writer = archive_write_new();
        if (!writer || archive_write_set_format_zip(writer) != ARCHIVE_OK ||
            archive_write_open_memory(writer, data.data(), data.size(), &used) != ARCHIVE_OK)
            std::abort();
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, path);
        archive_entry_set_filetype(entry, symlink ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(entry, 0755);
        if (symlink) archive_entry_set_symlink(entry, symlink);
        archive_entry_set_size(entry, 0);
        if (archive_write_header(writer, entry) != ARCHIVE_OK) std::abort();
        archive_entry_free(entry);
        archive_write_close(writer);
        archive_write_free(writer);
        data.resize(static_cast<qsizetype>(used));
        return data;
    };
    for (const auto& data : {zipWith("../escape", nullptr),
                             zipWith("pkg_extractor.app/../../escape", nullptr),
                             zipWith("pkg_extractor.app/link", "../../escape")}) {
        TempDir dir;
        QString error;
        CHECK(!ExtractMacUpdateZip(data, QString::fromStdString(dir.path.string()), error));
        CHECK(!error.isEmpty());
    }
    const char* bundle = std::getenv("PKG_TEST_MAC_BUNDLE");
    if (!bundle) {
        std::printf("  skipped real bundle (set PKG_TEST_MAC_BUNDLE to enable)\n");
        return;
    }
    TempDir dir;
    const QString root = QString::fromStdString(dir.path.string());
    const QString zip = root + QStringLiteral("/update.zip");
    CHECK(QProcess::execute(QStringLiteral("/usr/bin/ditto"),
          {QStringLiteral("-c"), QStringLiteral("-k"), QStringLiteral("--sequesterRsrc"),
           QStringLiteral("--keepParent"), QString::fromLocal8Bit(bundle), zip}) == 0);
    QFile file(zip);
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray data = file.readAll();
    MacReleaseAsset asset{{}, QCryptographicHash::hash(data, QCryptographicHash::Sha256),
                          data.size(), MacReleaseAsset::Format::Zip};
    QString error;
    CHECK(StageMacUpdate(data, asset, root, error));
    if (!error.isEmpty()) std::fprintf(stderr, "  ZIP staging error: %s\n", qPrintable(error));
    CHECK(QFile::exists(root + QStringLiteral("/payload.app/Contents/MacOS/pkg_extractor")));
    const char* legacyPath = std::getenv("PKG_TEST_MAC_LEGACY_ZIP");
    if (legacyPath) {
        TempDir legacyDir;
        QFile legacy(QString::fromLocal8Bit(legacyPath));
        CHECK(legacy.open(QIODevice::ReadOnly));
        const QByteArray oldData = legacy.readAll();
        const MacReleaseAsset oldAsset{{}, QCryptographicHash::hash(oldData, QCryptographicHash::Sha256),
                                       oldData.size(), MacReleaseAsset::Format::Zip};
        CHECK(!StageMacUpdate(oldData, oldAsset, QString::fromStdString(legacyDir.path.string()), error));
        if (!error.contains(QStringLiteral("/usr/bin/codesign")))
            std::fprintf(stderr, "  legacy ZIP rejection: %s\n", qPrintable(error));
        CHECK(error.contains(QStringLiteral("/usr/bin/codesign")));
    }
}

void TestMacBundleReplacement() {
    TempDir dir;
    const QString root = QString::fromStdString(dir.path.string());
    const QString bundle = root + QStringLiteral("/Installed.app");
    const QString stage = root + QStringLiteral("/stage");
    CHECK(QDir().mkdir(bundle));
    CHECK(QDir().mkdir(stage));
    WriteFile(dir.path / "Installed.app" / "old", {'o'});
    QString error;
    CHECK(!ReplaceMacBundle(stage, bundle, error));
    CHECK(!error.isEmpty());
    CHECK(QFile::exists(bundle + QStringLiteral("/old")));
    CHECK(!QFile::exists(stage + QStringLiteral("/previous.app")));
    CHECK(QDir().mkdir(stage + QStringLiteral("/payload.app")));
    WriteFile(dir.path / "stage" / "payload.app" / "new", {'n'});
    CHECK(ReplaceMacBundle(stage, bundle, error));
    CHECK(QFile::exists(bundle + QStringLiteral("/new")));
    CHECK(QFile::exists(stage + QStringLiteral("/previous.app/old")));
    CHECK(!ReplaceMacBundle(stage, bundle, error));
    CHECK(QFile::exists(bundle + QStringLiteral("/new")));
    CHECK(QFile::exists(stage + QStringLiteral("/previous.app/old")));
}
#endif

#ifdef PKG_TEST_ARCHIVE
void TestWindowsUpdateStaging() {
    TempDir dir;
    const QString staging = QString::fromStdString((dir.path / "staging").string());
    QStringList files;
    QString error;
    CHECK(Stage(ValidItems(), staging, files, error));
    CHECK(files == QStringList({"Qt6Core.dll", "Qt6Gui.dll", "Qt6Network.dll", "Qt6Widgets.dll",
                                "pkg_extractor.exe", "platforms/qwindows.dll"}));
    const QDir root(staging + "/pkg-extractor-windows");
    CHECK(root.exists("pkg_extractor.exe") && root.exists("platforms/qwindows.dll"));
    CHECK(!root.exists("user_notes.txt") && !root.exists("config") && !root.exists("deep"));
    CHECK(!root.exists("install_pkg.bat") && !root.exists("readme_windows.txt"));
    // An existing staged update is never deleted or overwritten.
    CHECK(!Stage(ValidItems(), staging, files, error));
    CHECK(!error.isEmpty() && files.isEmpty());
    CHECK(root.exists("pkg_extractor.exe"));
    {
        auto items = ValidItems();
        items.push_back({QByteArray("pkg-extractor-windows/pkg_extractor_cli.exe"), FakePe(false)});
        TempDir cliDir;
        QStringList cliFiles;
        QString cliError;
        CHECK(Stage(items, QString::fromStdString(cliDir.path.string()), cliFiles, cliError));
        CHECK(cliFiles.contains(QStringLiteral("pkg_extractor_cli.exe")));
        items.back().data = FakePe(true);
        TempDir invalidDir;
        CHECK(!Stage(items, QString::fromStdString(invalidDir.path.string()), cliFiles, cliError));
    }

    auto expectRejectAt = [&](int line, std::vector<ZipItem> items) {
        TempDir d;
        const QString s = QString::fromStdString(d.path.string());
        QStringList f;
        QString e;
        const bool staged = Stage(items, s, f, e);
        if (staged)
            std::fprintf(stderr, "  reject case at line %d was accepted\n", line);
        CHECK(!staged);
        CHECK(!e.isEmpty() && f.isEmpty());
        CHECK(!QDir(s + "/pkg-extractor-windows").exists());
    };
#define expectReject(items) expectRejectAt(__LINE__, items)
    const QByteArray p = "pkg-extractor-windows/";
    auto with = [&](ZipItem item) { auto v = ValidItems(); v.push_back(item); return v; };
    auto without = [&](const QByteArray& name) {
        auto v = ValidItems();
        std::erase_if(v, [&](const ZipItem& i) { return i.name == name; });
        return v;
    };
    expectReject(with({p + "../evil.dll", "x"}));
    expectReject(with({"../evil", "x"}));
    expectReject(with({"/abs.txt", "x"}));
    {
        // Build with '/' and patch the raw local and central header names to '\\' (same length).
        QByteArray zip = MakeZip(with({p + "zz/b.txt", "x"}));
        const QByteArray from = p + "zz/b.txt";
        QByteArray to = from;
        to[to.size() - 6] = '\\';
        CHECK(zip.count(from) == 2);
        zip.replace(from, to);
        CHECK(zip.count(to) == 2);
        TempDir d;
        const QString s = QString::fromStdString(d.path.string());
        WindowsReleaseAsset asset{{}, QCryptographicHash::hash(zip, QCryptographicHash::Sha256), zip.size()};
        QStringList f;
        QString e;
        const bool staged = StageWindowsUpdate(zip, asset, s, f, e);
        std::fprintf(stderr, "  raw backslash entry: %s\n", staged ? "accepted (reader normalizes)" : "rejected");
        if (staged) {
            // Must be treated as the harmless, ignored non-DLL payload file zz/b.txt.
            CHECK(!f.join(',').contains("zz") && !f.join(',').contains('\\'));
            CHECK(!QDir(s + "/pkg-extractor-windows").exists("zz"));
        } else {
            CHECK(!e.isEmpty() && f.isEmpty());
            CHECK(!QDir(s + "/pkg-extractor-windows").exists());
        }
    }
    expectReject(with({p + "C:evil.txt", "x"}));
    expectReject(with({p + "sub/./x.txt", "x"}));
    expectReject(with({p + "NUL.txt", "x"}));
    expectReject(with({p + "trailing.", "x"}));
    expectReject(with({p + "link", {}, AE_IFLNK, "pkg_extractor.exe"}));
    expectReject(with({p + "qt6core.DLL", FakePe(true)}));
    expectReject(with({p + "platforms", "x"}));
    expectReject(with({p + "pkg_extractor.exe", FakePe(false)}));
    expectReject(with({"other.txt", "x"}));
    expectReject(with({p + "bad.dll", "not a pe"}));
    expectReject(with({p + "other.exe", FakePe(false)}));
    expectReject(with({p + "platforms/other.exe", FakePe(false)}));
    expectReject(without("Start Pkg Extractor.bat"));
    expectReject(without(p + "pkg_extractor.exe"));
    expectReject(without(p + "platforms/qwindows.dll"));
    expectReject(without(p + "Qt6Widgets.dll"));
    {
        auto v = ValidItems();
        for (auto& i : v)
            if (i.name == p + "pkg_extractor.exe")
                i.data = FakePe(false, 0x14c);
        expectReject(v);
        for (auto& i : v)
            if (i.name == p + "pkg_extractor.exe")
                i.data = "MZ not a pe";
        expectReject(v);
    }
    {
        TempDir d;
        QStringList f;
        QString e;
        CHECK(!Stage(ValidItems(), QString::fromStdString(d.path.string()), f, e, true));
        const QByteArray zip = MakeZip(ValidItems());
        WindowsReleaseAsset asset{{}, QCryptographicHash::hash(zip, QCryptographicHash::Sha256), zip.size() + 1};
        CHECK(!StageWindowsUpdate(zip, asset, QString::fromStdString(d.path.string()), f, e));
        const QByteArray junk("not a zip at all");
        asset = {{}, QCryptographicHash::hash(junk, QCryptographicHash::Sha256), junk.size()};
        CHECK(!StageWindowsUpdate(junk, asset, QString::fromStdString(d.path.string()), f, e));
    }
}

void TestRealWindowsZip() {
    const char* path = std::getenv("PKG_TEST_WINDOWS_ZIP");
    if (!path) {
        std::printf("  skipped (set PKG_TEST_WINDOWS_ZIP to enable)\n");
        return;
    }
    QFile file(QString::fromLocal8Bit(path));
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray zip = file.readAll();
    CHECK(!zip.isEmpty());
    WindowsReleaseAsset asset{{}, QCryptographicHash::hash(zip, QCryptographicHash::Sha256), zip.size()};
    TempDir dir;
    QStringList files;
    QString error;
    const bool staged = StageWindowsUpdate(zip, asset, QString::fromStdString(dir.path.string()), files, error);
    if (!staged)
        std::fprintf(stderr, "  staging failed: %s\n", error.toLocal8Bit().constData());
    CHECK(staged);
    for (const char* required : {"pkg_extractor.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
                                 "Qt6Network.dll", "platforms/qwindows.dll"}) {
        CHECK(files.contains(QString::fromLatin1(required), Qt::CaseInsensitive));
        CHECK(QFile::exists(QString::fromStdString((dir.path / "pkg-extractor-windows").string()) + "/" +
                            QString::fromLatin1(required)));
    }
    std::printf("  staged %d files\n", static_cast<int>(files.size()));
}
#endif

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"Release version parsing", TestReleaseInfo},
        {"Skipped update settings", TestUpdateSettings},
        {"Linux update validation and replacement", TestLinuxUpdate},
        {"Windows release asset selection", TestWindowsReleaseAsset},
        {"macOS release asset selection", TestMacReleaseAsset},
#ifdef Q_OS_MACOS
        {"macOS update staging", TestMacUpdateStaging},
        {"macOS ZIP staging", TestMacZipStaging},
        {"macOS update replacement and rollback", TestMacBundleReplacement},
#endif
#ifdef PKG_TEST_ARCHIVE
        {"Windows update staging validation", TestWindowsUpdateStaging},
        {"Real Windows release ZIP staging", TestRealWindowsZip},
#endif
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
