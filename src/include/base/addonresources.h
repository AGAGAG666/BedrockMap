#ifndef BEDROCKMAP_ADDONRESOURCES_H
#define BEDROCKMAP_ADDONRESOURCES_H

#include <QColor>
#include <QCoreApplication>
#include <QHash>
#include <QImage>
#include <QList>
#include <QStringList>
#include <QUrl>
#include <memory>

struct AddonEntityResource {
    QString name;
    QImage icon;
    QString iconUrl;
};

class AddonResources {
    Q_DECLARE_TR_FUNCTIONS(AddonResources)
   public:
    QHash<QString, AddonEntityResource> entities;
    QHash<QString, QColor> blockColors;
    QStringList warnings;
    QString error;
    int packCount{0};

    static std::shared_ptr<AddonResources> load(const QList<QUrl>& sources = {}, const QString& worldPath = {});
};

#endif
