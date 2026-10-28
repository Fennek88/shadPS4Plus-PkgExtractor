#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <utility>

#include "core/file_format/pkg.h"
#include "core/file_format/psf.h"
#include "common/string_util.h"
#include "core/loader.h"
#include "common/translations/translations.h"

#ifdef PKG_EXTRACTOR_WITH_QT
#include <QApplication>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#endif

namespace
{
    struct PackageInfo
    {
        int type = 0;
        std::filesystem::path outputPath;
    };

    // Read the package metadata and choose the destination layout for a game, update, or DLC.
    bool PreparePackage(const std::filesystem::path& file,
                        const std::filesystem::path& outputBase,
                        PKG& pkg,
                        PackageInfo& info,
                        std::string& failreason)
    {
        if (Loader::DetectFileType(file) != Loader::FileTypes::Pkg)
        {
            failreason = Translate(QStringLiteral("The selected file is not a valid PKG file.")).toStdString();
            return false;
        }

        if (!pkg.Open(file, failreason))
        {
            if (failreason.empty())
            {
                failreason = Translate(QStringLiteral("Could not open the package.")).toStdString();
            }
            return false;
        }

        PSF psf;
        if (!psf.Open(pkg.sfo))
        {
            failreason = Translate(QStringLiteral("Could not read the package SFO.")).toStdString();
            return false;
        }

        const auto category = psf.GetString("CATEGORY");
        if (!category.has_value())
        {
            failreason = Translate(
                    QStringLiteral("The package SFO does not contain a CATEGORY entry."))
                .toStdString();
            return false;
        }

        info.outputPath = outputBase / std::string{pkg.GetTitleID()};
        const std::string pkgType = pkg.GetPkgFlags();
        if (pkgType.contains("PATCH"))
        {
            info.type = 102;
            info.outputPath += "-patch";
        }
        else if (*category == "ac")
        {
            info.type = 103;
            if (const auto contentId = psf.GetString("CONTENT_ID"); contentId.has_value())
            {
                const auto parts = Common::SplitString(std::string{*contentId}, '-');
                if (parts.size() > 2 && !parts[2].empty())
                {
                    info.outputPath /= parts[2];
                }
            }
        }
        else
        {
            info.type = 101;
        }

        return true;
    }

    // Create and write a temporary file under the output base, then remove only the probe created here.
    bool CheckOutputWritable(const std::filesystem::path& outputBase, std::string& failreason)
    {
        const auto directory = outputBase.empty() ? std::filesystem::path{"."} : outputBase;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            failreason = error.message();
            return false;
        }

        static std::atomic_uint64_t nextId{0};
        std::filesystem::path probeDirectory;
        bool created = false;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto id = nextId.fetch_add(1, std::memory_order_relaxed);
            probeDirectory =
                directory /
                (".pkg-extractor-write-test-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(id));
            if (std::filesystem::create_directory(probeDirectory, error))
            {
                created = true;
                break;
            }
            if (error)
            {
                failreason = error.message();
                return false;
            }
        }
        if (!created)
        {
            failreason = "Could not create a temporary write-test directory.";
            return false;
        }

        {
            std::ofstream testFile(probeDirectory / "write-test", std::ios::binary);
            if (!testFile)
            {
                failreason = "Could not create a temporary file in the output directory.";
            }
            else
            {
                testFile.put('x');
                testFile.flush();
                if (!testFile)
                {
                    failreason = "Could not write to the output directory.";
                }
            }
        }

