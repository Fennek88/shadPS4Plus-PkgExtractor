#pragma once

#include <filesystem>

#include <QDir>
#include <QString>
#include <QTemporaryFile>

// Convert through the platform's native path encoding to preserve non-ASCII filenames.
inline std::filesystem::path ToPath(const QString& value)
{
#ifdef _WIN32
    return value.toStdWString();
#else
    return value.toStdString();
#endif
}

// Convert a filesystem path to a QString using the matching platform representation.
inline QString ToQString(const std::filesystem::path& value)
{
#ifdef _WIN32
    return QString::fromStdWString(value.wstring());
#else
    return QString::fromStdString(value.string());
#endif
}

inline bool CanWriteToDirectory(const QString& directory, QString& error)
{
    QTemporaryFile writeTest(QDir(directory).filePath(QStringLiteral(".pkg-extractor-write-test-XXXXXX")));
    if (!writeTest.open() || writeTest.write("x", 1) != 1 || !writeTest.flush())
    {
        error = writeTest.errorString();
        return false;
    }
    return true;
}
