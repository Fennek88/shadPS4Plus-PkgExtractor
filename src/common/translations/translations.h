#pragma once

#include <QString>
#include <QStringList>

QString Translate(const QString& key);
QStringList AvailableLanguages();
QString LanguageName(const QString& language);
QString SystemLanguage();
void SetLanguage(const QString& language);
