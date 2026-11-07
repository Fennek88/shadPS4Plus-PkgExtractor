#include "app/extractor_window.h"

#include <QApplication>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDebug>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QTimer>
#include <QTemporaryDir>
#include <QSysInfo>
#include <QVersionNumber>
#include <QVBoxLayout>
#include <QWidget>

#include "app/extraction_thread.h"
#include "app/gui_utils.h"
#include "app/linux_update.h"
#include "app/release_info.h"
#include "app/update_settings.h"
#ifdef Q_OS_WIN
#include "app/windows_update.h"
#include "app/windows_update_archive.h"
#endif
#ifdef Q_OS_MACOS
#include "app/mac_update.h"
#endif
#include "common/translations/translations.h"

namespace
{
    class PathLineEdit final : public QLineEdit
    {
    public:
        explicit PathLineEdit(bool expectsDirectory, QWidget* parent = nullptr)
            : QLineEdit(parent), expectsDirectory_(expectsDirectory)
        {
        }

    protected:
        void dragEnterEvent(QDragEnterEvent* event) override
        {
            if (DroppedPathMatchesField(event->mimeData()->urls()))
            {
                event->acceptProposedAction();
                return;
            }
            event->ignore();
        }

        void dropEvent(QDropEvent* event) override
        {
            const auto urls = event->mimeData()->urls();
            if (!DroppedPathMatchesField(urls))
            {
                event->ignore();
                return;
            }
            const QString path = urls.first().toLocalFile();
            if (expectsDirectory_)
            {
                QString error;
                if (!CanWriteToDirectory(path, error))
                {
                    QMessageBox::warning(
                        this, Translate(QStringLiteral("Output directory is not writable")),
                        Translate(QStringLiteral("Cannot write to output directory %1: %2")).arg(path, error));
                    event->ignore();
                    return;
                }
            }
            setText(path);
            event->acceptProposedAction();
        }

    private:
        bool DroppedPathMatchesField(const QList<QUrl>& urls) const
        {
            if (urls.size() != 1 || !urls.first().isLocalFile())
            {
                return false;
            }

            const QFileInfo pathInfo(urls.first().toLocalFile());
            if (!pathInfo.exists())
            {
                return false;
            }
            if (expectsDirectory_)
            {
                return pathInfo.isDir();
            }
            return pathInfo.isFile() && pathInfo.suffix().compare(QStringLiteral("pkg"), Qt::CaseInsensitive) == 0;
        }

        bool expectsDirectory_;
    };

