#pragma once

#include "app/release_info.h"

bool CanUpdateMacInstallation(const QString& executable, QString& bundle, QString& error);
bool StageMacUpdate(const QByteArray& data, const MacReleaseAsset& asset, const QString& stage,
                    QString& error);
bool ReplaceMacBundle(const QString& stage, const QString& bundle, QString& error);
bool StartMacUpdate(const QString& stage, const QString& bundle, QString& error);
int RunMacUpdateHelper(int argc, char** argv);
