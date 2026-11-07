#include "app/windows_update_archive.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>

#include <archive.h>
#include <archive_entry.h>
#include <memory>

namespace {
constexpr int kMaxEntries = 4096;
constexpr qint64 kMaxFileSize = 256LL * 1024 * 1024;
constexpr qint64 kMaxTotalSize = 768LL * 1024 * 1024;
constexpr qint64 kMaxArchiveSize = 128LL * 1024 * 1024;
const QString kPayloadDir = QStringLiteral("pkg-extractor-windows");
const QString kLauncher = QStringLiteral("Start Pkg Extractor.bat");

struct Payload
{
    QString path;
    QByteArray data;
};

bool Fail(QString& error, const QString& reason)
{
    error = QStringLiteral("The downloaded update failed validation: %1").arg(reason);
    return false;
}

bool IsReservedName(const QString& component)
{
    QString base = component.left(component.indexOf(QLatin1Char('.'))).toUpper();
    static const QStringList devices{QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                     QStringLiteral("NUL"), QStringLiteral("CONIN$"), QStringLiteral("CONOUT$")};
    if (devices.contains(base))
        return true;
    return base.size() == 4 && (base.startsWith(QStringLiteral("COM")) || base.startsWith(QStringLiteral("LPT"))) &&
        base[3] >= QLatin1Char('1') && base[3] <= QLatin1Char('9');
}

bool IsValidComponent(const QString& c)
{
    if (c.isEmpty() || c == QStringLiteral(".") || c == QStringLiteral("..") || c.size() > 255 ||
        c.endsWith(QLatin1Char('.')) || c.endsWith(QLatin1Char(' ')) || IsReservedName(c))
        return false;
    for (const QChar ch : c)
    {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f ||
            QStringLiteral("<>:\"/\\|?*").contains(ch) || ch.isSurrogate())
            return false;
    }
    return true;
}

// Splits an archive path into validated components; `isDir` is set for trailing-slash entries.
bool SplitPath(const char* raw, QStringList& parts, bool& isDir)
{
    if (!raw)
        return false;
    const QByteArray bytes(raw);
    const QString path = QString::fromUtf8(bytes);
    if (path.isEmpty() || path.toUtf8() != bytes || path.startsWith(QLatin1Char('/')) ||
        path.contains(QLatin1Char('\\')) || path.size() > 1024)
        return false;
    isDir = path.endsWith(QLatin1Char('/'));
    parts = (isDir ? path.chopped(1) : path).split(QLatin1Char('/'));
    for (const QString& part : parts)
        if (!IsValidComponent(part))
            return false;
    return parts.size() <= 32;
}

bool IsUserEditable(const QString& relative)
{
    static const QStringList names{QStringLiteral("install_pkg.bat"), QStringLiteral("batch_install.bat"),
                                   QStringLiteral("readme_windows.txt")};
    return names.contains(relative.toLower());
}

// Only known executables and DLLs in the payload root or one plugin directory deep are staged.
enum class PayloadKind { Ignore, Executable, Dll, Reject };

PayloadKind Classify(const QStringList& relativeParts)
{
    const QString lower = relativeParts.join(QLatin1Char('/')).toLower();
    if (relativeParts.size() == 1 && IsUserEditable(lower))
        return PayloadKind::Ignore;
    if (lower.endsWith(QStringLiteral(".exe")))
        return lower == QStringLiteral("pkg_extractor.exe") || lower == QStringLiteral("pkg_extractor_cli.exe")
            ? PayloadKind::Executable : PayloadKind::Reject;
    if (lower.endsWith(QStringLiteral(".dll")) && relativeParts.size() <= 2)
        return PayloadKind::Dll;
    return PayloadKind::Ignore;
}

quint32 ReadLe(const QByteArray& d, qsizetype offset, int bytes)
{
    quint32 v = 0;
    for (int i = bytes - 1; i >= 0; --i)
        v = (v << 8) | static_cast<unsigned char>(d[offset + i]);
    return v;
}

bool IsX64Pe(const QByteArray& d, bool wantDll)
{
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z')
        return false;
    const quint32 pe = ReadLe(d, 0x3c, 4);
    if (pe < 0x40 || pe > static_cast<quint32>(d.size()) || d.size() - pe < 24 + 96 ||
        d.mid(pe, 4) != QByteArray("PE\0\0", 4))
        return false;
    const quint32 optSize = ReadLe(d, pe + 20, 2);
    const quint32 characteristics = ReadLe(d, pe + 22, 2);
    return ReadLe(d, pe + 4, 2) == 0x8664 && optSize >= 112 && pe + 24 + optSize <= static_cast<quint32>(d.size()) &&
        ReadLe(d, pe + 24, 2) == 0x20b && (characteristics & 0x0002) != 0 &&
        ((characteristics & 0x2000) != 0) == wantDll;
}

