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
    if (argc < 2 || argc > 3)
        return 2;
    QCoreApplication::setLibraryPaths({QString::fromLocal8Bit(argv[1])});
    if (!QSslSocket::availableBackends().contains(QStringLiteral("securetransport")) ||
        !QSslSocket::setActiveBackend(QStringLiteral("securetransport")) || !QSslSocket::supportsSsl())
    {
        std::fprintf(stderr, "Qt Secure Transport TLS backend is unavailable.\n");
        return 1;
    }
    QNetworkAccessManager manager;
    const QUrl url(argc == 3 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("https://api.github.com/"));
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty())
    {
        std::fprintf(stderr, "TLS probe requires an HTTPS URL.\n");
        return 2;
    }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "shadPS4Plus-PkgExtractor-TLS-test");
    QNetworkReply* reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    bool encrypted = false;
    bool sslErrors = false;
    QObject::connect(reply, &QNetworkReply::encrypted, [&encrypted] { encrypted = true; });
    QObject::connect(reply, &QNetworkReply::sslErrors,
                     [&sslErrors](const QList<QSslError>&) { sslErrors = true; });
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(15000);
    loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const bool httpError = (networkError >= QNetworkReply::ContentAccessDenied &&
                            networkError <= QNetworkReply::UnknownContentError) ||
                           (networkError >= QNetworkReply::InternalServerError &&
                            networkError <= QNetworkReply::UnknownServerError);
    if (!reply->isFinished() || !encrypted || sslErrors || status < 200 || status > 599 ||
        (networkError != QNetworkReply::NoError && !(status >= 400 && httpError)))
    {
        std::fprintf(stderr, "HTTPS request failed (HTTP %d): %s\n", status, qPrintable(reply->errorString()));
        return 1;
    }
    if (status >= 400)
        std::fprintf(stderr, "TLS succeeded; server returned HTTP %d.\n", status);
    return 0;
}
