#include "app/release_info.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

bool ParseReleaseInfo(const QByteArray& response, ReleaseInfo& release, QString& error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("GitHub returned an invalid release response.");
        return false;
    }

    const QJsonObject object = document.object();
    const auto tagValue = object.value(QStringLiteral("tag_name"));
    const auto draft = object.value(QStringLiteral("draft"));
    const auto prerelease = object.value(QStringLiteral("prerelease"));
    if (!tagValue.isString() || !draft.isBool() || !prerelease.isBool() ||
        draft.toBool() || prerelease.toBool())
    {
        error = QStringLiteral("GitHub did not return a stable release.");
        return false;
    }

    const QString tag = tagValue.toString();
    static const QRegularExpression pattern(
        QStringLiteral("^(?:Pkg-Extractor-)?v?([0-9]+\\.[0-9]+(?:\\.[0-9]+)?)$"));
    const auto match = pattern.match(tag);
    int suffixIndex = 0;
    const QVersionNumber version = QVersionNumber::fromString(match.captured(1), &suffixIndex);
    if (!match.hasMatch() || version.isNull() || suffixIndex != match.capturedLength(1))
    {
        error = QStringLiteral("The latest release has an unsupported version tag.");
        return false;
    }

    release = {tag, version, QUrl(QStringLiteral(
        "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/tag/") + tag)};
    error.clear();
    return true;
}

bool FindLinuxReleaseAsset(const QByteArray& response, const ReleaseInfo& release,
                           LinuxReleaseAsset& asset, QString& error)
{
    const QJsonArray assets = QJsonDocument::fromJson(response).object().value(QStringLiteral("assets")).toArray();
    const QString prefix = QStringLiteral("pkg-extractor-linux-");
    const QString numeric = release.version.toString();
    const QString elfName = prefix + release.tag;
    const QStringList zipNames{prefix + release.tag + QStringLiteral(".zip"),
                               prefix + numeric + QStringLiteral(".zip"),
                               prefix + QStringLiteral("v") + numeric + QStringLiteral(".zip")};
    int rank = 0;
    int matches = 0;
    QJsonObject selected;
    for (const auto& value : assets)
    {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        const int entryRank = name == elfName ? 2 : zipNames.contains(name) ? 1 : 0;
        if (entryRank == 0 || entryRank < rank)
            continue;
        if (entryRank > rank)
        {
            rank = entryRank;
            matches = 0;
        }
        ++matches;
        selected = entry;
    }
    const QString name = selected.value(QStringLiteral("name")).toString();
    const QString expectedUrl = QStringLiteral(
        "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/") +
        release.tag + QLatin1Char('/') + name;
    const QString digest = selected.value(QStringLiteral("digest")).toString();
    static const QRegularExpression digestPattern(QStringLiteral("^sha256:([a-fA-F0-9]{64})$"));
    const auto match = digestPattern.match(digest);
    const double size = selected.value(QStringLiteral("size")).toDouble(-1);
    if (matches != 1 || selected.value(QStringLiteral("state")).toString() != QStringLiteral("uploaded") ||
        selected.value(QStringLiteral("browser_download_url")).toString() != expectedUrl ||
        !match.hasMatch() || size < 20 || size > 128 * 1024 * 1024 || size != static_cast<qint64>(size))
    {
        error = QStringLiteral("The Linux release asset is missing or invalid.");
        return false;
    }
    asset = {QUrl(expectedUrl), QByteArray::fromHex(match.captured(1).toLatin1()),
             static_cast<qint64>(size),
             rank == 2 ? LinuxReleaseAsset::Format::Elf : LinuxReleaseAsset::Format::Zip};
    error.clear();
    return true;
}

