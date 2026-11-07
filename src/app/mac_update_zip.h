#pragma once

#include <QByteArray>
#include <QString>

bool ExtractMacUpdateZip(const QByteArray& data, const QString& stage, QString& error);