    class ExtractorWindow final : public QMainWindow
    {
    public:
        // Build the picker, progress, status, and action controls and connect their UI events.
        ExtractorWindow()
        {
            setWindowIcon(QIcon(QStringLiteral(":/icons/icon.png")));
            menuBar_ = new QMenuBar(nullptr);
            menuBar_->setNativeMenuBar(true);
            fileMenu_ = menuBar_->addMenu(QString());
            openPkgAction_ = fileMenu_->addAction(QString());
            outputDirectoryAction_ = fileMenu_->addAction(QString());
            fileMenu_->addSeparator();
            exitAction_ = fileMenu_->addAction(QString());
            extrasMenu_ = menuBar_->addMenu(QString());
            languageMenu_ = extrasMenu_->addMenu(QString());
            checkUpdatesAction_ = extrasMenu_->addAction(QString());
            githubAction_ = extrasMenu_->addAction(QString());
            network_ = new QNetworkAccessManager(this);

            auto* languageGroup = new QActionGroup(this);
            languageGroup->setExclusive(true);
            const QStringList languages = AvailableLanguages();
            const QString selectedLanguage = SystemLanguage();
            SetLanguage(selectedLanguage);
            for (const QString& code : languages)
            {
                auto* action = languageMenu_->addAction(LanguageName(code));
                action->setCheckable(true);
                action->setData(code);
                languageGroup->addAction(action);
                if (code == selectedLanguage)
                {
                    action->setChecked(true);
                }
            }
            connect(languageGroup, &QActionGroup::triggered, this, [this](QAction* action)
            {
                SetLanguage(action->data().toString());
                UpdateTexts();
            });

            connect(openPkgAction_, &QAction::triggered, this, [this] { SelectInputFile(); });
            connect(outputDirectoryAction_, &QAction::triggered, this, [this] { SelectOutputDirectory(); });
            connect(exitAction_, &QAction::triggered, this, &QWidget::close);
            connect(checkUpdatesAction_, &QAction::triggered, this, [this] { CheckForUpdates(false); });
            connect(githubAction_, &QAction::triggered, this, [this]
            {
                const QUrl url(QStringLiteral("https://github.com/Fennek88/shadPS4Plus-PkgExtractor"));
                if (!QDesktopServices::openUrl(url))
                {
                    QMessageBox::warning(this, Translate(QStringLiteral("Could not open GitHub")),
                        Translate(QStringLiteral("Could not open the GitHub repository: %1")).arg(url.toString()));
                }
            });

            // Keep translated labels and buttons from changing the window's layout dimensions.
            setMinimumWidth(680);

            auto* centralWidget = new QWidget(this);
            setCentralWidget(centralWidget);
            auto* layout = new QVBoxLayout(centralWidget);
            centralWidget->setLayout(layout);
            setMenuBar(menuBar_);
            auto* inputRow = new QHBoxLayout;
            auto* outputRow = new QHBoxLayout;
            auto* actionRow = new QHBoxLayout;
            inputPath_ = new PathLineEdit(false);
            outputPath_ = new PathLineEdit(true);
            inputButton_ = new QPushButton;
            outputButton_ = new QPushButton;
            inputLabel_ = new QLabel;
            outputLabel_ = new QLabel;
            inputLabel_->setFixedWidth(175);
            outputLabel_->setFixedWidth(175);
            inputLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            outputLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            inputPath_->setMinimumHeight(36);
            outputPath_->setMinimumHeight(36);
            inputButton_->setFixedSize(120, 36);
            outputButton_->setFixedSize(120, 36);
            inputButton_->setEnabled(true);
            outputButton_->setEnabled(true);
            inputRow->addWidget(inputLabel_);
            inputRow->addWidget(inputPath_);
            inputRow->addWidget(inputButton_);
            outputRow->addWidget(outputLabel_);
            outputRow->addWidget(outputPath_);
            outputRow->addWidget(outputButton_);

            progressBar_ = new QProgressBar;
            progressBar_->setRange(0, 100);
            progressBar_->setFixedHeight(30);
            terminal_ = new QPlainTextEdit;
            terminal_->setReadOnly(true);
            terminal_->setMaximumBlockCount(300);
            terminal_->setMinimumHeight(90);
            terminal_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            terminal_->setStyleSheet(
                QStringLiteral("QPlainTextEdit { background-color: #111; color: #ddd; font-family: monospace; }"));
            statusLabel_ = new QLabel;
            executeButton_ = new QPushButton;
            cancelButton_ = new QPushButton;
            executeButton_->setFixedSize(140, 38);
            cancelButton_->setFixedSize(140, 38);
            executeButton_->setEnabled(false);
            cancelButton_->setEnabled(false);

            layout->addLayout(inputRow);
            layout->addLayout(outputRow);
            layout->addWidget(progressBar_);
            layout->addWidget(terminal_, 1);
            layout->addWidget(statusLabel_);
            actionRow->addWidget(executeButton_);
            actionRow->addStretch();
            actionRow->addWidget(cancelButton_);
            layout->addLayout(actionRow);

            connect(inputButton_, &QPushButton::clicked, this, [this] { SelectInputFile(); });
            connect(outputButton_, &QPushButton::clicked, this, [this] { SelectOutputDirectory(); });
            connect(inputPath_, &QLineEdit::textChanged, this, [this] { UpdateExecuteButton(); });
            connect(outputPath_, &QLineEdit::textChanged, this, [this] { UpdateExecuteButton(); });
            connect(executeButton_, &QPushButton::clicked, this, [this] { StartExtraction(); });
            connect(cancelButton_, &QPushButton::clicked, this, [this] { ConfirmCancellation(); });
            UpdateTexts();
            QTimer::singleShot(0, this, [this] { CheckForUpdates(true); });
        }

    protected:
        // Keep the window open while the worker may still be writing files.
        void closeEvent(QCloseEvent* event) override
        {
            if (thread_ != nullptr && thread_->isRunning())
            {
                statusLabel_->setText(Translate(QStringLiteral("Wait for extraction to finish before closing.")));
                event->ignore();
                return;
            }
            event->accept();
        }

