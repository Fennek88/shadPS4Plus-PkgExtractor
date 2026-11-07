#pragma once

#include "app/release_info.h"

// Atomically replace the executable only after verifying the downloaded bytes.
bool InstallLinuxUpdate(const QString& path, const QByteArray& data, const LinuxReleaseAsset& asset,
                        QString& backupPath, QString& error);
