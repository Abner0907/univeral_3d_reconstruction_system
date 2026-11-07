#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QTcpServer>
#include <QUrlQuery>

#include <functional>

class QFile;
class QTcpSocket;

namespace u3d {

struct HttpRequest {
    QByteArray method;              // "GET", "POST", ...
    QString path;                   // percent-decoded, without query
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers; // lower-case names
    QByteArray body;
    QHostAddress peer;

    QByteArray header(const QByteArray &name) const { return headers.value(name.toLower()); }
};

struct HttpResponse {
    int status = 200;
    QByteArray contentType = "application/json";
    QByteArray body;
    QString filePath; // if set, the file is streamed as the body
    QList<QPair<QByteArray, QByteArray>> headers;

    static HttpResponse json(const QJsonObject &obj, int status = 200);
    static HttpResponse json(const QJsonArray &arr, int status = 200);
    static HttpResponse text(const QString &text, int status = 200);
    static HttpResponse error(int status, const QString &message);
    static HttpResponse file(const QString &path, const QString &downloadName);
    static HttpResponse noContent();
};

// A small HTTP/1.1 server on QTcpServer: keep-alive, Content-Length bodies, `Expect:
// 100-continue` and streamed file downloads. Enough for the REST API the Android app and
// scripts use, without depending on the optional QtHttpServer module.
class HttpServer : public QObject
{
    Q_OBJECT

public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    explicit HttpServer(QObject *parent = nullptr);
    ~HttpServer() override;

    void setHandler(Handler handler) { m_handler = std::move(handler); }
    void setMaxBodySize(qint64 bytes) { m_maxBodySize = bytes; }

    bool listen(const QHostAddress &address, quint16 port);
    void close();
    bool isListening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }
    QString errorString() const { return m_server.errorString(); }

    static QByteArray reasonPhrase(int status);

private:
    struct Connection {
        QByteArray buffer;
        bool headerDone = false;
        qint64 contentLength = 0;
        bool keepAlive = true;
        HttpRequest request;
        QFile *sending = nullptr; // file being streamed
        bool closeAfterSend = false;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void processBuffer(QTcpSocket *socket);
    bool parseHeader(QTcpSocket *socket, Connection &conn, int headerEnd);
    void send(QTcpSocket *socket, const HttpResponse &response, bool keepAlive);
    void pumpFile(QTcpSocket *socket);
    void sendErrorAndClose(QTcpSocket *socket, int status, const QString &message);
    void dropConnection(QTcpSocket *socket);

    QTcpServer m_server;
    Handler m_handler;
    qint64 m_maxBodySize = qint64(1) << 30; // 1 GiB
    QHash<QTcpSocket *, Connection> m_connections;
};

} // namespace u3d
