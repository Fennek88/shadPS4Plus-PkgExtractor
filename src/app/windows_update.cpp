#include "app/windows_update.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>

#include <windows.h>

namespace {
bool IsSafePath(const QString& root, const QString& relative)
{
    const QStringList parts = relative.split(QLatin1Char('/'));
    if (parts.isEmpty())
        return false;
    QString current = root;
    for (const QString& part : parts)
    {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") ||
            part.contains(QLatin1Char('\\')) || part.contains(QLatin1Char(':')))
            return false;
        current += QLatin1Char('/') + part;
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    return true;
}

bool CopyAtomically(const QString& source, const QString& destination, QString& error)
{
    QFile input(source);
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
    {
        error = QStringLiteral("%1: %2").arg(destination, input.isOpen() ? output.errorString() : input.errorString());
        return false;
    }
    while (!input.atEnd())
    {
        const QByteArray data = input.read(1024 * 1024);
        if (data.isEmpty() || output.write(data) != data.size())
        {
            error = QStringLiteral("%1: %2").arg(destination, data.isEmpty() ? input.errorString() : output.errorString());
            return false;
        }
    }
    if (!output.commit())
    {
        error = QStringLiteral("%1: %2").arg(destination, output.errorString());
        return false;
    }
    return true;
}

void ShowError(const QString& message)
{
    MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(message.utf16()), L"PKG Extractor Update",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
}

bool CanUpdateWindowsInstallation(const QString& executable, QString& error)
{
    const QFileInfo info(executable);
    const QString root = info.absolutePath();
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const auto isWithin = [&canonicalRoot](const QString& location)
    {
        if (location.isEmpty())
            return false;
        const QString path = QFileInfo(location).canonicalFilePath();
        return !path.isEmpty() && (canonicalRoot.compare(path, Qt::CaseInsensitive) == 0 ||
            canonicalRoot.startsWith(path + QLatin1Char('/'), Qt::CaseInsensitive));
    };
    const DWORD rootAttributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(root.utf16()));
    if (info.fileName().compare(QStringLiteral("pkg_extractor.exe"), Qt::CaseInsensitive) != 0 ||
        !info.isFile() || rootAttributes == INVALID_FILE_ATTRIBUTES ||
        canonicalRoot.isEmpty() || isWithin(qEnvironmentVariable("ProgramFiles")) ||
        isWithin(qEnvironmentVariable("ProgramFiles(x86)")) || isWithin(qEnvironmentVariable("WINDIR")) ||
        (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !IsSafePath(root, info.fileName()) ||
        !QFileInfo::exists(root + QStringLiteral("/Qt6Core.dll")) ||
        !QFileInfo::exists(root + QStringLiteral("/platforms/qwindows.dll")))
    {
        error = QStringLiteral("This installation cannot be updated automatically.");
        return false;
    }
    QTemporaryFile probe(root + QStringLiteral("/.pkg-update-write-XXXXXX"));
    if (!probe.open())
    {
        error = QStringLiteral("The installation directory is not writable.");
        return false;
    }
    error.clear();
    return true;
}

bool StartWindowsUpdate(const QString& stagingDir, const QStringList& files,
                        const QString& executable, QString& error)
{
    const QString root = QFileInfo(executable).absolutePath();
    if (!CanUpdateWindowsInstallation(executable, error))
        return false;
    const QString helperDir = stagingDir + QStringLiteral("/helper");
    if (!QDir().mkpath(helperDir) || !QFile::copy(executable, helperDir + QStringLiteral("/pkg_extractor.exe")))
    {
        error = QStringLiteral("Could not prepare the update helper.");
        return false;
    }
    const QFileInfoList dlls = QDir(root).entryInfoList({QStringLiteral("*.dll")}, QDir::Files);
    for (const QFileInfo& dll : dlls)
    {
        if (!IsSafePath(root, dll.fileName()) || !QFile::copy(dll.filePath(), helperDir + QLatin1Char('/') + dll.fileName()))
        {
            error = QStringLiteral("Could not prepare the update helper.");
            return false;
        }
    }
    QJsonArray array;
    for (const QString& file : files)
        array.append(file);
    QSaveFile manifest(stagingDir + QStringLiteral("/manifest.json"));
    const QByteArray manifestData = QJsonDocument(array).toJson(QJsonDocument::Compact);
    if (!manifest.open(QIODevice::WriteOnly) ||
        manifest.write(manifestData) != manifestData.size() || !manifest.commit())
    {
        error = QStringLiteral("Could not prepare the update helper.");
        return false;
    }
    const QStringList args{QStringLiteral("--windows-update-helper"),
                           QString::number(QCoreApplication::applicationPid()), stagingDir, executable};
    if (!QProcess::startDetached(helperDir + QStringLiteral("/pkg_extractor.exe"), args, helperDir))
    {
        error = QStringLiteral("Could not start the update helper.");
        return false;
    }
    for (int i = 0; i < 200; ++i)
    {
        if (QFileInfo::exists(stagingDir + QStringLiteral("/helper-ready")))
        {
            error.clear();
            return true;
        }
        QThread::msleep(50);
    }
    error = QStringLiteral("The update helper did not start.");
    return false;
}

int RunWindowsUpdateHelper(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 5)
        return 2;
    bool ok = false;
    const DWORD pid = args[2].toULong(&ok);
    if (!ok || !pid)
        return 2;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
    {
        ShowError(QStringLiteral("Could not wait for the application to exit. No update was installed."));
        return 1;
    }
    QFile ready(args[3] + QStringLiteral("/helper-ready"));
    if (!ready.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        CloseHandle(process);
        ShowError(QStringLiteral("Could not signal that the update helper is ready."));
        return 1;
    }
    ready.close();
    const DWORD result = WaitForSingleObject(process, 120000);
    CloseHandle(process);
    if (result != WAIT_OBJECT_0)
    {
        ShowError(QStringLiteral("The application did not exit. No update was installed."));
        return 1;
    }
    const QString stage = args[3];
    const QString executable = args[4];
    const QString root = QFileInfo(executable).absolutePath();
    QLockFile lock(root + QStringLiteral("/.pkg-update.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock())
    {
        ShowError(QStringLiteral("Another update is already running, or the installation is not writable."));
        return 1;
    }
    QFile manifest(stage + QStringLiteral("/manifest.json"));
    if (!manifest.open(QIODevice::ReadOnly))
    {
        ShowError(QStringLiteral("Could not read the update manifest."));
        return 1;
    }
    const QJsonDocument document = QJsonDocument::fromJson(manifest.readAll());
    if (!document.isArray() || document.array().isEmpty())
    {
        ShowError(QStringLiteral("The update manifest is invalid."));
        return 1;
    }
    QTemporaryDir backup(root + QStringLiteral("/.pkg-update-backup-XXXXXX"));
    if (!backup.isValid())
    {
        ShowError(QStringLiteral("Could not create an update backup in %1.").arg(root));
        return 1;
    }
    backup.setAutoRemove(false);
    const QString payload = stage + QStringLiteral("/pkg-extractor-windows");
    QStringList files;
    QStringList existing;
    QString error;
    for (const auto& value : document.array())
    {
        const QString file = value.toString();
        const QString source = payload + QLatin1Char('/') + file;
        const QString target = root + QLatin1Char('/') + file;
        if (file.isEmpty() || !IsSafePath(root, file) || !IsSafePath(payload, file) ||
            !QFileInfo(source).isFile() || (QFileInfo::exists(target) && !QFileInfo(target).isFile()))
        {
            error = QStringLiteral("Unsafe or invalid update file: %1").arg(file);
            break;
        }
        files.append(file);
        if (QFileInfo::exists(target))
        {
            existing.append(file);
            const QString saved = backup.path() + QLatin1Char('/') + file;
            if (!QDir().mkpath(QFileInfo(saved).absolutePath()) || !QFile::copy(target, saved))
            {
                error = QStringLiteral("Could not back up %1.").arg(target);
                break;
            }
        }
    }
    QStringList installed;
    if (error.isEmpty())
    {
        for (const QString& file : files)
        {
            const QString target = root + QLatin1Char('/') + file;
            if (!IsSafePath(root, file) || !QDir().mkpath(QFileInfo(target).absolutePath()) ||
                !CopyAtomically(payload + QLatin1Char('/') + file, target, error))
            {
                if (error.isEmpty())
                    error = QStringLiteral("Could not prepare %1.").arg(target);
                break;
            }
            installed.append(file);
        }
    }
    if (!error.isEmpty())
    {
        QStringList failed;
        for (auto it = installed.crbegin(); it != installed.crend(); ++it)
        {
            const QString target = root + QLatin1Char('/') + *it;
            QString rollbackError;
            if (existing.contains(*it) ? !CopyAtomically(backup.path() + QLatin1Char('/') + *it, target, rollbackError)
                                       : !QFile::remove(target))
                failed.append(*it);
        }
        QString message = QStringLiteral("The update failed: %1\nBackup: %2").arg(error, backup.path());
        if (!failed.isEmpty())
            message += QStringLiteral("\nCould not restore: %1. Restore these files from the backup.").arg(failed.join(QStringLiteral(", ")));
        else
            message += QStringLiteral("\nChanges were rolled back.");
        ShowError(message);
        return 1;
    }
    if (!QProcess::startDetached(executable, {}, root))
    {
        ShowError(QStringLiteral("The update was installed, but the application could not be restarted. Backup: %1").arg(backup.path()));
        return 1;
    }
    return 0;
}
