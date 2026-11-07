#include "HttpServer.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace u3d {

namespace {
constexpr int kMaxHeaderSize = 64 * 1024;
constexpr qint64 kSendChunk = 256 * 1024;
constexpr qint64 kSendHighWater = 1024 * 1024;
} // namespace

// ----------------------------------------------------------------------------- HttpResponse

HttpResponse HttpResponse::json(const QJsonObject &obj, int status)
{
    HttpResponse r;
    r.status = status;
    r.body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return r;
}

HttpResponse HttpResponse::json(const QJsonArray &arr, int status)
{
    HttpResponse r;
    r.status = status;
    r.body = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    return r;
}

HttpResponse HttpResponse::text(const QString &text, int status)
{
    HttpResponse r;
    r.status = status;
    r.contentType = "text/plain; charset=utf-8";
    r.body = text.toUtf8();
    return r;
}

HttpResponse HttpResponse::error(int status, const QString &message)
{
    return json(QJsonObject{{QStringLiteral("error"), message}}, status);
}

HttpResponse HttpResponse::file(const QString &path, const QString &downloadName)
{
    HttpResponse r;
    r.contentType = "application/octet-stream";
    r.filePath = path;
    QString safeName = downloadName;
    safeName.remove(QLatin1Char('"'));
    r.headers.append({"Content-Disposition",
                      "attachment; filename=\"" + safeName.toUtf8() + "\""});
    return r;
}

HttpResponse HttpResponse::noContent()
{
    HttpResponse r;
    r.status = 204;
    return r;
}

// ----------------------------------------------------------------------------- HttpServer

HttpServer::HttpServer(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

HttpServer::~HttpServer()
{
    close();
}

bool HttpServer::listen(const QHostAddress &address, quint16 port)
{
    return m_server.listen(address, port);
}

void HttpServer::close()
{
    m_server.close();
    const auto sockets = m_connections.keys();
    for (QTcpSocket *socket : sockets) {
        disconnect(socket, nullptr, this, nullptr);
        socket->abort();
        delete m_connections.value(socket).sending;
        socket->deleteLater();
    }
    m_connections.clear();
}

QByteArray HttpServer::reasonPhrase(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default: return "Unknown";
    }
}

void HttpServer::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        m_connections.insert(socket, Connection());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::bytesWritten, this, [this, socket] { pumpFile(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { dropConnection(socket); });
    }
}

void HttpServer::dropConnection(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;
    delete it->sending;
    m_connections.erase(it);
    socket->deleteLater();
}

void HttpServer::onReadyRead(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;
    const QByteArray data = socket->readAll();
    if (socket->state() != QAbstractSocket::ConnectedState)
        return; // closing after an error response: ignore the rest of the request
    it->buffer += data;
    processBuffer(socket);
}

void HttpServer::processBuffer(QTcpSocket *socket)
{
    for (;;) {
        auto it = m_connections.find(socket);
        if (it == m_connections.end() || socket->state() != QAbstractSocket::ConnectedState)
            return;
        Connection &conn = *it;
        if (conn.sending)
            return; // finish streaming the previous response first

        if (!conn.headerDone) {
            const int headerEnd = conn.buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                if (conn.buffer.size() > kMaxHeaderSize)
                    sendErrorAndClose(socket, 431, QStringLiteral("Request header too large"));
                return;
            }
            if (!parseHeader(socket, conn, headerEnd))
                return;
        }
        if (conn.buffer.size() < conn.contentLength)
            return; // wait for the rest of the body

        HttpRequest request = std::move(conn.request);
        request.body = conn.buffer.left(int(conn.contentLength));
        conn.buffer.remove(0, int(conn.contentLength));
        const bool keepAlive = conn.keepAlive;
        conn.request = HttpRequest();
        conn.headerDone = false;
        conn.contentLength = 0;

        HttpResponse response;
        if (m_handler) {
            try {
                response = m_handler(request);
            } catch (const std::exception &e) {
                response = HttpResponse::error(500, QString::fromUtf8(e.what()));
            }
        } else {
            response = HttpResponse::error(404, QStringLiteral("Not found"));
        }
        send(socket, response, keepAlive);
        if (!keepAlive)
            return;
    }
}

