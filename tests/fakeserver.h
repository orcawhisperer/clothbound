#pragma once

// A local HTTP server that answers each request with a prepared response,
// so provider adapters can be tested against recorded API traffic.

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <memory>

class FakeServer {
public:
    struct Response {
        int status = 200;
        QByteArray contentType = "application/json";
        QByteArray body;
    };
    struct Request {
        QByteArray method;
        QByteArray path;
        QMap<QByteArray, QByteArray> headers;
        QByteArray body;
    };

    FakeServer() {
        m_server.listen(QHostAddress::LocalHost);
        QPointer<QTcpServer> guard(&m_server);
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this, guard] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, guard, socket, buffer] {
                    if (!guard)
                        return;
                    *buffer += socket->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    Request request;
                    const QList<QByteArray> lines = buffer->left(end).split('\n');
                    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                    request.method = first.value(0);
                    request.path = first.value(1);
                    for (const QByteArray &line : lines.mid(1)) {
                        const qsizetype colon = line.indexOf(':');
                        if (colon > 0)
                            request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                    }
                    const int length = request.headers.value("content-length").toInt();
                    if (buffer->size() < end + 4 + length)
                        return;
                    request.body = buffer->mid(end + 4, length);
                    requests.append(request);
                    Response response{404, "text/plain", "none"};
                    if (routes.contains(request.path))
                        response = routes.value(request.path);
                    else if (!responses.isEmpty())
                        response = responses.takeFirst();
                    QByteArray reply = "HTTP/1.1 " + QByteArray::number(response.status) + " Status\r\n";
                    reply += "Content-Type: " + response.contentType + "\r\n";
                    reply += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
                    reply += "Connection: close\r\n\r\n";
                    reply += response.body;
                    socket->write(reply);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    ~FakeServer() { m_server.close(); }

    QString endpoint() const { return QStringLiteral("http://127.0.0.1:%1/v1beta").arg(m_server.serverPort()); }

    QString origin() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    QList<Response> responses;
    // Answers for a path, whenever it's asked (before `responses`).
    QMap<QByteArray, Response> routes;
    QList<Request> requests;

private:
    QTcpServer m_server;
};
