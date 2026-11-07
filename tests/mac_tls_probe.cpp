#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTimer>

#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2)
        return 2;
    QCoreApplication::setLibraryPaths({QString::fromLocal8Bit(argv[1])});
    if (!QSslSocket::supportsSsl() || !QSslSocket::availableBackends().contains(QStringLiteral("securetransport")))
    {
        std::fprintf(stderr, "Qt Secure Transport TLS backend is unavailable.\n");
        return 1;
    }
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/")));
    request.setRawHeader("User-Agent", "shadPS4Plus-PkgExtractor-TLS-test");
    QNetworkReply* reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(15000);
    loop.exec();
    if (!reply->isFinished() || reply->error() != QNetworkReply::NoError ||
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
    {
        std::fprintf(stderr, "HTTPS request failed: %s\n", qPrintable(reply->errorString()));
        return 1;
    }
    return 0;
}
