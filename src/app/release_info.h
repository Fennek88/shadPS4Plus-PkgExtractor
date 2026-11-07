#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVersionNumber>

struct ReleaseInfo
{
    QString tag;
    QVersionNumber version;
    QUrl url;
};

struct LinuxReleaseAsset
{
    enum class Format { Elf, Zip };
    QUrl url;
    QByteArray sha256;
    qint64 size = 0;
    Format format = Format::Elf;
};

struct WindowsReleaseAsset
{
    QUrl url;
    QByteArray sha256;
    qint64 size = 0;
};

struct MacReleaseAsset
{
    enum class Format { Dmg, Zip };
    QUrl url;
    QByteArray sha256;
    qint64 size = 0;
    Format format = Format::Dmg;
};

// Accept stable numeric versions in the repository's release-tag format.
bool ParseReleaseInfo(const QByteArray& response, ReleaseInfo& release, QString& error);
bool FindLinuxReleaseAsset(const QByteArray& response, const ReleaseInfo& release,
                           LinuxReleaseAsset& asset, QString& error);
bool FindWindowsReleaseAsset(const QByteArray& response, const ReleaseInfo& release,
                             WindowsReleaseAsset& asset, QString& error);
bool FindMacReleaseAsset(const QByteArray& response, const ReleaseInfo& release,
                         MacReleaseAsset& asset, QString& error);