    private:
        // Apply the currently selected language to all visible controls and menu actions.
        void UpdateTexts()
        {
            setWindowTitle(Translate(QStringLiteral("ShadPS4Plus PKG Extractor")) +
                           QStringLiteral(" ") + QStringLiteral(PKG_EXTRACTOR_VERSION));
            fileMenu_->setTitle(Translate(QStringLiteral("File")));
            openPkgAction_->setText(Translate(QStringLiteral("Open PKG")));
            outputDirectoryAction_->setText(Translate(QStringLiteral("Output Directory")));
            exitAction_->setText(Translate(QStringLiteral("Exit")));
            extrasMenu_->setTitle(Translate(QStringLiteral("Extras")));
            languageMenu_->setTitle(Translate(QStringLiteral("Language")));
            checkUpdatesAction_->setText(Translate(checkUpdatesAction_->isEnabled()
                ? QStringLiteral("Check for Updates") : QStringLiteral("Checking for Updates...")));
            githubAction_->setText(Translate(QStringLiteral("GitHub Repository")));
            inputPath_->setPlaceholderText(Translate(QStringLiteral("Select a PKG file")));
            outputPath_->setPlaceholderText(Translate(QStringLiteral("Select an output directory")));
            inputLabel_->setText(Translate(QStringLiteral("Input file:")));
            outputLabel_->setText(Translate(QStringLiteral("Output directory:")));
            inputButton_->setText(Translate(QStringLiteral("Browse...")));
            outputButton_->setText(Translate(QStringLiteral("Browse...")));
            executeButton_->setText(Translate(QStringLiteral("Extract")));
            cancelButton_->setText(Translate(QStringLiteral("Cancel")));
            if (thread_ == nullptr)
            {
                statusLabel_->setText(Translate(QStringLiteral("Choose a PKG file and output directory.")));
            }
        }