bool HttpServer::parseHeader(QTcpSocket *socket, Connection &conn, int headerEnd)
{
    const QByteArray head = conn.buffer.left(headerEnd);
    conn.buffer.remove(0, headerEnd + 4);

    const QList<QByteArray> lines = head.split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    if (requestLine.size() != 3 || !requestLine[2].startsWith("HTTP/1.")) {
        sendErrorAndClose(socket, 400, QStringLiteral("Malformed request line"));
        return false;
    }

    HttpRequest &req = conn.request;
    req.method = requestLine[0].toUpper();
    const QByteArray target = requestLine[1];
    const int q = target.indexOf('?');
    req.path = QUrl::fromPercentEncoding(q < 0 ? target : target.left(q));
    req.query = QUrlQuery(q < 0 ? QString() : QString::fromUtf8(target.mid(q + 1)));
    req.peer = socket->peerAddress();
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const int colon = line.indexOf(':');
        if (colon > 0)
            req.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }

    const QByteArray connection = req.header("connection").toLower();
    conn.keepAlive = requestLine[2] == "HTTP/1.1" ? connection != "close" : connection == "keep-alive";

    if (req.headers.contains("transfer-encoding")) {
        sendErrorAndClose(socket, 411, QStringLiteral("Chunked uploads are not supported; send Content-Length"));
        return false;
    }
    conn.contentLength = 0;
    if (req.headers.contains("content-length")) {
        bool ok = false;
        conn.contentLength = req.header("content-length").toLongLong(&ok);
        if (!ok || conn.contentLength < 0) {
            sendErrorAndClose(socket, 400, QStringLiteral("Invalid Content-Length"));
            return false;
        }
    }
    if (conn.contentLength > m_maxBodySize) {
        sendErrorAndClose(socket, 413, QStringLiteral("Request body too large"));
        return false;
    }
    if (req.header("expect").toLower() == "100-continue" && conn.buffer.size() < conn.contentLength)
        socket->write("HTTP/1.1 100 Continue\r\n\r\n");
    conn.headerDone = true;
    return true;
}

void HttpServer::send(QTcpSocket *socket, const HttpResponse &input, bool keepAlive)
{
    HttpResponse response = input;
    QFile *file = nullptr;
    qint64 length = response.body.size();
    if (!response.filePath.isEmpty()) {
        file = new QFile(response.filePath);
        if (file->open(QIODevice::ReadOnly)) {
            length = file->size();
        } else {
            delete file;
            file = nullptr;
            response = HttpResponse::error(404, QStringLiteral("File not found"));
            length = response.body.size();
        }
    }

    QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + ' '
                      + reasonPhrase(response.status) + "\r\n";
    if (response.status != 204) {
        head += "Content-Type: " + response.contentType + "\r\n";
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
    }
    head += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    for (const auto &h : response.headers)
        head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";
    socket->write(head);

    if (file) {
        Connection &conn = m_connections[socket];
        conn.sending = file;
        conn.closeAfterSend = !keepAlive;
        pumpFile(socket);
        return;
    }
    if (response.status != 204)
        socket->write(response.body);
    if (!keepAlive)
        socket->disconnectFromHost();
}

void HttpServer::pumpFile(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || !it->sending)
        return;
    QFile *file = it->sending;
    while (socket->bytesToWrite() < kSendHighWater && !file->atEnd()) {
        const QByteArray chunk = file->read(kSendChunk);
        if (chunk.isEmpty())
            break;
        socket->write(chunk);
    }
    if (!file->atEnd())
        return;

    delete file;
    it->sending = nullptr;
    if (it->closeAfterSend) {
        socket->disconnectFromHost();
        return;
    }
    // Handle any request that arrived while the file was streaming.
    QPointer<QTcpSocket> guard(socket);
    QTimer::singleShot(0, this, [this, guard] {
        if (guard)
            processBuffer(guard);
    });
}

void HttpServer::sendErrorAndClose(QTcpSocket *socket, int status, const QString &message)
{
    send(socket, HttpResponse::error(status, message), false);
}

} // namespace u3d
