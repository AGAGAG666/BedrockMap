#ifndef BEDROCKMAP_MCPSERVER_P_H
#define BEDROCKMAP_MCPSERVER_P_H

#include "controller/mcpserver.h"
#include <QJsonArray>
#include <QJsonValue>
#include <QMap>
#include <functional>

class McpServer::Tools final : public QObject {
public:
    explicit Tools(AppController& app);
    QJsonArray list() const;
    QJsonObject call(const QJsonObject& params, bool writeAllowed);

private:
    struct Tool {
        QJsonObject schema;
        bool write;
        std::function<QJsonValue(const QJsonObject&)> action;
    };
    AppController& app_;
    QMap<QString, Tool> tools_;
    struct Job {
        QJsonObject state;
        QObject* observer;
    };
    QMap<QString, Job> jobs_;
    void add(const char* name, const char* description, const char* arguments, bool write,
             std::function<QJsonValue(const QJsonObject&)> action);
    QString startJob(QObject* owner);
    void registerWorldTools();
    void registerDocumentTools();
    void registerAppTools();
    void finishJob(const QString& id, const QString& error = {});
};

#endif
