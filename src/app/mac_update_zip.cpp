#include "app/mac_update_zip.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QVector>

#include <archive.h>
#include <archive_entry.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool Reject(QString& error)
{
    error = QStringLiteral("The downloaded update failed integrity or architecture validation.");
    return false;
}

bool SafeParents(const QString& root, const QString& relative)
{
    QString current = root;
    const QStringList parts = relative.split(QLatin1Char('/'));
    for (int i = 0; i + 1 < parts.size(); ++i)
    {
        current += QLatin1Char('/') + parts[i];
        const QFileInfo info(current);
        if (info.isSymLink() || (info.exists() && !info.isDir()))
            return false;
        if (!info.exists() && !QDir().mkdir(current))
            return false;
    }
    return true;
}
}

bool ExtractMacUpdateZip(const QByteArray& data, const QString& stage, QString& error)
{
    using Reader = std::unique_ptr<archive, decltype(&archive_read_free)>;
    Reader reader(archive_read_new(), &archive_read_free);
    if (!reader || archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        archive_read_open_memory(reader.get(), data.constData(), static_cast<size_t>(data.size())) != ARCHIVE_OK)
        return Reject(error);

    const QString root = stage + QStringLiteral("/payload.app");
    if (!QDir().mkdir(root))
        return Reject(error);
    struct Link { QString path; QByteArray target; };
    QVector<Link> links;
    QSet<QString> paths;
    qint64 total = 0;
    int count = 0;
    archive_entry* entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK)
    {
        if (++count > 10000 || !archive_entry_pathname(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry) != 0)
            return Reject(error);
        const QByteArray raw(archive_entry_pathname(entry));
        QString path = QString::fromUtf8(raw);
        if (path.toUtf8() != raw || path.size() > 1024 || path.contains(QLatin1Char('\\')) ||
            path.startsWith(QLatin1Char('/')))
            return Reject(error);
        if (path.endsWith(QLatin1Char('/'))) path.chop(1);
        const QStringList parts = path.split(QLatin1Char('/'));
        if (parts.size() > 48 || parts.isEmpty() || parts.first() != QStringLiteral("pkg_extractor.app"))
            return Reject(error);
        for (const QString& part : parts)
            if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") || part.size() > 255)
                return Reject(error);
        if (parts.size() == 1)
        {
            if (archive_entry_filetype(entry) != AE_IFDIR || paths.contains(QStringLiteral("/")))
                return Reject(error);
            paths.insert(QStringLiteral("/"));
            continue;
        }
        const QString relative = parts.mid(1).join(QLatin1Char('/'));
        const QString key = relative.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (paths.contains(key) || !SafeParents(root, relative))
            return Reject(error);
        paths.insert(key);
        const QString target = root + QLatin1Char('/') + relative;
        const auto type = archive_entry_filetype(entry);
        if (type == AE_IFDIR)
        {
            const QFileInfo info(target);
            if (info.isSymLink() || (info.exists() && !info.isDir()) ||
                (!info.exists() && !QDir().mkdir(target)))
                return Reject(error);
        }
        else if (type == AE_IFLNK)
        {
            const char* link = archive_entry_symlink(entry);
            if (!link || QFileInfo(target).exists() || QFileInfo(target).isSymLink())
                return Reject(error);
            const QByteArray bytes(link);
            const QString value = QString::fromUtf8(bytes);
            const QString resolved = QDir::cleanPath(QFileInfo(target).path() + QLatin1Char('/') + value);
            if (value.isEmpty() || value.toUtf8() != bytes || value.startsWith(QLatin1Char('/')) ||
                value.contains(QLatin1Char('\\')) || !resolved.startsWith(root + QLatin1Char('/')))
                return Reject(error);
            links.append({target, bytes});
        }
        else if (type == AE_IFREG)
        {
            const qint64 size = archive_entry_size(entry);
            if (size < 0 || size > 256LL * 1024 * 1024 || total + size > 1024LL * 1024 * 1024)
                return Reject(error);
            total += size;
            QFile output(target);
            if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return Reject(error);
            char buffer[65536];
            qint64 written = 0;
            la_ssize_t n;
            while ((n = archive_read_data(reader.get(), buffer, sizeof(buffer))) > 0)
            {
                written += n;
                if (written > size || output.write(buffer, n) != n)
                    return Reject(error);
            }
            if (n < 0 || written != size || !output.flush())
                return Reject(error);
            output.close();
            const mode_t mode = archive_entry_perm(entry) & 0755;
            if (::chmod(QFile::encodeName(target).constData(), mode | S_IRUSR | S_IWUSR) != 0)
                return Reject(error);
        }
        else
            return Reject(error);
    }
    if (status != ARCHIVE_EOF)
        return Reject(error);
    for (const Link& link : links)
        if (::symlink(link.target.constData(), QFile::encodeName(link.path).constData()) != 0)
            return Reject(error);
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return Reject(error);
    for (const Link& link : links)
    {
        const QString resolved = QFileInfo(link.path).canonicalFilePath();
        if (resolved.isEmpty() || !resolved.startsWith(canonicalRoot + QLatin1Char('/')))
            return Reject(error);
    }
    error.clear();
    return true;
}
