#ifndef BEDROCKMAP_WORLDENTITYCONTROLLER_H
#define BEDROCKMAP_WORLDENTITYCONTROLLER_H

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <vector>
#include "geometry.h"

class AsyncLevelLoader;
class ResourceManager;

class WorldEntityController final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(bool stale READ stale NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(qulonglong scannedKeys READ scannedKeys NOTIFY progressChanged)
    Q_PROPERTY(qulonglong entityCount READ entityCount NOTIFY stateChanged)
    Q_PROPERTY(int regionCount READ regionCount NOTIFY stateChanged)
    Q_PROPERTY(qulonglong skippedRecords READ skippedRecords NOTIFY stateChanged)

   public:
    explicit WorldEntityController(QObject* parent = nullptr);
    ~WorldEntityController() override;
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool loading() const { return loading_; }
    bool loaded() const { return loaded_; }
    bool stale() const { return stale_; }
    QString error() const { return error_; }
    qulonglong scannedKeys() const { return scanned_keys_; }
    qulonglong entityCount() const { return entity_count_; }
    int regionCount() const { return static_cast<int>(regions_.size()); }
    qulonglong skippedRecords() const { return skipped_records_; }
    void setLoader(AsyncLevelLoader* loader);
    void invalidate();
    void stopAndWait();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QVariantList entitiesForRegion(int row) const;

   signals:
    void stateChanged();
    void progressChanged();

   private:
    enum Role { Dimension = Qt::UserRole + 1, DimensionName, StartX, StartZ, RegionEntityCount, EntitySummary };
    struct Region {
        bl::chunk_pos pos;
        QString dimensionName;
        QString summary;
        QVariantList entities;
    };
    struct Result {
        std::vector<Region> regions;
        qulonglong entityCount{0};
        qulonglong skippedRecords{0};
        QString error;
    };
    static Result scan(AsyncLevelLoader* loader, const std::shared_ptr<const ResourceManager>& resources,
                       const std::atomic_bool& stop, const std::function<void(uint64_t)>& progress);
    AsyncLevelLoader* loader_{nullptr};
    QFutureWatcher<Result> watcher_;
    std::vector<Region> regions_;
    std::atomic_bool stop_{false};
    quint64 revision_{0};
    quint64 scan_revision_{0};
    quint64 scan_id_{0};
    qulonglong scanned_keys_{0};
    qulonglong entity_count_{0};
    qulonglong skipped_records_{0};
    bool loading_{false};
    bool loaded_{false};
    bool stale_{false};
    QString error_;
};

#endif
