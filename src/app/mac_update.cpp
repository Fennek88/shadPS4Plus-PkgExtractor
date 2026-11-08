#include "app/mac_update.h"
#include "app/mac_update_zip.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>

#include <cerrno>
#include <cstring>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool Run(const QString& program, const QStringList& arguments, QString& error)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10000) || !process.waitForFinished(180000))
    {
        process.kill();
        process.waitForFinished();
        error = QStringLiteral("%1: %2").arg(program, process.errorString());
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        error = QStringLiteral("%1: %2").arg(program, QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        return false;
    }
    return true;
}

void ShowError(const QString& text)
{
    QProcess process;
    process.start(QStringLiteral("/usr/bin/osascript"),
                  {QStringLiteral("-e"),
                   QStringLiteral("on run argv\n display dialog (item 1 of argv) with title \"PKG Extractor Update\" buttons {\"OK\"} default button \"OK\" with icon stop\nend run"),
                   text});
    if (!process.waitForStarted(10000) || !process.waitForFinished(-1) || process.exitCode() != 0)
        qWarning("Could not display macOS update error: %s", qPrintable(text));
}
}

bool CanUpdateMacInstallation(const QString& executable, QString& bundle, QString& error)
{
    const QFileInfo exe(executable);
    const QDir macos(exe.absolutePath());
    const QString candidate = QDir::cleanPath(macos.filePath(QStringLiteral("../..")));
    const QFileInfo app(candidate);
    const QString parent = app.absolutePath();
    if (exe.fileName() != QStringLiteral("pkg_extractor") ||
        macos.dirName() != QStringLiteral("MacOS") ||
        QFileInfo(QDir::cleanPath(macos.filePath(QStringLiteral("..")))).fileName() != QStringLiteral("Contents") ||
        !app.fileName().endsWith(QStringLiteral(".app"), Qt::CaseInsensitive) ||
        !app.isDir() || app.isSymLink() || !exe.isFile() || exe.isSymLink() ||
        app.canonicalFilePath() != candidate ||
        candidate.startsWith(QStringLiteral("/Volumes/")) ||
        candidate.startsWith(QStringLiteral("/System/")) ||
        candidate.contains(QStringLiteral("/AppTranslocation/")) ||
        !QFileInfo::exists(candidate + QStringLiteral("/Contents/Frameworks/QtCore.framework")) ||
        !QFileInfo::exists(candidate + QStringLiteral("/Contents/PlugIns/platforms/libqcocoa.dylib")))
    {
        error = QStringLiteral("This installation cannot be updated automatically.");
        return false;
    }
    QTemporaryFile probe(parent + QStringLiteral("/.pkg-update-write-XXXXXX"));
    if (!probe.open())
    {
        error = QStringLiteral("The installation directory is not writable.");
        return false;
    }
    bundle = candidate;
    error.clear();
    return true;
}

bool StageMacUpdate(const QByteArray& data, const MacReleaseAsset& asset, const QString& stage,
                    QString& error)
{
    if (data.size() != asset.size || asset.sha256.size() != 32 ||
        QCryptographicHash::hash(data, QCryptographicHash::Sha256) != asset.sha256)
    {
        error = QStringLiteral("The downloaded update failed integrity or architecture validation.");
        return false;
    }
    const QString payload = stage + QStringLiteral("/payload.app");
    if (asset.format == MacReleaseAsset::Format::Zip)
    {
        if (!ExtractMacUpdateZip(data, stage, error))
            return false;
    }
    else
    {
        QFile image(stage + QStringLiteral("/update.dmg"));
        if (!image.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            image.write(data) != data.size() || !image.flush())
        {
            error = QStringLiteral("Could not create an update staging directory.");
            return false;
        }
        image.close();
        const QString mount = stage + QStringLiteral("/mount");
        if (!QDir().mkdir(mount))
        {
            error = QStringLiteral("Could not create an update staging directory.");
            return false;
        }
        if (!Run(QStringLiteral("/usr/bin/hdiutil"),
                 {QStringLiteral("attach"), image.fileName(), QStringLiteral("-readonly"),
                  QStringLiteral("-nobrowse"), QStringLiteral("-noautoopen"), QStringLiteral("-mountpoint"), mount},
                 error))
            return false;
        const QString source = mount + QStringLiteral("/pkg_extractor.app");
        bool ok = QFileInfo(source).isDir() && !QFileInfo(source).isSymLink();
        if (!ok)
            error = QStringLiteral("This installation cannot be updated automatically.");
        if (ok)
            ok = Run(QStringLiteral("/usr/bin/ditto"), {source, payload}, error);
        QString detachError;
        if (!Run(QStringLiteral("/usr/bin/hdiutil"), {QStringLiteral("detach"), mount}, detachError))
        {
            error = detachError;
            return false;
        }
        if (!ok)
            return false;
    }
    const QString exe = payload + QStringLiteral("/Contents/MacOS/pkg_extractor");
    if (!QFileInfo(exe).isFile() || QFileInfo(exe).isSymLink() ||
        !QFileInfo::exists(payload + QStringLiteral("/Contents/PlugIns/platforms/libqcocoa.dylib")))
    {
        error = QStringLiteral("The downloaded update failed integrity or architecture validation.");
        return false;
    }
    if (!Run(QStringLiteral("/usr/bin/codesign"),
             {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), payload}, error) ||
        !Run(QStringLiteral("/usr/bin/lipo"),
             {exe, QStringLiteral("-verify_arch"), QStringLiteral("arm64")}, error))
        return false;
    if (::chmod(QFile::encodeName(exe).constData(), 0755) != 0)
    {
        error = QStringLiteral("Could not prepare the update helper.");
        return false;
    }
    error.clear();
    return true;
}

