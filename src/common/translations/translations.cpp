#include "translations.h"

#include <QFile>
#include <QLocale>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QString& SelectedLanguage() {
    static QString language;
    return language;
}

// Load the embedded translation JSON once and reuse the parsed object for all lookups.
const QJsonObject& TranslationTable() {
    static const QJsonObject translations = [] {
        QFile file(QStringLiteral(":/i18n/translations.json"));
        if (!file.open(QIODevice::ReadOnly)) {
            return QJsonObject{};
        }

        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) {
            return QJsonObject{};
        }
        return document.object();
    }();
    return translations;
}

} // namespace

// Resolve a key using the system language, then English, and finally the source text.
QString Translate(const QString& key) {
    const QJsonObject& translations = TranslationTable();
    const QString language = SelectedLanguage().isEmpty()
                                 ? SystemLanguage()
                                 : SelectedLanguage();
    const QJsonObject localized = translations.value(language).toObject();
    const QJsonObject english = translations.value(QStringLiteral("en")).toObject();

    const QString translation = localized.value(key).toString();
    if (!translation.isEmpty()) {
        return translation;
    }
    const QString fallback = english.value(key).toString();
    return fallback.isEmpty() ? key : fallback;
}

void SetLanguage(const QString& language) {
    SelectedLanguage() = language;
}

QStringList AvailableLanguages() {
    QStringList languages = TranslationTable().keys();
    languages.sort();
    return languages;
}

QString SystemLanguage() {
    const QStringList languages = AvailableLanguages();
    const auto findSupportedLanguage = [&languages](const QString& languageTag) {
        const QString language = languageTag.left(2).toLower();
        return languages.contains(language) ? language : QString{};
    };

    for (const QString& languageTag : QLocale::system().uiLanguages()) {
        const QString language = findSupportedLanguage(languageTag);
        if (!language.isEmpty()) {
            return language;
        }
    }

    const QString localeLanguage = findSupportedLanguage(QLocale::system().name());
    if (!localeLanguage.isEmpty()) {
        return localeLanguage;
    }
    return languages.contains(QStringLiteral("en")) ? QStringLiteral("en") : languages.value(0);
}

QString LanguageName(const QString& language) {
    const QString name = TranslationTable().value(language).toObject()
                             .value(QStringLiteral("_languageName")).toString();
    return name.isEmpty() ? language : name;
}
