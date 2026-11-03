#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <utility>

#include <QString>
#include <QThread>

#include "app/gui_utils.h"
#include "app/package_extraction.h"
#include "common/translations/translations.h"
#include "core/file_format/pkg.h"

class ExtractionThread final : public QThread
{
    Q_OBJECT

public:
    ExtractionThread(std::filesystem::path file, std::filesystem::path outputBase, QObject* parent)
        : QThread(parent), file_(std::move(file)), outputBase_(std::move(outputBase))
    {
    }

signals:
    void progressUpdated(int percent, const QString& status);
    void logMessage(const QString& message);
    void subpackageProgress(const QString& name, int percent);
    void finalizing();
    void completed(bool success, bool cancelled, const QString& message, const QString& outputDirectory);

public:
    // The GUI requests cancellation atomically; the worker observes it between extracted files.
    void RequestCancel()
    {
        cancelRequested_.store(true, std::memory_order_relaxed);
    }

protected:
    // Run package parsing and extraction off the UI thread, reporting state through Qt signals.
    void run() override
    {
        emit progressUpdated(0, Translate(QStringLiteral("Preparing package...")));
        emit logMessage(Translate(QStringLiteral("Preparing package: %1")).arg(ToQString(file_)));

        PKG pkg;
        PackageExtraction::PackageInfo info;
        std::string error;
        if (!PackageExtraction::PreparePackage(file_, outputBase_, pkg, info, error))
        {
            emit logMessage(
                Translate(QStringLiteral("Cannot open PKG file: %1")).arg(Translate(QString::fromStdString(error))));
            emit completed(false, false, Translate(QString::fromStdString(error)), QString());
            return;
        }

        emit logMessage(Translate(QStringLiteral("%1 is a valid PKG")).arg(ToQString(file_)));
        if (info.type == 102)
        {
            emit logMessage(Translate(QStringLiteral("pkg is a game update")));
        }
        else if (info.type == 103)
        {
            emit logMessage(Translate(QStringLiteral("pkg is a dlc")));
        }
        else
        {
            emit logMessage(Translate(QStringLiteral("pkg is a base game")));
        }
        emit logMessage(
            Translate(QStringLiteral("Extracting pkg to %1")).arg(ToQString(info.outputPath)));

        bool cancelled = false;
        const bool success = PackageExtraction::ExtractPackage(
            pkg, file_, info, outputBase_,
            [this](int current, int total, int percent)
            {
                emit progressUpdated(
                    percent, Translate(QStringLiteral("Extracting file %1 of %2 (%3%)"))
                                 .arg(current)
                                 .arg(total)
                                 .arg(percent));
            },
            [this](const std::string& name, int percent)
            {
                emit subpackageProgress(QString::fromStdString(name), percent);
            },
            [this] { emit finalizing(); },
            [this](int percent)
            {
                const auto status =
                    Translate(QStringLiteral("Copying extracted files: %1%")).arg(percent);
                emit progressUpdated(percent, status);
                emit logMessage(status);
            },
            &cancelRequested_,
            true,
            cancelled,
            error);
        if (success)
        {
            emit progressUpdated(100, Translate(QStringLiteral("Extraction complete.")));
            emit logMessage(Translate(QStringLiteral("THE END %1")).arg(ToQString(file_)));
            emit completed(true, false,
                           Translate(QStringLiteral("Extracted to %1")).arg(ToQString(info.outputPath)),
                           ToQString(info.outputPath));
        }
        else if (cancelled)
        {
            emit logMessage(Translate(QStringLiteral("Extraction cancelled.")));
            emit completed(false, true,
                           Translate(QStringLiteral("Extraction cancelled. Unpacked files were deleted.")),
                           QString());
        }
        else
        {
            emit logMessage(Translate(QStringLiteral("Cannot extract PKG file: %1"))
                .arg(Translate(QString::fromStdString(error))));
            emit completed(false, false, Translate(QString::fromStdString(error)), QString());
        }
    }

private:
    std::filesystem::path file_;
    std::filesystem::path outputBase_;
    std::atomic_bool cancelRequested_{false};
};
