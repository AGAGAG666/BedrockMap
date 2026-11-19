#include "mcpserver_p.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {
constexpr quint16 localPort = 8765;
constexpr qsizetype maxHeader = 16 * 1024;
constexpr qsizetype maxBody = 1024 * 1024;
constexpr int maxConnections = 16;
const QString protocol = QStringLiteral("2025-11-25");
const QSet<QString> protocols{protocol, QStringLiteral("2025-06-18")};

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message) {
    return {{"jsonrpc", "2.0"}, {"id", id.isUndefined() ? QJsonValue() : id},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

void reply(QTcpSocket* socket, const QByteArray& status, const QByteArray& bytes = {}, const QByteArray& extra = {}) {
    socket->setProperty("mcpReplied", true);
    socket->write("HTTP/1.1 " + status + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(bytes.size()) +
                  "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n" + extra + "\r\n" + bytes);
    socket->disconnectFromHost();
}

bool sameToken(const QByteArray& provided, const QByteArray& expected) {
    if (provided.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < expected.size(); ++i) diff |= static_cast<unsigned char>(provided[i] ^ expected[i]);
    return diff == 0;
}
}

McpServer::McpServer(AppController& app)
    : listener_(std::make_unique<QTcpServer>()), tools_(std::make_unique<Tools>(app)) {
    listener_->setMaxPendingConnections(maxConnections);
    connect(listener_.get(), &QTcpServer::newConnection, this, &McpServer::acceptConnections);
}

McpServer::~McpServer() = default;

bool McpServer::supported() const {
#ifdef Q_OS_WASM
    return false;
#else
    return true;
#endif
}

bool McpServer::running() const { return listener_->isListening(); }

QString McpServer::endpoint() const { return QStringLiteral("http://127.0.0.1:%1/mcp").arg(localPort); }

bool McpServer::start() {
    if (running()) return true;
    if (!supported()) return false;
    error_.clear();
    if (!listener_->listen(QHostAddress::LocalHost, localPort)) {
        error_ = listener_->errorString();
        emit changed();
        return false;
    }
    QByteArray random;
    for (int i = 0; i < 8; ++i) random += QByteArray::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0');
    token_ = QString::fromLatin1(random);
    emit changed();
    return true;
}

void McpServer::stop() {
    listener_->close();
    for (auto* socket : listener_->findChildren<QTcpSocket*>()) socket->abort();
    token_.clear();
    writeEnabled_ = false;
    error_.clear();
    emit changed();
}

void McpServer::setWriteEnabled(bool enabled) {
    enabled = enabled && running();
    if (enabled == writeEnabled_) return;
    writeEnabled_ = enabled;
    emit changed();
}

void McpServer::acceptConnections() {
    while (auto* socket = listener_->nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        if (listener_->findChildren<QTcpSocket*>().size() > maxConnections) {
            reply(socket, "503 Service Unavailable");
            continue;
        }
        socket->setReadBufferSize(maxHeader + maxBody + 4);
        auto* deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, socket, &QAbstractSocket::abort);
        deadline->start(10000);
        const auto process = [this, socket, bytes = QByteArray()]() mutable {
            if (socket->property("mcpReplied").toBool()) return;
            bytes += socket->readAll();
            const auto headerSize = bytes.indexOf("\r\n\r\n");
            if ((headerSize == -1 && bytes.size() > maxHeader) || headerSize > maxHeader) {
                reply(socket, "431 Request Header Fields Too Large");
                return;
            }
            if (headerSize < 0) return;
            auto lines = bytes.first(headerSize).split('\n');
            const auto start = lines.takeFirst().trimmed().split(' ');
            if (start.size() != 3 || start[2] != "HTTP/1.1") { reply(socket, "400 Bad Request"); return; }
            if (start[1] != "/mcp") { reply(socket, "404 Not Found"); return; }
            QMap<QByteArray, QByteArray> headers;
            for (const auto& line : lines) {
                const auto colon = line.indexOf(':');
                if (colon <= 0 || line.startsWith(' ') || line.startsWith('\t')) { reply(socket, "400 Bad Request"); return; }
                const auto name = line.first(colon).toLower();
                if (headers.contains(name)) { reply(socket, "400 Bad Request"); return; }
                headers.insert(name, line.sliced(colon + 1).trimmed());
            }
            const auto port = QByteArray::number(localPort);
            const auto host = headers.value("host").toLower();
            const auto origin = headers.value("origin");
            if (!(host == "127.0.0.1:" + port || host == "localhost:" + port) ||
                (headers.contains("origin") && origin != "http://127.0.0.1:" + port && origin != "http://localhost:" + port)) {
                reply(socket, "403 Forbidden");
                return;
            }
            if (!running() || token_.isEmpty() || !sameToken(headers.value("authorization"), "Bearer " + token_.toLatin1())) {
                reply(socket, "401 Unauthorized", {}, "WWW-Authenticate: Bearer\r\n");
                return;
            }
            if (start[0] != "POST") { reply(socket, "405 Method Not Allowed", {}, "Allow: POST\r\n"); return; }
            const auto version = headers.value("mcp-protocol-version");
            if (!version.isEmpty() && !protocols.contains(QString::fromLatin1(version))) { reply(socket, "400 Bad Request"); return; }
            if (headers.contains("transfer-encoding")) { reply(socket, "400 Bad Request"); return; }
            if (headers.value("content-type").split(';').value(0).trimmed().toLower() != "application/json") {
                reply(socket, "415 Unsupported Media Type");
                return;
            }
            bool ok = false;
            const auto length = headers.value("content-length").toLongLong(&ok);
            if (!ok || length <= 0) { reply(socket, "400 Bad Request"); return; }
            if (length > maxBody) { reply(socket, "413 Content Too Large"); return; }
            const auto available = bytes.size() - headerSize - 4;
            if (available < length) return;
            if (available != length) { reply(socket, "400 Bad Request"); return; }
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(bytes.sliced(headerSize + 4, length), &parse);
            auto result = parse.error != QJsonParseError::NoError ? rpcError({}, -32700, "Parse error") :
                !document.isObject() ? rpcError({}, -32600, "Invalid Request") : dispatch(document.object());
            if (result.isEmpty()) reply(socket, "202 Accepted");
            else reply(socket, "200 OK", QJsonDocument(result).toJson(QJsonDocument::Compact));
        };
        connect(socket, &QTcpSocket::readyRead, socket, process);
        if (socket->bytesAvailable() > 0) QMetaObject::invokeMethod(socket, [socket] { emit socket->readyRead(); }, Qt::QueuedConnection);
    }
}

