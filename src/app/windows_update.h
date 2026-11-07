#pragma once

#include <QString>
#include <QStringList>

bool CanUpdateWindowsInstallation(const QString& executable, QString& error);
bool StartWindowsUpdate(const QString& stagingDir, const QStringList& files,
                        const QString& executable, QString& error);
int RunWindowsUpdateHelper(int argc, char** argv);
