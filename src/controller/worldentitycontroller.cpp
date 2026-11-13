#include "controller/worldentitycontroller.h"

#include <QtConcurrent>
#include <QMetaObject>
#include <QVariantMap>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include "base/resourcemanager.h"
#include "loader/asynclevelloader.h"

WorldEntityController::WorldEntityController(QObject* parent) : QAbstractListModel(parent) {
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        if (!loading_ || !watcher_.isFinished()) return;
        loading_ = false;
        if (!stop_.load()) {
            auto result = watcher_.result();
            error_ = result.error;
            if (!error_.isEmpty() && loaded_) stale_ = true;
            if (error_.isEmpty()) {
                beginResetModel();
                regions_ = std::move(result.regions);
                entity_count_ = result.entityCount;
                skipped_records_ = result.skippedRecords;
                endResetModel();
                loaded_ = true;
                stale_ = revision_ != scan_revision_;
            }
        }
        emit stateChanged();
    });
}

WorldEntityController::~WorldEntityController() { stopAndWait(); }

int WorldEntityController::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(regions_.size());
}

QVariant WorldEntityController::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& region = regions_[index.row()];
    switch (role) {
        case Dimension: return region.pos.dim;
        case DimensionName: return region.dimensionName;
        case StartX: return region.pos.x * 16;
        case StartZ: return region.pos.z * 16;
        case RegionEntityCount: return region.entities.size();
        case EntitySummary: return region.summary;
        default: return {};
    }
}

QHash<int, QByteArray> WorldEntityController::roleNames() const {
    return {{Dimension, "regionDimension"}, {DimensionName, "dimensionName"}, {StartX, "startX"},
            {StartZ, "startZ"}, {RegionEntityCount, "regionEntityCount"}, {EntitySummary, "entitySummary"}};
}

QVariantList WorldEntityController::entitiesForRegion(int row) const {
    return row >= 0 && row < rowCount() ? regions_[row].entities : QVariantList{};
}

void WorldEntityController::setLoader(AsyncLevelLoader* loader) {
    stopAndWait();
    if (loader_) disconnect(loader_, nullptr, this, nullptr);
    loader_ = loader;
    beginResetModel();
    regions_.clear();
    entity_count_ = skipped_records_ = scanned_keys_ = 0;
    loaded_ = stale_ = false;
    error_.clear();
    endResetModel();
    if (loader_) connect(loader_, &AsyncLevelLoader::dirtyChanged, this, &WorldEntityController::invalidate);
    emit stateChanged();
    emit progressChanged();
}

void WorldEntityController::invalidate() {
    ++revision_;
    stale_ = loaded_ || loading_;
    emit stateChanged();
}

void WorldEntityController::stopAndWait() {
    stop_.store(true);
    if (watcher_.isRunning()) watcher_.waitForFinished();
    loading_ = false;
    ++scan_id_;
}

void WorldEntityController::cancel() { stop_.store(true); }

void WorldEntityController::refresh() {
    if (!loader_ || !loader_->isOpen() || loading_) return;
    stop_.store(false);
    loading_ = true;
    scan_revision_ = revision_;
    const auto scanId = ++scan_id_;
    scanned_keys_ = 0;
    error_.clear();
    emit stateChanged();
    emit progressChanged();
    const auto resources = loader_->resources() ? loader_->resources()->snapshot() : nullptr;
    watcher_.setFuture(QtConcurrent::run([this, loader = loader_, resources, scanId] {
        return scan(loader, resources, stop_, [this, scanId](uint64_t count) {
            QMetaObject::invokeMethod(this, [this, count, scanId] {
                if (!loading_ || scan_id_ != scanId) return;
                scanned_keys_ = count;
                emit progressChanged();
            }, Qt::QueuedConnection);
        });
    }));
}

WorldEntityController::Result WorldEntityController::scan(AsyncLevelLoader* loader,
    const std::shared_ptr<const ResourceManager>& resources, const std::atomic_bool& stop,
    const std::function<void(uint64_t)>& progress) {
    Result result;
    try {
        auto scanned = loader->scanWorldEntities(stop, progress);
        if (stop.load()) return result;
        std::map<bl::chunk_pos, Region> regions;
        for (const auto& entity : scanned.entities) {
            if (stop.load()) return result;
            const bl::chunk_pos pos(static_cast<int>(std::floor(static_cast<double>(entity.x) / 16)),
                                    static_cast<int>(std::floor(static_cast<double>(entity.z) / 16)), entity.dimension);
            auto [it, inserted] = regions.try_emplace(pos);
            auto& region = it->second;
            if (inserted) {
                region.pos = pos;
                switch (pos.dim) {
                    case 0: region.dimensionName = tr("Overworld"); break;
                    case 1: region.dimensionName = tr("Nether"); break;
                    case 2: region.dimensionName = tr("The End"); break;
                    default: {
                        region.dimensionName = tr("Dimension %1").arg(pos.dim);
                        for (const auto& [name, id] : loader->level().custom_dimension_table()) {
                            if (id == pos.dim) { region.dimensionName = QString::fromStdString(name); break; }
                        }
                    }
                }
            }
            const auto id = QString::fromStdString(entity.identifier);
            const auto customName = QString::fromStdString(entity.customName);
            region.entities.append(QVariantMap{{QStringLiteral("identifier"), id},
                {QStringLiteral("label"), resources ? resources->actorLabel(id, customName) : id},
                {QStringLiteral("icon"), resources ? resources->actorIconUrl(id) : QString{}},
                {QStringLiteral("x"), entity.x}, {QStringLiteral("y"), entity.y}, {QStringLiteral("z"), entity.z}});
        }
        result.entityCount = scanned.entities.size();
        result.skippedRecords = scanned.skippedRecords;
        result.regions.reserve(regions.size());
        for (auto& [pos, region] : regions) {
            std::map<QString, int> counts;
            for (const auto& entity : region.entities) ++counts[entity.toMap().value(QStringLiteral("label")).toString()];
            QStringList summary;
            for (const auto& [label, count] : counts) summary.append(label + QStringLiteral(" × ") + QString::number(count));
            region.summary = summary.join(QStringLiteral("; "));
            result.regions.push_back(std::move(region));
        }
    } catch (const std::exception& error) {
        result.error = tr("Could not scan world entities: %1").arg(QString::fromUtf8(error.what()));
    } catch (...) {
        result.error = tr("Could not scan world entities.");
    }
    return result;
}