QJsonObject McpServer::dispatch(const QJsonObject& message) {
    const auto id = message.value("id");
    const auto method = message.value("method").toString();
    const auto params = message.value("params");
    if (message.value("jsonrpc").toString() != "2.0" || method.isEmpty() ||
        !(id.isUndefined() || id.isDouble() || id.isString()) || (!params.isUndefined() && !params.isObject()))
        return rpcError({}, -32600, "Invalid Request");
    if (id.isUndefined()) return {};
    QJsonObject result;
    if (method == "initialize") {
        const auto requested = params.toObject().value("protocolVersion").toString();
        if (requested.isEmpty()) return rpcError(id, -32602, "protocolVersion is required");
        result = {{"protocolVersion", protocols.contains(requested) ? requested : protocol},
                  {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                  {"serverInfo", QJsonObject{{"name", "BedrockMap"}, {"version", QStringLiteral(BEDROCKMAP_VERSION)}}}};
    } else if (method == "tools/list") {
        result = {{"tools", tools_->list()}};
    } else if (method == "tools/call") {
        const auto arguments = params.toObject();
        if (!arguments.value("name").isString() || (arguments.contains("arguments") && !arguments.value("arguments").isObject()))
            return rpcError(id, -32602, "Expected a tool name and an arguments object");
        result = tools_->call(arguments, writeEnabled_);
    } else if (method != "ping") {
        return rpcError(id, -32601, "Method not found");
    }
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}
