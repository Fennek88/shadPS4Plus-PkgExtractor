#pragma once

#include <QString>
#include <QVersionNumber>

QString UpdateSettingsPath();
bool ReadSkippedVersion(const QString& path, QVersionNumber& version, QString& error);
bool WriteSkippedVersion(const QString& path, const QVersionNumber& version, QString& error);
bool ClearMatchingSkippedVersion(const QString& path, const QVersionNumber& version, QString& error);
bool ShouldNotifyUpdate(const QVersionNumber& current, const QVersionNumber& latest,
                        const QVersionNumber& skipped, bool automatic);
