#include "app/extractor_window.h"

#include <QApplication>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
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
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "app/extraction_thread.h"
#include "app/gui_utils.h"
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
