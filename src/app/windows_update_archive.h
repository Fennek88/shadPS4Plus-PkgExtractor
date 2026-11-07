#pragma once

#include "app/release_info.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

// Validate a Windows release ZIP and stage its application files in
// <stagingDir>/pkg-extractor-windows. Nothing is written unless the whole archive validates.
// `files` receives the normalized relative payload paths (relative to that directory).
bool StageWindowsUpdate(const QByteArray& data, const WindowsReleaseAsset& asset,
                        const QString& stagingDir, QStringList& files, QString& error);