        std::filesystem::remove_all(probeDirectory, error);
        if (error)
        {
            failreason = error.message();
            return false;
        }
        return failreason.empty();
    }

    // Extract into a private staging tree for GUI runs, then merge only after all files succeed.
    // CLI runs retain their direct-to-output behavior because they do not enable staging.
    bool ExtractPackage(PKG& pkg,
                        const std::filesystem::path& file,
                        const PackageInfo& info,
                        const std::filesystem::path& outputBase,
                        const std::function<void(int, int, int)>& onProgress,
                        const std::function<void(const std::string&, int)>& onSubpackageProgress,
                        const std::function<void()>& onFinalizing,
                        const std::atomic_bool* cancelRequested,
                        bool stageOutput,
                        bool& cancelled,
                        std::string& failreason)
    {
        std::filesystem::path stagingDirectory;
        std::filesystem::path extractionPath = info.outputPath;
        if (stageOutput)
        {
            // Create a unique child directory so concurrent or repeated runs never share partial files.
            std::error_code error;
            std::filesystem::create_directories(outputBase, error);
            if (error)
            {
                failreason = error.message();
                return false;
            }

            static std::atomic_uint64_t nextId{0};
            bool created = false;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                const auto id = nextId.fetch_add(1, std::memory_order_relaxed);
                // Exclusive creation gives each run a private tree, even when final output paths match.
                stagingDirectory =
                    outputBase /
                    (".pkg-extractor-staging-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                        std::to_string(id));
                if (std::filesystem::create_directory(stagingDirectory, error))
                {
                    created = true;
                    break;
                }
                if (error)
                {
                    failreason = error.message();
                    return false;
                }
            }
            if (!created)
            {
                failreason = "Could not create an isolated staging directory.";
                return false;
            }

            const auto relativeOutput = info.outputPath.lexically_relative(outputBase);
            if (relativeOutput.empty() || relativeOutput.is_absolute() ||
                *relativeOutput.begin() == "..")
            {
                std::filesystem::remove_all(stagingDirectory, error);
                failreason = "The output path is not inside the selected output directory.";
                return false;
            }
            extractionPath = stagingDirectory / relativeOutput;
        }

        // PKG::Extract prepares package metadata and the file table before individual payloads are read.
        if (!pkg.Extract(file, extractionPath, failreason))
        {
            if (failreason.empty())
            {
                failreason = Translate(
                        QStringLiteral("Failed to prepare the package for extraction."))
                    .toStdString();
            }
            if (stageOutput)
            {
                std::error_code ignored;
                std::filesystem::remove_all(stagingDirectory, ignored);
            }
            return false;
        }

        // Check cancellation between sectors so progress remains responsive for large files.
        const int fileCount = static_cast<int>(pkg.GetNumberOfFiles());
        int lastReportedPercent = -1;
        const auto reportProgress = [&](int current, int percent)
        {
            if (onProgress && percent != lastReportedPercent)
            {
                onProgress(current, fileCount, percent);
                lastReportedPercent = percent;
            }
        };
        reportProgress(0, 0);
        for (int i = 0; i < fileCount; ++i)
        {
            if (cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed))
            {
                cancelled = true;
                std::error_code error;
                std::filesystem::remove_all(stagingDirectory, error);
                if (error)
                {
                    cancelled = false;
                    failreason = error.message();
                }
                return false;
            }
            if (!pkg.ExtractFiles(i, cancelRequested, [&](const std::string& name, int completedSectors, int totalSectors)
                {
                    const double fileProgress =
                        totalSectors == 0 ? 1.0
                                          : static_cast<double>(completedSectors) / totalSectors;
                    const int percent = fileCount == 0
                                            ? 100
                                            : static_cast<int>((i + fileProgress) * 100 / fileCount);
                    reportProgress(i + 1, percent);
                    if (onSubpackageProgress)
                    {
                        onSubpackageProgress(
                            name, totalSectors == 0 ? 100 : completedSectors * 100 / totalSectors);
                    }
                }))
            {
                cancelled = true;
                std::error_code error;
                if (stageOutput)
                {
                    std::filesystem::remove_all(stagingDirectory, error);
                }
                if (error)
                {
                    cancelled = false;
                    failreason = error.message();
                }
                return false;
            }
            reportProgress(i + 1, (i + 1) * 100 / fileCount);
        }

        if (cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed))
        {
            cancelled = true;
            std::error_code error;
            std::filesystem::remove_all(stagingDirectory, error);
            if (error)
            {
                cancelled = false;
                failreason = error.message();
            }
            return false;
        }

        reportProgress(fileCount, 100);

        if (stageOutput)
        {
            if (onFinalizing)
            {
                onFinalizing();
            }
            // Once merging begins, cancellation is disabled because output may already be replacing files.
            if (cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed))
            {
                cancelled = true;
                std::error_code error;
                std::filesystem::remove_all(stagingDirectory, error);
                if (error)
                {
                    cancelled = false;
                    failreason = error.message();
                }
                return false;
            }

            std::error_code error;
            // Merge only after extraction succeeds; cancellation before this point removes only this run's tree.
            for (std::filesystem::recursive_directory_iterator it(extractionPath, error), end;
                 !error && it != end; it.increment(error))
            {
                const auto relativePath = std::filesystem::relative(it->path(), extractionPath, error);
                if (error)
                {
                    break;
                }
                const auto destination = info.outputPath / relativePath;
                if (it->is_directory(error))
                {
                    std::filesystem::create_directories(destination, error);
                }
                else if (!error && it->is_regular_file(error))
                {
                    std::filesystem::create_directories(destination.parent_path(), error);
                    if (!error)
                    {
                        std::filesystem::copy_file(
                            it->path(), destination, std::filesystem::copy_options::overwrite_existing, error);
                    }
                }
            }
            if (error)
            {
                failreason = error.message();
                std::error_code ignored;
                std::filesystem::remove_all(stagingDirectory, ignored);
                return false;
            }

            std::filesystem::remove_all(stagingDirectory, error);
            if (error)
            {
                failreason = error.message();
                return false;
            }
        }
        return true;
    }