bool StartMacUpdate(const QString& stage, const QString& bundle, QString& error)
{
    const QString helper = stage + QStringLiteral("/helper.app");
    if (!Run(QStringLiteral("/usr/bin/ditto"), {bundle, helper}, error))
        return false;
    struct stat identity {};
    if (lstat(QFile::encodeName(bundle).constData(), &identity) != 0)
    {
        error = QStringLiteral("This installation cannot be updated automatically.");
        return false;
    }
    const QStringList args{QStringLiteral("--mac-update-helper"), QString::number(QCoreApplication::applicationPid()),
                           stage, bundle, QString::number(static_cast<qulonglong>(identity.st_dev)),
                           QString::number(static_cast<qulonglong>(identity.st_ino))};
    if (!QProcess::startDetached(helper + QStringLiteral("/Contents/MacOS/pkg_extractor"), args, stage))
    {
        error = QStringLiteral("Could not start the update helper.");
        return false;
    }
    for (int i = 0; i < 200; ++i)
    {
        if (QFileInfo::exists(stage + QStringLiteral("/helper-ready")))
        {
            QSaveFile go(stage + QStringLiteral("/helper-go"));
            if (!go.open(QIODevice::WriteOnly) || go.write("go", 2) != 2 || !go.commit())
            {
                error = QStringLiteral("Could not start the update helper.");
                return false;
            }
            error.clear();
            return true;
        }
        QThread::msleep(50);
    }
    error = QStringLiteral("The update helper did not start.");
    return false;
}

bool ReplaceMacBundle(const QString& stage, const QString& bundle, QString& error)
{
    const QString backup = stage + QStringLiteral("/previous.app");
    const QString payload = stage + QStringLiteral("/payload.app");
    if (!QDir().rename(bundle, backup))
    {
        error = QStringLiteral("Could not back up the application. No update was installed.");
        return false;
    }
    if (!QDir().rename(payload, bundle))
    {
        const bool restored = QDir().rename(backup, bundle);
        error = restored ? QStringLiteral("The update failed; the original app was restored.")
                         : QStringLiteral("The update failed. Restore the original app from: %1").arg(backup);
        return false;
    }
    error.clear();
    return true;
}

int RunMacUpdateHelper(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 7)
        return 2;
    bool pidOk = false, devOk = false, inoOk = false;
    const int pid = args[2].toInt(&pidOk);
    const qulonglong dev = args[5].toULongLong(&devOk);
    const qulonglong ino = args[6].toULongLong(&inoOk);
    if (!pidOk || pid <= 0 || !devOk || !inoOk)
        return 2;
    const int queue = kqueue();
    struct kevent event {};
    EV_SET(&event, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (queue < 0 || kevent(queue, &event, 1, nullptr, 0, nullptr) < 0)
    {
        if (queue >= 0) ::close(queue);
        ShowError(QStringLiteral("Could not wait for the application to exit. No update was installed."));
        return 1;
    }
    QFile ready(args[3] + QStringLiteral("/helper-ready"));
    if (!ready.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        ::close(queue);
        ShowError(QStringLiteral("Could not signal that the update helper is ready."));
        return 1;
    }
    ready.close();
    bool authorized = false;
    for (int i = 0; i < 200; ++i)
    {
        if (QFileInfo::exists(args[3] + QStringLiteral("/helper-go")))
        {
            authorized = true;
            break;
        }
        QThread::msleep(50);
    }
    if (!authorized)
    {
        ::close(queue);
        ShowError(QStringLiteral("The update was not authorized. No update was installed."));
        return 1;
    }
    const struct timespec timeout {120, 0};
    const int result = kevent(queue, nullptr, 0, &event, 1, &timeout);
    ::close(queue);
    if (result != 1)
    {
        ShowError(QStringLiteral("The application did not exit. No update was installed."));
        return 1;
    }
    const QString stage = args[3], bundle = args[4];
    QLockFile lock(QFileInfo(bundle).absolutePath() + QStringLiteral("/.pkg-update.lock"));
    lock.setStaleLockTime(0);
    struct stat identity {};
    if (!lock.tryLock() || lstat(QFile::encodeName(bundle).constData(), &identity) != 0 ||
        static_cast<qulonglong>(identity.st_dev) != dev || static_cast<qulonglong>(identity.st_ino) != ino)
    {
        ShowError(QStringLiteral("The installation changed or another update is running. No update was installed."));
        return 1;
    }
    const QString backup = stage + QStringLiteral("/previous.app");
    QString replaceError;
    if (!ReplaceMacBundle(stage, bundle, replaceError))
    {
        ShowError(replaceError);
        return 1;
    }
    QString launchError;
    if (!Run(QStringLiteral("/usr/bin/open"), {QStringLiteral("-n"), bundle}, launchError))
    {
        ShowError(QStringLiteral("The update was installed, but could not be restarted. Backup: %1\n%2")
                      .arg(backup, launchError));
        return 1;
    }
    return 0;
}