        void CheckForUpdates(bool automatic)
        {
            checkUpdatesAction_->setEnabled(false);
            checkUpdatesAction_->setText(Translate(QStringLiteral("Checking for Updates...")));
            QNetworkRequest request(QUrl(QStringLiteral(
                "https://api.github.com/repos/Fennek88/shadPS4Plus-PkgExtractor/releases/latest")));
            request.setRawHeader("Accept", "application/vnd.github+json");
            request.setRawHeader("User-Agent", "shadPS4Plus-PkgExtractor");
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
            auto* reply = network_->get(request);
            auto* timer = new QTimer(reply);
            timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, reply, [reply]
            {
                reply->setProperty("updateTimeout", true);
                reply->abort();
            });
            timer->start(15000);
            connect(reply, &QNetworkReply::readyRead, reply, [reply]
            {
                if (reply->bytesAvailable() > 1024 * 1024)
                {
                    reply->setProperty("updateTooLarge", true);
                    reply->abort();
                }
            });
            connect(reply, &QNetworkReply::finished, this, [this, reply, automatic]
            {
                checkUpdatesAction_->setEnabled(true);
                checkUpdatesAction_->setText(Translate(QStringLiteral("Check for Updates")));
                reply->deleteLater();
                QString error;
                if (reply->property("updateTimeout").toBool())
                    error = Translate(QStringLiteral("The update check timed out."));
                else if (reply->property("updateTooLarge").toBool())
                    error = Translate(QStringLiteral("The release response is too large."));
                else if (reply->error() != QNetworkReply::NoError)
                    error = Translate(QStringLiteral("Could not contact GitHub: %1")).arg(reply->errorString());
                else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                    error = Translate(QStringLiteral("GitHub returned HTTP status %1."))
                        .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());

                const QByteArray response = reply->readAll();
                ReleaseInfo release;
                if (error.isEmpty() && !ParseReleaseInfo(response, release, error))
                    error = Translate(error);
                if (!error.isEmpty())
                {
                    if (automatic)
                        qWarning().noquote() << "Automatic update check failed:" << error;
                    else
                        QMessageBox::warning(this, Translate(QStringLiteral("Update check failed")), error);
                    return;
                }

                const QVersionNumber current = QVersionNumber::fromString(QStringLiteral(PKG_EXTRACTOR_VERSION));
                if (QVersionNumber::compare(release.version.normalized(), current.normalized()) <= 0)
                {
                    if (!automatic)
                        QMessageBox::information(this, Translate(QStringLiteral("Check for Updates")),
                            Translate(QStringLiteral("No newer release is available. Installed: %1; latest: %2."))
                                .arg(current.toString(), release.version.toString()));
                    return;
                }
                const QString settingsPath = UpdateSettingsPath();
                if (automatic)
                {
                    QVersionNumber skipped;
                    QString settingsError;
                    if (!ReadSkippedVersion(settingsPath, skipped, settingsError))
                    {
                        qWarning().noquote() << "Automatic update check failed:" << settingsError;
                        return;
                    }
                    if (!ShouldNotifyUpdate(current, release.version, skipped, true))
                        return;
                }
                if (thread_ != nullptr)
                {
                    if (automatic)
                        qWarning().noquote() << "Automatic update notification not shown: extraction is running.";
                    else
                        QMessageBox::information(this, Translate(QStringLiteral("Update unavailable")),
                            Translate(QStringLiteral("Wait for extraction to finish before updating.")));
                    return;
                }
                QString manualPrompt = Translate(QStringLiteral(
                    "Version %1 is available (installed: %2). Open the release page?"))
                    .arg(release.version.toString(), current.toString());
#ifdef Q_OS_LINUX
                LinuxReleaseAsset asset;
                QString assetError;
                const QString executable = QCoreApplication::applicationFilePath();
                const bool managed = qEnvironmentVariableIsSet("APPIMAGE") || qEnvironmentVariableIsSet("SNAP") ||
                    qEnvironmentVariableIsSet("FLATPAK_ID") || QFileInfo::exists(QStringLiteral("/.flatpak-info")) ||
                    executable.startsWith(QStringLiteral("/usr/")) ||
                    executable.startsWith(QStringLiteral("/bin/")) ||
                    executable.startsWith(QStringLiteral("/sbin/")) ||
                    executable.startsWith(QStringLiteral("/opt/"));
                if (!managed && QSysInfo::currentCpuArchitecture() == QStringLiteral("x86_64") &&
                    FindLinuxReleaseAsset(response, release, asset, assetError))
                {
                    QMessageBox dialog(QMessageBox::Question, Translate(QStringLiteral("Update available")),
                        Translate(QStringLiteral("Version %1 is available (installed: %2). Download and install it?"))
                            .arg(release.version.toString(), current.toString()),
                        QMessageBox::Yes | QMessageBox::No, this);
                    auto* skip = dialog.addButton(Translate(QStringLiteral("Skip this version")), QMessageBox::ActionRole);
                    dialog.setDefaultButton(QMessageBox::No);
                    const auto answer = dialog.exec();
                    if (dialog.clickedButton() == skip)
                        SaveSkippedVersion(settingsPath, release.version);
                    else if (answer == QMessageBox::Yes)
                    {
                        ClearSkippedVersionForUpdate(settingsPath, release.version);
                        DownloadUpdate(asset.url, asset.size, release.url, &asset, nullptr, nullptr);
                    }
                    return;
                }
                if (!assetError.isEmpty())
                    manualPrompt += QStringLiteral("\n\n") + Translate(assetError);
#endif
#ifdef Q_OS_WIN
                WindowsReleaseAsset windowsAsset;
                QString windowsError;
                if (QSysInfo::currentCpuArchitecture() == QStringLiteral("x86_64") &&
                    CanUpdateWindowsInstallation(QCoreApplication::applicationFilePath(), windowsError) &&
                    FindWindowsReleaseAsset(response, release, windowsAsset, windowsError))
                {
                    QMessageBox dialog(QMessageBox::Question, Translate(QStringLiteral("Update available")),
                        Translate(QStringLiteral("Version %1 is available (installed: %2). Download and install it?"))
                            .arg(release.version.toString(), current.toString()),
                        QMessageBox::Yes | QMessageBox::No, this);
                    auto* skip = dialog.addButton(Translate(QStringLiteral("Skip this version")), QMessageBox::ActionRole);
                    dialog.setDefaultButton(QMessageBox::No);
                    const auto answer = dialog.exec();
                    if (dialog.clickedButton() == skip)
                        SaveSkippedVersion(settingsPath, release.version);
                    else if (answer == QMessageBox::Yes)
                    {
                        ClearSkippedVersionForUpdate(settingsPath, release.version);
                        DownloadUpdate(windowsAsset.url, windowsAsset.size, release.url, nullptr, &windowsAsset, nullptr);
                    }
                    return;
                }
                if (!windowsError.isEmpty())
                    manualPrompt += QStringLiteral("\n\n") + Translate(windowsError);
#endif
#ifdef Q_OS_MACOS
                MacReleaseAsset macAsset;
                QString macError;
                QString bundle;
                if (QSysInfo::currentCpuArchitecture() == QStringLiteral("arm64") &&
                    CanUpdateMacInstallation(QCoreApplication::applicationFilePath(), bundle, macError) &&
                    FindMacReleaseAsset(response, release, macAsset, macError))
                {
                    QMessageBox dialog(QMessageBox::Question, Translate(QStringLiteral("Update available")),
                        Translate(QStringLiteral("Version %1 is available (installed: %2). Download and install it?"))
                            .arg(release.version.toString(), current.toString()),
                        QMessageBox::Yes | QMessageBox::No, this);
                    auto* skip = dialog.addButton(Translate(QStringLiteral("Skip this version")), QMessageBox::ActionRole);
                    dialog.setDefaultButton(QMessageBox::No);
                    const auto answer = dialog.exec();
                    if (dialog.clickedButton() == skip)
                        SaveSkippedVersion(settingsPath, release.version);
                    else if (answer == QMessageBox::Yes)
                    {
                        ClearSkippedVersionForUpdate(settingsPath, release.version);
                        DownloadUpdate(macAsset.url, macAsset.size, release.url, nullptr, nullptr, &macAsset);
                    }
                    return;
                }
                if (!macError.isEmpty())
                    manualPrompt += QStringLiteral("\n\n") + Translate(macError);
#endif
                QMessageBox dialog(QMessageBox::Question, Translate(QStringLiteral("Update available")),
                                   manualPrompt, QMessageBox::Yes | QMessageBox::No, this);
                auto* skip = dialog.addButton(Translate(QStringLiteral("Skip this version")), QMessageBox::ActionRole);
                dialog.setDefaultButton(QMessageBox::No);
                const auto answer = dialog.exec();
                if (dialog.clickedButton() == skip)
                    SaveSkippedVersion(settingsPath, release.version);
                else if (answer == QMessageBox::Yes && !QDesktopServices::openUrl(release.url))
                {
                    QMessageBox::warning(this, Translate(QStringLiteral("Could not open release page")),
                        Translate(QStringLiteral("Could not open the release page: %1")).arg(release.url.toString()));
                }
            });
        }

        void SaveSkippedVersion(const QString& path, const QVersionNumber& version)
        {
            QString error;
            if (!WriteSkippedVersion(path, version, error))
                QMessageBox::warning(this, Translate(QStringLiteral("Update check failed")), Translate(error));
        }

        void ClearSkippedVersionForUpdate(const QString& path, const QVersionNumber& version)
        {
            QString error;
            if (!ClearMatchingSkippedVersion(path, version, error))
                QMessageBox::warning(this, Translate(QStringLiteral("Update check failed")), Translate(error));
        }

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        void DownloadUpdate(const QUrl& url, qint64 size, const QUrl& releaseUrl,
                            const LinuxReleaseAsset* linuxAsset, const WindowsReleaseAsset* windowsAsset,
                            const MacReleaseAsset* macAsset)
        {
            QNetworkRequest request(url);
            request.setRawHeader("User-Agent", "shadPS4Plus-PkgExtractor");
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
            auto* reply = network_->get(request);
            QProgressDialog progress(Translate(QStringLiteral("Downloading update...")),
                                     Translate(QStringLiteral("Cancel")), 0, 100, this);
            progress.setWindowModality(Qt::WindowModal);
            progress.setMinimumDuration(0);
            progress.setAutoClose(false);
            progress.setAutoReset(false);
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            bool timedOut = false;
            bool cancelled = false;
            bool tooLarge = false;
            QByteArray data;
            connect(&timeout, &QTimer::timeout, &loop, [&]
            {
                timedOut = true;
                reply->abort();
            });
            connect(&progress, &QProgressDialog::canceled, &loop, [&]
            {
                cancelled = true;
                reply->abort();
            });
            connect(reply, &QNetworkReply::readyRead, &loop, [&]
            {
                while (reply->bytesAvailable() > 0)
                {
                    const QByteArray chunk = reply->read(64 * 1024);
                    if (chunk.isEmpty())
                        break;
                    if (data.size() + chunk.size() > size)
                    {
                        tooLarge = true;
                        reply->abort();
                        break;
                    }
                    data.append(chunk);
                }
            });
            connect(reply, &QNetworkReply::downloadProgress, &progress, [&](qint64 received, qint64)
            {
                progress.setValue(static_cast<int>(qBound<qint64>(0, received, size) * 100 / size));
            });
            connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            timeout.start(120000);
            progress.show();
            loop.exec();
            timeout.stop();
            disconnect(&progress, &QProgressDialog::canceled, &loop, nullptr);
            progress.close();
            QString error;
            if (cancelled)
            {
                reply->deleteLater();
                return;
            }
            if (timedOut)
                error = Translate(QStringLiteral("The update download timed out."));
            else if (tooLarge)
                error = Translate(QStringLiteral("The downloaded update is larger than expected."));
            else if (reply->error() != QNetworkReply::NoError)
                error = Translate(QStringLiteral("Could not download the update: %1")).arg(reply->errorString());
            else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                error = Translate(QStringLiteral("GitHub returned HTTP status %1."))
                    .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
            reply->deleteLater();
            QString backup;
            const QString executable = QCoreApplication::applicationFilePath();
#ifdef Q_OS_LINUX
            Q_UNUSED(windowsAsset);
            Q_UNUSED(macAsset);
            if (error.isEmpty() &&
                !InstallLinuxUpdate(executable, data, *linuxAsset, backup, error))
            {
                // Translate the fixed part of errors containing system details separately.
                const int separator = error.indexOf(QStringLiteral(": "));
                if (separator >= 0)
                    error = Translate(error.left(separator + 2) + QStringLiteral("%1"))
                        .arg(error.mid(separator + 2));
                else
                    error = Translate(error);
            }
#elif defined(Q_OS_WIN)
            Q_UNUSED(linuxAsset);
            Q_UNUSED(macAsset);
            QTemporaryDir stage(QDir::tempPath() + QStringLiteral("/pkg-extractor-update-XXXXXX"));
            QStringList files;
            if (error.isEmpty() && !stage.isValid())
                error = Translate(QStringLiteral("Could not create an update staging directory."));
            if (error.isEmpty() && !StageWindowsUpdate(data, *windowsAsset, stage.path(), files, error))
                error = Translate(error);
#else
            Q_UNUSED(linuxAsset);
            Q_UNUSED(windowsAsset);
            QString bundle;
            if (error.isEmpty() && !CanUpdateMacInstallation(executable, bundle, error))
                error = Translate(error);
            QTemporaryDir stage((bundle.isEmpty() ? QDir::tempPath() : QFileInfo(bundle).absolutePath()) +
                                QStringLiteral("/.pkg-update-XXXXXX"));
            if (stage.isValid())
                stage.setAutoRemove(false);
            if (error.isEmpty() && !stage.isValid())
                error = Translate(QStringLiteral("Could not create an update staging directory."));
            if (error.isEmpty() && !StageMacUpdate(data, *macAsset, stage.path(), error))
                error = Translate(error);
#endif
            if (!error.isEmpty())
            {
                const auto answer = QMessageBox::warning(this, Translate(QStringLiteral("Update failed")),
                    error + QStringLiteral("\n\n") +
                        Translate(QStringLiteral("Open the release page for manual installation?")),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (answer == QMessageBox::Yes && !QDesktopServices::openUrl(releaseUrl))
                    QMessageBox::warning(this, Translate(QStringLiteral("Could not open release page")),
                        Translate(QStringLiteral("Could not open the release page: %1")).arg(releaseUrl.toString()));
                return;
            }
#ifdef Q_OS_LINUX
            const auto answer = QMessageBox::question(this, Translate(QStringLiteral("Update installed")),
                Translate(QStringLiteral("The update is installed. A backup was saved at %1. Restart now?")).arg(backup),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (answer == QMessageBox::Yes)
            {
                if (QProcess::startDetached(executable, {}))
                    close();
                else
                    QMessageBox::warning(this, Translate(QStringLiteral("Could not restart")),
                        Translate(QStringLiteral("Please restart the application manually.")));
            }
#else
            const auto answer = QMessageBox::question(this, Translate(QStringLiteral("Update ready")),
                Translate(QStringLiteral("The update is ready. Install it and restart now?")),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (answer != QMessageBox::Yes)
                return;
#ifdef Q_OS_WIN
            const bool started = StartWindowsUpdate(stage.path(), files, executable, error);
#else
            const bool started = StartMacUpdate(stage.path(), bundle, error);
#endif
            if (!started)
            {
                const auto fallback = QMessageBox::warning(this, Translate(QStringLiteral("Update failed")),
                    Translate(error) + QStringLiteral("\n\n") +
                        Translate(QStringLiteral("Open the release page for manual installation?")),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (fallback == QMessageBox::Yes && !QDesktopServices::openUrl(releaseUrl))
                    QMessageBox::warning(this, Translate(QStringLiteral("Could not open release page")),
                        Translate(QStringLiteral("Could not open the release page: %1")).arg(releaseUrl.toString()));
                return;
            }
            stage.setAutoRemove(false);
            close();
#endif
        }
#endif

        void SelectInputFile()
        {
            // The native macOS dialog can fail to present in packaged Qt builds; use the Qt dialog implementation.
            const QString file = QFileDialog::getOpenFileName(
                this, Translate(QStringLiteral("Select PKG file")), QDir::homePath(),
                Translate(QStringLiteral("PS4 package files (*.pkg);;All files (*)")), nullptr,
                QFileDialog::DontUseNativeDialog);
            if (!file.isEmpty())
            {
                inputPath_->setText(file);
            }
        }

        void SelectOutputDirectory()
        {
            const QString directory = QFileDialog::getExistingDirectory(
                this, Translate(QStringLiteral("Select output directory")), QDir::homePath(),
                QFileDialog::DontUseNativeDialog);
            if (directory.isEmpty())
            {
                return;
            }
            QString error;
            if (!CanWriteToDirectory(directory, error))
            {
                QMessageBox::warning(
                    this, Translate(QStringLiteral("Output directory is not writable")),
                    Translate(QStringLiteral("Cannot write to output directory %1: %2")).arg(directory, error));
                return;
            }
            outputPath_->setText(directory);
        }

        // The extract action is available only when both required paths are selected and idle.
        void UpdateExecuteButton()
        {
            executeButton_->setEnabled(!inputPath_->text().isEmpty() && !outputPath_->text().isEmpty() &&
                thread_ == nullptr);
        }

        // Disable path controls, start the worker, and restore the UI when it finishes.
        void StartExtraction()
        {
            progressBar_->setValue(0);
            statusLabel_->setText(Translate(QStringLiteral("Starting extraction...")));
            executeButton_->setEnabled(false);
            cancelButton_->setEnabled(true);
            inputPath_->setEnabled(false);
            outputPath_->setEnabled(false);
            inputButton_->setEnabled(false);
            outputButton_->setEnabled(false);
            menuBar_->setEnabled(false);

            thread_ = new ExtractionThread(ToPath(inputPath_->text()), ToPath(outputPath_->text()), this);
            terminal_->clear();
            terminalSubpackage_.clear();
            connect(thread_, &ExtractionThread::progressUpdated, this,
                    [this](int percent, const QString& status)
                    {
                        progressBar_->setValue(percent);
                        statusLabel_->setText(status);
                    });
            connect(thread_, &ExtractionThread::logMessage, terminal_, &QPlainTextEdit::appendPlainText);
            connect(thread_, &ExtractionThread::subpackageProgress, this,
                    [this](const QString& name, int percent)
                    {
                        if (terminalSubpackage_ != name)
                        {
                            terminalSubpackage_ = name;
                            terminal_->appendPlainText(Translate(QStringLiteral("Extracting subpackage: %1 (%2%)"))
                                                           .arg(name)
                                                           .arg(percent));
                            return;
                        }
                        auto cursor = terminal_->textCursor();
                        cursor.movePosition(QTextCursor::End);
                        cursor.select(QTextCursor::LineUnderCursor);
                        cursor.insertText(
                            Translate(QStringLiteral("Extracting subpackage: %1 (%2%)"))
                                .arg(name)
                                .arg(percent));
                        terminal_->setTextCursor(cursor);
                    });
            connect(thread_, &ExtractionThread::completed, this,
                    [this](bool success, bool cancelled, const QString& message, const QString& outputDirectory)
                    {
                        if (success)
                        {
                            statusLabel_->setText(message);
                            const auto answer = QMessageBox::question(
                                this, Translate(QStringLiteral("Extraction complete")),
                                Translate(QStringLiteral(
                                    "Extraction succeeded. Would you like to show the extracted directory?")),
                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                            if (answer == QMessageBox::Yes &&
                                !QDesktopServices::openUrl(QUrl::fromLocalFile(outputDirectory)))
                            {
                                QMessageBox::warning(
                                    this, Translate(QStringLiteral("Could not open directory")),
                                    Translate(QStringLiteral("Could not show the extracted directory: %1"))
                                        .arg(outputDirectory));
                            }
                        }
                        else if (cancelled)
                        {
                            statusLabel_->setText(message);
                        }
                        else
                        {
                            statusLabel_->setText(Translate(QStringLiteral("Extraction failed.")));
                            QMessageBox::critical(this, Translate(QStringLiteral("Extraction failed")), message);
                        }
                        if (success)
                        {
                            progressBar_->setValue(100);
                        }
                    });
            connect(thread_, &ExtractionThread::finalizing, this,
                    [this]
                    {
                        cancelButton_->setEnabled(false);
                        statusLabel_->setText(Translate(QStringLiteral("Finalizing extraction...")));
                    },
                    Qt::BlockingQueuedConnection);
            connect(thread_, &QThread::finished, this, [this]
            {
                thread_ = nullptr;
                cancelButton_->setEnabled(false);
                inputPath_->setEnabled(true);
                outputPath_->setEnabled(true);
                inputButton_->setEnabled(true);
                outputButton_->setEnabled(true);
                menuBar_->setEnabled(true);
                executeButton_->setEnabled(!inputPath_->text().isEmpty() && !outputPath_->text().isEmpty());
            });
            connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
            thread_->start();
        }

        // Confirm destructive cancellation before asking the worker to discard its staging tree.
        void ConfirmCancellation()
        {
            if (thread_ == nullptr || !thread_->isRunning())
            {
                return;
            }
            const auto answer = QMessageBox::warning(
                this, Translate(QStringLiteral("Cancel extraction?")),
                Translate(QStringLiteral(
                    "Cancelling will delete all files unpacked during this extraction. Existing files will be untouched.")),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes)
            {
                cancelButton_->setEnabled(false);
                statusLabel_->setText(Translate(QStringLiteral("Cancelling extraction...")));
                thread_->RequestCancel();
            }
        }

        QLineEdit* inputPath_ = nullptr;
        QLineEdit* outputPath_ = nullptr;
        QLabel* inputLabel_ = nullptr;
        QLabel* outputLabel_ = nullptr;
        QProgressBar* progressBar_ = nullptr;
        QPlainTextEdit* terminal_ = nullptr;
        QString terminalSubpackage_;
        QLabel* statusLabel_ = nullptr;
        QPushButton* executeButton_ = nullptr;
        QPushButton* cancelButton_ = nullptr;
        QPushButton* inputButton_ = nullptr;
        QPushButton* outputButton_ = nullptr;
        QMenuBar* menuBar_ = nullptr;
        QMenu* fileMenu_ = nullptr;
        QMenu* extrasMenu_ = nullptr;
        QMenu* languageMenu_ = nullptr;
        QAction* openPkgAction_ = nullptr;
        QAction* outputDirectoryAction_ = nullptr;
        QAction* exitAction_ = nullptr;
        QAction* checkUpdatesAction_ = nullptr;
        QAction* githubAction_ = nullptr;
        QNetworkAccessManager* network_ = nullptr;
        ExtractionThread* thread_ = nullptr;
    };
} // namespace

int RunGui(int argc, char** argv)
{
    QApplication app(argc, argv);
    ExtractorWindow window;
    window.show();
    window.activateWindow();
    return app.exec();
}
