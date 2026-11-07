#include "app/linux_update.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>

#ifdef Q_OS_LINUX
#include <archive.h>
#include <archive_entry.h>
#include <memory>

namespace {
bool ExtractExecutable(const QByteArray& data, QByteArray& executable)
{
    using Archive = std::unique_ptr<archive, decltype(&archive_read_free)>;
    Archive reader(archive_read_new(), &archive_read_free);
    if (!reader || archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        archive_read_open_memory(reader.get(), data.constData(), static_cast<size_t>(data.size())) != ARCHIVE_OK)
        return false;

    int files = 0;
    int matches = 0;
    archive_entry* entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK)
    {
        if (++files > 4096)
            return false;
        const char* rawPath = archive_entry_pathname(entry);
        if (!rawPath)
            return false;
        const QString path = QString::fromUtf8(rawPath);
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\')) ||
            parts.contains(QStringLiteral("..")))
            return false;
        if (parts.isEmpty() || parts.last() != QStringLiteral("pkg_extractor"))
            continue;
        if (++matches != 1 || archive_entry_filetype(entry) != AE_IFREG ||
            archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry) != 0 ||
            archive_entry_size(entry) < 20 || archive_entry_size(entry) > 128 * 1024 * 1024)
            return false;

        char buffer[65536];
        la_ssize_t n;
        while ((n = archive_read_data(reader.get(), buffer, sizeof(buffer))) > 0)
        {
            if (executable.size() + n > 128 * 1024 * 1024)
                return false;
            executable.append(buffer, static_cast<qsizetype>(n));
        }
        if (n < 0 || executable.size() != archive_entry_size(entry))
            return false;
    }
    return status == ARCHIVE_EOF && matches == 1;
}
}
#endif

bool InstallLinuxUpdate(const QString& path, const QByteArray& data, const LinuxReleaseAsset& asset,
                        QString& backupPath, QString& error)
{
    backupPath.clear();
    QByteArray executable;
    bool valid = data.size() == asset.size && asset.sha256.size() == 32 &&
        QCryptographicHash::hash(data, QCryptographicHash::Sha256) == asset.sha256;
    if (valid && asset.format == LinuxReleaseAsset::Format::Zip)
    {
#ifdef Q_OS_LINUX
        valid = ExtractExecutable(data, executable);
#else
        valid = false;
#endif
    }
    else if (valid)
        executable = data;
    if (!valid || executable.size() < 20 ||
        executable.left(6) != QByteArray("\x7f" "ELF\x02\x01", 6) ||
        static_cast<unsigned char>(executable[18]) != 0x3e || executable[19] != 0)
    {
        error = QStringLiteral("The downloaded update failed integrity or architecture validation.");
        return false;
    }

    QFileInfo original(path);
    if (!original.isFile() || original.isSymLink())
    {
        error = QStringLiteral("The installed executable is not a regular file.");
        return false;
    }
    QFile input(path);
    QTemporaryFile backup(path + QStringLiteral(".backup.XXXXXX"));
    if (!input.open(QIODevice::ReadOnly) || !backup.open())
    {
        error = QStringLiteral("Could not create an update backup: %1").arg(
            input.isOpen() ? backup.errorString() : input.errorString());
        return false;
    }
    while (!input.atEnd())
    {
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty() || backup.write(chunk) != chunk.size())
        {
            error = QStringLiteral("Could not create an update backup: %1").arg(
                chunk.isEmpty() ? input.errorString() : backup.errorString());
            return false;
        }
    }
    if (!backup.setPermissions(original.permissions()) || !backup.flush())
    {
        error = QStringLiteral("Could not create an update backup: %1").arg(backup.errorString());
        return false;
    }
    input.close();
    backup.close();

    QSaveFile replacement(path);
    replacement.setDirectWriteFallback(false);
    if (!replacement.open(QIODevice::WriteOnly) ||
        !replacement.setPermissions(original.permissions()) ||
        replacement.write(executable) != executable.size() || !replacement.commit())
    {
        error = QStringLiteral("Could not replace the executable: %1").arg(replacement.errorString());
        return false;
    }
    backup.setAutoRemove(false);
    backupPath = backup.fileName();
    error.clear();
    return true;
}
