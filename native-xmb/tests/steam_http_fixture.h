#pragma once
// A loopback HTTP server for the Steam store / owned-games tests. It answers every request with the canned
// status, headers and body that the test's handler returns, and records the request line and headers so a test
// can assert what was sent (for example that the Web API key travels in a header and never in the URL).
// Header-only; QtNetwork and a running QCoreApplication event loop are required.

#include <QByteArray>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>

namespace steamtest {

struct HttpReply {
    int status = 200;
    QByteArray body;
    QByteArray contentType = "application/json";
    QByteArray extraHeaders;      // raw "Name: value\r\n" lines
    bool neverAnswer = false;     // accept the request and hold the connection open
};

class HttpFixture : public QObject {
public:
    using Handler = std::function<HttpReply(const QByteArray& requestLine, const QByteArray& headers)>;

    explicit HttpFixture(QObject* parent = nullptr) : QObject(parent) {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                sockets_.append(socket);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onData(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return server_.serverPort(); }
    QByteArray origin() const { return "http://127.0.0.1:" + QByteArray::number(port()); }
    void setHandler(Handler handler) { handler_ = std::move(handler); }
    int requestCount() const { return requests_; }
    QByteArray lastRequestLine() const { return lastLine_; }
    QByteArray lastHeaders() const { return lastHeaders_; }
    void closeHeldConnections() {
        for (const QPointer<QTcpSocket>& s : sockets_)
            if (s) s->disconnectFromHost();
        sockets_.clear();
    }

private:
    void onData(QTcpSocket* socket) {
        buffers_[socket].append(socket->readAll());
        const QByteArray& data = buffers_[socket];
        const int end = data.indexOf("\r\n\r\n");
        if (end < 0) return;
        const int firstLineEnd = data.indexOf("\r\n");
        lastLine_ = data.left(firstLineEnd);
        lastHeaders_ = data.mid(firstLineEnd + 2, end - firstLineEnd - 2);
        buffers_.remove(socket);
        ++requests_;
        const HttpReply reply = handler_ ? handler_(lastLine_, lastHeaders_) : HttpReply{404, "{}"};
        if (reply.neverAnswer) return;
        QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + " Fixture\r\n";
        out += "Content-Type: " + reply.contentType + "\r\n";
        out += "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\n";
        out += "Connection: close\r\n" + reply.extraHeaders + "\r\n";
        socket->write(out);
        socket->write(reply.body);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    QList<QPointer<QTcpSocket>> sockets_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    Handler handler_;
    int requests_ = 0;
    QByteArray lastLine_;
    QByteArray lastHeaders_;
};

// Runs the event loop until `done()` is true or the timeout elapses; returns whether done() became true.
inline bool waitFor(const std::function<bool()>& done, int timeoutMs = 8000) {
    QEventLoop loop;
    QTimer poll;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (done()) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(5);
    deadline.start(timeoutMs);
    if (!done()) loop.exec();
    return done();
}

// Lets already-queued events (and anything a cancelled request might still emit) run for a short while.
inline void settle(int ms = 150) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace steamtest