bool FindWindowsReleaseAsset(const QByteArray& response, const ReleaseInfo& release,
                             WindowsReleaseAsset& asset, QString& error)
{
    const QJsonArray assets = QJsonDocument::fromJson(response).object().value(QStringLiteral("assets")).toArray();
    const QString prefix = QStringLiteral("pkg-extractor-windows-");
    const QString numeric = release.version.toString();
    const QStringList names{prefix + release.tag + QStringLiteral(".zip"),
                            prefix + numeric + QStringLiteral(".zip"),
                            prefix + QStringLiteral("v") + numeric + QStringLiteral(".zip")};
    int matches = 0;
    QJsonObject selected;
    for (const auto& value : assets)
    {
        const QJsonObject entry = value.toObject();
        if (!names.contains(entry.value(QStringLiteral("name")).toString()))
            continue;
        ++matches;
        selected = entry;
    }
    const QString name = selected.value(QStringLiteral("name")).toString();
    const QString expectedUrl = QStringLiteral(
        "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/") +
        release.tag + QLatin1Char('/') + name;
    const QString digest = selected.value(QStringLiteral("digest")).toString();
    static const QRegularExpression digestPattern(QStringLiteral("^sha256:([a-fA-F0-9]{64})$"));
    const auto match = digestPattern.match(digest);
    const double size = selected.value(QStringLiteral("size")).toDouble(-1);
    if (matches != 1 || selected.value(QStringLiteral("state")).toString() != QStringLiteral("uploaded") ||
        selected.value(QStringLiteral("browser_download_url")).toString() != expectedUrl ||
        !match.hasMatch() || size < 22 || size > 128 * 1024 * 1024 || size != static_cast<qint64>(size))
    {
        error = QStringLiteral("The Windows release asset is missing or invalid.");
        return false;
    }
    asset = {QUrl(expectedUrl), QByteArray::fromHex(match.captured(1).toLatin1()),
             static_cast<qint64>(size)};
    error.clear();
    return true;
}

bool FindMacReleaseAsset(const QByteArray& response, const ReleaseInfo& release,
                         MacReleaseAsset& asset, QString& error)
{
    const QJsonArray assets = QJsonDocument::fromJson(response).object().value(QStringLiteral("assets")).toArray();
    const QString prefix = QStringLiteral("pkg-extractor-macos-");
    const QStringList stems{prefix + release.tag, prefix + release.version.toString(),
                            prefix + QStringLiteral("v") + release.version.toString()};
    QJsonObject selected;
    int matches = 0;
    int rank = 0;
    for (const auto& value : assets)
    {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        int entryRank = 0;
        for (const QString& stem : stems)
        {
            if (name == stem + QStringLiteral(".dmg")) entryRank = 2;
            if (name == stem + QStringLiteral(".zip")) entryRank = 1;
        }
        if (entryRank == 0 || entryRank < rank) continue;
        if (entryRank > rank) { rank = entryRank; matches = 0; }
        selected = entry;
        ++matches;
    }
    const QString name = selected.value(QStringLiteral("name")).toString();
    const QString url = QStringLiteral(
        "https://github.com/Fennek88/shadPS4Plus-PkgExtractor/releases/download/") +
        release.tag + QLatin1Char('/') + name;
    static const QRegularExpression digestPattern(QStringLiteral("^sha256:([a-fA-F0-9]{64})$"));
    const auto digest = digestPattern.match(selected.value(QStringLiteral("digest")).toString());
    const double size = selected.value(QStringLiteral("size")).toDouble(-1);
    if (matches != 1 || selected.value(QStringLiteral("state")).toString() != QStringLiteral("uploaded") ||
        selected.value(QStringLiteral("browser_download_url")).toString() != url ||
        !digest.hasMatch() || size < (rank == 2 ? 512 : 22) ||
        size > 128 * 1024 * 1024 || size != static_cast<qint64>(size))
    {
        error = QStringLiteral("The macOS release asset is missing or invalid.");
        return false;
    }
    asset = {QUrl(url), QByteArray::fromHex(digest.captured(1).toLatin1()), static_cast<qint64>(size),
             rank == 2 ? MacReleaseAsset::Format::Dmg : MacReleaseAsset::Format::Zip};
    error.clear();
    return true;
}
