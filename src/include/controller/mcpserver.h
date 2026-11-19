#ifndef BEDROCKMAP_MCPSERVER_H
#define BEDROCKMAP_MCPSERVER_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <memory>

class AppController;
class QTcpServer;

class McpServer final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool authorizationRequired READ authorizationRequired WRITE setAuthorizationRequired NOTIFY changed)
    Q_PROPERTY(bool hasCustomToken READ hasCustomToken NOTIFY changed)
    Q_PROPERTY(bool writeEnabled READ writeEnabled WRITE setWriteEnabled NOTIFY changed)
    Q_PROPERTY(QString endpoint READ endpoint NOTIFY changed)
    Q_PROPERTY(QString token READ token NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    explicit McpServer(AppController& app);
    ~McpServer() override;
    bool supported() const;
    bool running() const;
    bool authorizationRequired() const { return authorizationRequired_; }
    bool hasCustomToken() const { return !tokenVerifier_.isEmpty(); }
    bool writeEnabled() const { return writeEnabled_; }
    QString endpoint() const;
    QString token() const { return token_; }
    QString error() const { return error_; }
    void setAuthorizationRequired(bool required);
    void setWriteEnabled(bool enabled);
    Q_INVOKABLE bool setCustomToken(const QString& token);
    Q_INVOKABLE bool start();
    Q_INVOKABLE void stop();

signals:
    void changed();

private:
    class Tools;
    bool authorized(const QByteArray& header) const;
    bool saveAuthorization(bool required, const QByteArray& verifier);
    void acceptConnections();
    QJsonObject dispatch(const QJsonObject& message);

    std::unique_ptr<QTcpServer> listener_;
    std::unique_ptr<Tools> tools_;
    QByteArray tokenVerifier_;
    QString token_;
    QString error_;
    bool authorizationRequired_{true};
    bool writeEnabled_{false};
#ifdef Q_OS_ANDROID
    qint64 keepAliveGeneration_{0};
#endif
};

#endif
