#include "app/update_settings.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

QString UpdateSettingsPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        return {};
    return QDir(base).filePath(QStringLiteral("shadPS4Plus-PkgExtractor/update.ini"));
}

bool ReadSkippedVersion(const QString& path, QVersionNumber& version, QString& error)
{
    version = {};
    if (path.isEmpty())
    {
        error = QStringLiteral("Could not read the update configuration.");
        return false;
    }
    QSettings settings(path, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        error = QStringLiteral("Could not read the update configuration.");
        return false;
    }
    const QString value = settings.value(QStringLiteral("updates/skippedVersion")).toString();
    if (!value.isEmpty())
    {
        static const QRegularExpression pattern(QStringLiteral("^[0-9]+\\.[0-9]+(?:\\.[0-9]+)?$"));
        int suffix = 0;
        version = QVersionNumber::fromString(value, &suffix);
        if (!pattern.match(value).hasMatch() || version.isNull() || suffix != value.size())
        {
            version = {};
            error = QStringLiteral("The update configuration contains an invalid skipped version.");
            return false;
        }
    }
    error.clear();
    return true;
}

bool WriteSkippedVersion(const QString& path, const QVersionNumber& version, QString& error)
{
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        error = QStringLiteral("Could not save the update configuration.");
        return false;
    }
    QSettings settings(path, QSettings::IniFormat);
    if (version.isNull())
        settings.remove(QStringLiteral("updates/skippedVersion"));
    else
        settings.setValue(QStringLiteral("updates/skippedVersion"), version.toString());
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        error = QStringLiteral("Could not save the update configuration.");
        return false;
    }
    error.clear();
    return true;
}

bool ClearMatchingSkippedVersion(const QString& path, const QVersionNumber& version, QString& error)
{
    QVersionNumber skipped;
    if (!ReadSkippedVersion(path, skipped, error))
        return false;
    if (!version.isNull() && !skipped.isNull() &&
        QVersionNumber::compare(version.normalized(), skipped.normalized()) == 0)
        return WriteSkippedVersion(path, {}, error);
    error.clear();
    return true;
}

bool ShouldNotifyUpdate(const QVersionNumber& current, const QVersionNumber& latest,
                        const QVersionNumber& skipped, bool automatic)
{
    return QVersionNumber::compare(latest.normalized(), current.normalized()) > 0 &&
        (!automatic || skipped.isNull() ||
         QVersionNumber::compare(latest.normalized(), skipped.normalized()) != 0);
}