#ifdef PKG_EXTRACTOR_WITH_QT
    // Convert through the platform's native path encoding to preserve non-ASCII filenames.
    std::filesystem::path ToPath(const QString& value)
    {
#ifdef _WIN32
        return value.toStdWString();
#else
        return value.toStdString();
#endif
    }

    // Convert a filesystem path to a QString using the matching platform representation.
    QString ToQString(const std::filesystem::path& value)
    {
#ifdef _WIN32
        return QString::fromStdWString(value.wstring());
#else
        return QString::fromStdString(value.string());
#endif
    }

    bool CanWriteToDirectory(const QString& directory, QString& error)
    {
        QTemporaryFile writeTest(QDir(directory).filePath(QStringLiteral(".pkg-extractor-write-test-XXXXXX")));
        if (!writeTest.open() || writeTest.write("x", 1) != 1 || !writeTest.flush())
        {
            error = writeTest.errorString();
            return false;
        }
        return true;
    }

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
        void completed(bool success, bool cancelled, const QString& message);

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
            PackageInfo info;
            std::string error;
            if (!PreparePackage(file_, outputBase_, pkg, info, error))
            {
                emit logMessage(
                    Translate(QStringLiteral("Cannot open PKG file: %1")).arg(QString::fromStdString(error)));
                emit completed(false, false, QString::fromStdString(error));
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
            const bool success = ExtractPackage(
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
                &cancelRequested_,
                true,
                cancelled,
                error);
            if (success)
            {
                emit progressUpdated(100, Translate(QStringLiteral("Extraction complete.")));
                emit logMessage(Translate(QStringLiteral("THE END %1")).arg(ToQString(file_)));
                emit completed(true, false,
                               Translate(QStringLiteral("Extracted to %1")).arg(ToQString(info.outputPath)));
            }
            else if (cancelled)
            {
                emit logMessage(Translate(QStringLiteral("Extraction cancelled.")));
                emit completed(false, true,
                               Translate(QStringLiteral("Extraction cancelled. Unpacked files were deleted.")));
            }
            else
            {
                emit logMessage(Translate(QStringLiteral("Cannot extract PKG file: %1"))
                    .arg(QString::fromStdString(error)));
                emit completed(false, false, QString::fromStdString(error));
            }
        }

    private:
        std::filesystem::path file_;
        std::filesystem::path outputBase_;
        std::atomic_bool cancelRequested_{false};
    };

    class ExtractorWindow final : public QMainWindow
    {
    public:
        // Build the picker, progress, status, and action controls and connect their UI events.
        ExtractorWindow()
        {
            menuBar_ = new QMenuBar(nullptr);
            menuBar_->setNativeMenuBar(true);
            fileMenu_ = menuBar_->addMenu(QString());
            openPkgAction_ = fileMenu_->addAction(QString());
            outputDirectoryAction_ = fileMenu_->addAction(QString());
            fileMenu_->addSeparator();
            exitAction_ = fileMenu_->addAction(QString());
            extrasMenu_ = menuBar_->addMenu(QString());
            languageMenu_ = extrasMenu_->addMenu(QString());

            auto* languageGroup = new QActionGroup(this);
            languageGroup->setExclusive(true);
            const QStringList languages = AvailableLanguages();
            const QString systemLanguage = QLocale::system().name().section(QLatin1Char('_'), 0, 0);
            const QString selectedLanguage = languages.contains(systemLanguage)
                                                 ? systemLanguage
                                                 : languages.contains(QStringLiteral("en"))
                                                     ? QStringLiteral("en")
                                                     : languages.value(0);
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
            setWindowTitle(Translate(QStringLiteral("ShadPS4Plus PKG Extractor")));
            fileMenu_->setTitle(Translate(QStringLiteral("File")));
            openPkgAction_->setText(Translate(QStringLiteral("Open PKG")));
            outputDirectoryAction_->setText(Translate(QStringLiteral("Output Directory")));
            exitAction_->setText(Translate(QStringLiteral("Exit")));
            extrasMenu_->setTitle(Translate(QStringLiteral("Extras")));
            languageMenu_->setTitle(Translate(QStringLiteral("Language")));
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
                    [this](bool success, bool cancelled, const QString& message)
                    {
                        if (success)
                        {
                            statusLabel_->setText(message);
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
        ExtractionThread* thread_ = nullptr;
    };
#endif
} // namespace

// Run the Qt interface without arguments; otherwise preserve the command-line extractor modes.
int main(int argc, char** argv)
{
#ifdef PKG_EXTRACTOR_WITH_QT
    if (argc == 1)
    {
        QApplication app(argc, argv);
        ExtractorWindow window;
        window.show();
        window.activateWindow();
        return app.exec();
    }
#endif

    std::filesystem::path file;
    if (argc > 1)
    {
        file = argv[1];
    }
    std::filesystem::path outputBase;
    if (argc > 2)
    {
        outputBase = argv[2];
    }

    PKG pkg;
    PackageInfo info;
    std::string failreason;
    if (!PreparePackage(file, outputBase, pkg, info, failreason))
    {
        if (Loader::DetectFileType(file) != Loader::FileTypes::Pkg)
        {
            std::cout << file << " doesn't appear to be a valid PKG file" << std::endl;
        }
        else
        {
            std::cout << "Cannot open PKG file: " << failreason << std::endl;
        }
        return 1;
    }

    std::cout << file << " is a valid PKG\n" << std::endl;
    if (info.type == 102)
    {
        std::cout << "pkg is a game update" << std::endl;
    }
    else if (info.type == 103)
    {
        std::cout << "pkg is a dlc" << std::endl;
    }
    else
    {
        std::cout << "pkg is a base game" << std::endl;
    }

    if (argc == 3 && std::string("--check-type") == argv[2])
    {
        return info.type;
    }

    if (!CheckOutputWritable(outputBase, failreason))
    {
        std::cerr << "Cannot write to output directory: " << failreason << std::endl;
        return 1;
    }

    std::cout << "Extracting pkg to " << info.outputPath << std::endl;
    bool cancelled = false;
    if (!ExtractPackage(pkg, file, info, outputBase,
                        [](int, int, int)
                        {
                        },
                        [](const std::string& name, int percent)
                        {
                            std::cout << "\rExtracting subpackage " << name << ": " << percent << "%"
                                      << std::flush;
                        },
                        {},
                        nullptr,
                        false,
                        cancelled,
                        failreason))
    {
        std::cout << "\nCannot extract PKG file: " << failreason << std::endl;
        return 1;
    }

    std::cout << "\nTHE END " << file << std::endl;
    if (argc == 2)
    {
        std::cout << "\nPress [enter] to exit." << std::endl;
        std::cin.get();
    }
    return 0;
}

#ifdef PKG_EXTRACTOR_WITH_QT
#include "main.moc"
#endif
