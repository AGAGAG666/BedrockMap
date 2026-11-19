#include "mcpserver_p.h"

#include "base/apppaths.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <mutex>
#endif

namespace {
constexpr quint16 localPort = 8765;
constexpr qsizetype maxHeader = 16 * 1024;
constexpr qsizetype maxBody = 1024 * 1024;
constexpr int maxConnections = 16;
constexpr qsizetype saltSize = 32;
constexpr qsizetype digestSize = 32;

QString mcpSettingsPath() { return app_paths::configDirectory() + QStringLiteral("/mcp.ini"); }

QString generatedToken() {
    QByteArray random;
    for (int i = 0; i < 8; ++i) random += QByteArray::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0');
    return QString::fromLatin1(random);
}

QByteArray tokenDigest(const QByteArray& token, const QByteArray& salt) {
    return QCryptographicHash::hash(salt + token, QCryptographicHash::Sha256);
}
#ifdef Q_OS_ANDROID
constexpr char keepAliveClass[] = "io/github/hhhxiao/bedrockmap/McpKeepAliveService";
std::mutex keepAliveMutex;
McpServer* keepAliveTarget = nullptr;
qint64 nextKeepAliveGeneration = 0;
#endif

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
    const QSettings settings(mcpSettingsPath(), QSettings::IniFormat);
    authorizationRequired_ = settings.value(QStringLiteral("authorizationRequired"), true).toBool();
    const auto verifier = settings.value(QStringLiteral("tokenVerifier")).toByteArray();
    if (verifier.size() == saltSize + digestSize) tokenVerifier_ = verifier;
    listener_->setMaxPendingConnections(maxConnections);
    connect(listener_.get(), &QTcpServer::newConnection, this, &McpServer::acceptConnections);
#ifdef Q_OS_ANDROID
    const std::lock_guard lock(keepAliveMutex);
    keepAliveTarget = this;
#endif
}

McpServer::~McpServer() {
#ifdef Q_OS_ANDROID
    const std::lock_guard lock(keepAliveMutex);
    keepAliveTarget = nullptr;
#endif
}

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
#ifdef Q_OS_ANDROID
    QJniEnvironment env;
    static const bool registered = env.registerNativeMethods(keepAliveClass, {{"notifyStopped", "(JZ)V",
        reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong generation, jboolean failed) {
            const std::lock_guard lock(keepAliveMutex);
            if (auto* server = keepAliveTarget) {
                QMetaObject::invokeMethod(server, [server, generation, failed] {
                    if (server->keepAliveGeneration_ != generation) return;
                    server->keepAliveGeneration_ = 0;
                    server->stop();
                    if (failed) {
                        server->error_ = tr("Could not start Android MCP keep-alive.");
                        emit server->changed();
                    }
                }, Qt::QueuedConnection);
            }
        })}});
    const auto context = QNativeInterface::QAndroidApplication::context();
    const auto generation = ++nextKeepAliveGeneration;
    if (!registered || !context.isValid() || !QJniObject::callStaticMethod<jboolean>(
            keepAliveClass, "start", "(Landroid/content/Context;J)Z", context.object(), jlong(generation))) {
        listener_->close();
        error_ = tr("Could not start Android MCP keep-alive.");
        emit changed();
        return false;
    }
    keepAliveGeneration_ = generation;
#endif
    if (!hasCustomToken()) token_ = generatedToken();
    emit changed();
    return true;
}

void McpServer::stop() {
    listener_->close();
    for (auto* socket : listener_->findChildren<QTcpSocket*>()) socket->abort();
#ifdef Q_OS_ANDROID
    if (keepAliveGeneration_ != 0) {
        keepAliveGeneration_ = 0;
        const auto context = QNativeInterface::QAndroidApplication::context();
        if (context.isValid()) QJniObject::callStaticMethod<void>(
            keepAliveClass, "stop", "(Landroid/content/Context;)V", context.object());
    }
#endif
    if (!hasCustomToken()) token_.clear();
    writeEnabled_ = false;
    error_.clear();
    emit changed();
}

bool McpServer::saveAuthorization(bool required, const QByteArray& verifier) {
    QSettings settings(mcpSettingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("authorizationRequired"), required);
    if (verifier.isEmpty()) settings.remove(QStringLiteral("tokenVerifier"));
    else settings.setValue(QStringLiteral("tokenVerifier"), verifier);
    settings.sync();
    if (settings.status() == QSettings::NoError) {
        error_.clear();
        return true;
    }
    error_ = tr("Could not save MCP authorization settings.");
    emit changed();
    return false;
}

void McpServer::setAuthorizationRequired(bool required) {
    if (required == authorizationRequired_ || !saveAuthorization(required, tokenVerifier_)) return;
    authorizationRequired_ = required;
    emit changed();
}

bool McpServer::setCustomToken(const QString& token) {
    static const QRegularExpression bearerToken(QStringLiteral(R"(\A[A-Za-z0-9._~+/-]+=*\z)"));
    if (!token.isEmpty() && (token.size() > 512 || !bearerToken.match(token).hasMatch())) {
        error_ = tr("Use up to 512 characters allowed in a Bearer token, without spaces.");
        emit changed();
        return false;
    }
    QByteArray verifier;
    if (!token.isEmpty()) {
        const auto salt = QByteArray::fromHex(generatedToken().toLatin1());
        verifier = salt + tokenDigest(token.toLatin1(), salt);
    }
    if (!saveAuthorization(authorizationRequired_, verifier)) return false;
    tokenVerifier_ = verifier;
    token_ = token.isEmpty() && running() ? generatedToken() : token;
    emit changed();
    return true;
}

bool McpServer::authorized(const QByteArray& header) const {
    if (!authorizationRequired_) return true;
    if (header.first(qMin(qsizetype(7), header.size())).compare("Bearer ", Qt::CaseInsensitive) != 0) return false;
    const auto provided = header.sliced(7);
    if (provided.isEmpty() || provided.size() > 512) return false;
    if (!hasCustomToken()) return sameToken(provided, token_.toLatin1());
    return sameToken(tokenDigest(provided, tokenVerifier_.first(saltSize)), tokenVerifier_.sliced(saltSize));
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
            if (!running() || !authorized(headers.value("authorization"))) {
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