bool ReadArchive(const QByteArray& data, QList<Payload>& payload, QString& error)
{
    using Archive = std::unique_ptr<archive, decltype(&archive_read_free)>;
    Archive reader(archive_read_new(), &archive_read_free);
    if (!reader || archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        archive_read_open_memory(reader.get(), data.constData(), static_cast<size_t>(data.size())) != ARCHIVE_OK)
        return Fail(error, QStringLiteral("not a readable ZIP archive"));

    QHash<QString, bool> seen; // lowercase path -> is directory
    bool launcher = false;
    qint64 total = 0;
    int count = 0;
    archive_entry* entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK)
    {
        if (++count > kMaxEntries)
            return Fail(error, QStringLiteral("too many entries"));
        QStringList parts;
        bool isDir = false;
        if (!SplitPath(archive_entry_pathname(entry), parts, isDir))
            return Fail(error, QStringLiteral("unsafe entry path"));
        const unsigned type = archive_entry_filetype(entry);
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry) || archive_entry_is_encrypted(entry) != 0 ||
            (type != AE_IFREG && type != AE_IFDIR) || (type == AE_IFDIR) != isDir)
            return Fail(error, QStringLiteral("unsupported entry type"));

        const QString full = parts.join(QLatin1Char('/'));
        const QString key = full.toLower();
        if (seen.contains(key))
            return Fail(error, QStringLiteral("duplicate entry"));
        for (int i = 1; i < parts.size(); ++i)
        {
            const auto it = seen.constFind(parts.mid(0, i).join(QLatin1Char('/')).toLower());
            if (it != seen.constEnd() && !it.value())
                return Fail(error, QStringLiteral("entry conflicts with a file"));
        }
        seen.insert(key, isDir);

        const bool isLauncher = parts.size() == 1 && !isDir && parts[0] == kLauncher;
        const bool inPayload = parts.size() > 1 && parts[0] == kPayloadDir;
        const bool payloadRoot = parts.size() == 1 && isDir && parts[0] == kPayloadDir;
        if (!isLauncher && !inPayload && !payloadRoot)
            return Fail(error, QStringLiteral("unexpected top-level entry"));
        if (isLauncher)
            launcher = true;
        if (isDir)
            continue;

        const qint64 size = archive_entry_size(entry);
        if (!archive_entry_size_is_set(entry) || size < 0 || size > kMaxFileSize || (total += size) > kMaxTotalSize)
            return Fail(error, QStringLiteral("entry size out of bounds"));
        QByteArray content;
        content.reserve(static_cast<qsizetype>(size));
        char buffer[65536];
        la_ssize_t n;
        while ((n = archive_read_data(reader.get(), buffer, sizeof(buffer))) > 0)
        {
            if (content.size() + n > size)
                return Fail(error, QStringLiteral("entry size mismatch"));
            content.append(buffer, static_cast<qsizetype>(n));
        }
        if (n < 0 || content.size() != size)
            return Fail(error, QStringLiteral("corrupt entry data"));
        if (inPayload)
        {
            const PayloadKind kind = Classify(parts.mid(1));
            if (kind == PayloadKind::Reject)
                return Fail(error, QStringLiteral("unexpected executable"));
            if (kind != PayloadKind::Ignore)
                payload.append({parts.mid(1).join(QLatin1Char('/')), std::move(content)});
        }
    }
    if (status != ARCHIVE_EOF)
        return Fail(error, QStringLiteral("corrupt archive"));
    if (!launcher)
        return Fail(error, QStringLiteral("missing launcher"));
    return true;
}

bool ValidatePayload(const QList<Payload>& payload, QString& error)
{
    static const QStringList required{
        QStringLiteral("pkg_extractor.exe"), QStringLiteral("qt6core.dll"), QStringLiteral("qt6gui.dll"),
        QStringLiteral("qt6widgets.dll"), QStringLiteral("qt6network.dll"), QStringLiteral("platforms/qwindows.dll")};
    QSet<QString> present;
    for (const Payload& p : payload)
    {
        const QString lower = p.path.toLower();
        present.insert(lower);
        if (lower.endsWith(QStringLiteral(".exe")) || lower.endsWith(QStringLiteral(".dll")))
        {
            if (!IsX64Pe(p.data, lower.endsWith(QStringLiteral(".dll"))))
                return Fail(error, QStringLiteral("%1 is not a Windows x64 binary").arg(p.path));
        }
    }
    for (const QString& r : required)
        if (!present.contains(r))
            return Fail(error, QStringLiteral("missing required file %1").arg(r));
    return true;
}

bool WritePayload(const QList<Payload>& payload, const QString& stagingDir, QString& error)
{
    const QString target = QDir(stagingDir).filePath(kPayloadDir);
    const QFileInfo existing(target);
    if (existing.isSymLink() || existing.exists())
    {
        error = QStringLiteral("A staged update already exists; remove it before staging again.");
        return false;
    }
    if (!QDir().mkpath(stagingDir))
    {
        error = QStringLiteral("Could not create the update staging directory.");
        return false;
    }
    QTemporaryDir temp(QDir(stagingDir).filePath(QStringLiteral(".pkg-extractor-staging-XXXXXX")));
    if (!temp.isValid())
    {
        error = QStringLiteral("Could not create the update staging directory.");
        return false;
    }
    const QDir root(temp.path());
    for (const Payload& p : payload)
    {
        const QString path = root.filePath(p.path);
        QFile file(path);
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly) ||
            file.write(p.data) != p.data.size() || !file.flush())
        {
            error = QStringLiteral("Could not stage %1: %2").arg(p.path, file.errorString());
            return false;
        }
    }
    if (!QDir().rename(temp.path(), target))
    {
        error = QStringLiteral("Could not finalize the staged update.");
        return false;
    }
    temp.setAutoRemove(false);
    return true;
}
}

bool StageWindowsUpdate(const QByteArray& data, const WindowsReleaseAsset& asset,
                        const QString& stagingDir, QStringList& files, QString& error)
{
    files.clear();
    if (data.size() != asset.size || data.size() > kMaxArchiveSize || asset.sha256.size() != 32 ||
        QCryptographicHash::hash(data, QCryptographicHash::Sha256) != asset.sha256)
        return Fail(error, QStringLiteral("size or checksum mismatch"));

    QList<Payload> payload;
    if (!ReadArchive(data, payload, error) || !ValidatePayload(payload, error) ||
        !WritePayload(payload, stagingDir, error))
        return false;
    for (const Payload& p : payload)
        files.append(p.path);
    files.sort();
    error.clear();
    return true;
}
