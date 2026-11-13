#include "controller/worldentitycontroller.h"

#include <QtConcurrent>
#include <QMetaObject>
#include <QVariantMap>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <exception>
#include <map>
#include "base/resourcemanager.h"
#include "loader/asynclevelloader.h"

WorldEntityController::WorldEntityController(QObject* parent) : QAbstractListModel(parent) {
    connect(&watcher_, &QFutureWatcher<std::shared_ptr<Result>>::finished, this, [this] {
        if (!loading_ || !watcher_.isFinished()) return;
        loading_ = false;
        if (!stop_.load()) {
            auto result = watcher_.result();
            error_ = result->error;
            if (!error_.isEmpty() && loaded_) stale_ = true;
            if (error_.isEmpty()) {
                beginResetModel();
                regions_ = std::move(result->regions);
                resources_ = std::move(result->resources);
                entity_count_ = result->entityCount;
                skipped_records_ = result->skippedRecords;
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
        case RegionEntityCount: return static_cast<qulonglong>(region.entities.size());
        case EntitySummary: return region.summary;
        default: return {};
    }
}

QHash<int, QByteArray> WorldEntityController::roleNames() const {
    return {{Dimension, "regionDimension"}, {DimensionName, "dimensionName"}, {StartX, "startX"},
            {StartZ, "startZ"}, {RegionEntityCount, "regionEntityCount"}, {EntitySummary, "entitySummary"}};
}

QVariantList WorldEntityController::entitiesForRegion(int row) const {
    QVariantList entities;
    if (row < 0 || row >= rowCount()) return entities;
    const auto& records = regions_[row].entities;
    entities.reserve(static_cast<qsizetype>(records.size()));
    for (const auto& entity : records) {
        const auto id = QString::fromStdString(entity.identifier);
        entities.append(QVariantMap{{QStringLiteral("identifier"), id},
            {QStringLiteral("label"), resources_ ? resources_->actorLabel(id, QString::fromStdString(entity.customName)) : id},
            {QStringLiteral("icon"), resources_ ? resources_->actorIconUrl(id) : QString{}},
            {QStringLiteral("x"), entity.x}, {QStringLiteral("y"), entity.y}, {QStringLiteral("z"), entity.z}});
    }
    return entities;
}

std::optional<bl::chunk_pos> WorldEntityController::regionPosition(int row) const {
    if (row < 0 || row >= rowCount()) return std::nullopt;
    return regions_[row].pos;
}

std::vector<WorldEntityTarget> WorldEntityController::regionTargets(int row) const {
    std::vector<WorldEntityTarget> targets;
    if (row < 0 || row >= rowCount()) return targets;
    targets.reserve(regions_[row].entities.size());
    for (const auto& entity : regions_[row].entities) targets.push_back({entity.sourceChunk, entity.uid});
    return targets;
}

void WorldEntityController::removeDeletedRegion(int row, quint64 previousRevision) {
    if (row < 0 || row >= rowCount()) return;
    beginRemoveRows({}, row, row);
    entity_count_ -= regions_[row].entities.size();
    regions_.erase(regions_.begin() + row);
    endRemoveRows();
    stale_ = revision_ != previousRevision + 1;
    emit stateChanged();
}

void WorldEntityController::setError(const QString& error) {
    error_ = error;
    emit stateChanged();
}

QString WorldEntityController::progressText() const {
    if (loading_ && stop_.load()) return tr("Cancelling scan…");
    const auto processed = QString::number(progress_.processed);
    const auto entities = QString::number(progress_.entities);
    const auto total = QString::number(progress_.total.value_or(0));
    switch (progress_.stage) {
        case WorldEntityScanStage::Preparing:
            return tr("Reading pending entity edits… %1 entities").arg(entities);
        case WorldEntityScanStage::ModernEntities:
            return tr("Reading entity index… %1 entries, %2 records checked, %3 entities")
                .arg(processed, QString::number(progress_.actorReads), entities);
        case WorldEntityScanStage::LegacyEntities:
            if (progress_.total)
                return tr("Reading legacy entities… %1 / %2 entries, %3 entities").arg(processed, total, entities);
            return tr("Finding legacy entities… %1 keys, %2 entities").arg(processed, entities);
        case WorldEntityScanStage::Grouping:
            return tr("Grouping entities… %1 / %2").arg(processed, total);
        case WorldEntityScanStage::Summaries:
            return tr("Preparing region summaries… %1 / %2 entities").arg(processed, total);
    }
    return {};
}

void WorldEntityController::setLoader(AsyncLevelLoader* loader) {
    stopAndWait();
    if (loader_) disconnect(loader_, nullptr, this, nullptr);
    loader_ = loader;
    beginResetModel();
    regions_.clear();
    entity_count_ = skipped_records_ = 0;
    progress_ = {};
    resources_.reset();
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

void WorldEntityController::cancel() {
    stop_.store(true);
    emit progressChanged();
}

void WorldEntityController::refresh() {
    if (!loader_ || !loader_->isOpen() || loading_) return;
    stop_.store(false);
    loading_ = true;
    scan_revision_ = revision_;
    const auto scanId = ++scan_id_;
    progress_ = {};
    error_.clear();
    emit stateChanged();
    emit progressChanged();
    const auto resources = loader_->resources() ? loader_->resources()->snapshot() : nullptr;
    auto legacyChunks = loader_->legacyEntityChunks();
    watcher_.setFuture(QtConcurrent::run([this, loader = loader_, resources, legacyChunks = std::move(legacyChunks), scanId] {
        return scan(loader, resources, legacyChunks, stop_, [this, scanId](const WorldEntityScanProgress& state) {
            QMetaObject::invokeMethod(this, [this, state, scanId] {
                if (!loading_ || scan_id_ != scanId) return;
                progress_ = state;
                emit progressChanged();
            }, Qt::QueuedConnection);
        });
    }));
}

std::shared_ptr<WorldEntityController::Result> WorldEntityController::scan(AsyncLevelLoader* loader,
    const std::shared_ptr<const ResourceManager>& resources, const std::optional<std::vector<bl::chunk_pos>>& legacyChunks,
    const std::atomic_bool& stop, const std::function<void(const WorldEntityScanProgress&)>& progress) {
    auto result = std::make_shared<Result>();
    result->resources = resources;
    try {
        auto scanned = loader->scanWorldEntities(stop, progress, legacyChunks);
        if (stop.load()) return result;
        WorldEntityScanProgress state{WorldEntityScanStage::Grouping, 0, scanned.entities.size(), 0, scanned.entities.size()};
        auto lastReport = std::chrono::steady_clock::now();
        const auto report = [&](bool force = false) {
            const auto now = std::chrono::steady_clock::now();
            if (!force && now - lastReport < std::chrono::milliseconds(100)) return;
            progress(state);
            lastReport = now;
        };
        report(true);
        std::map<bl::chunk_pos, Region> regions;
        for (auto& entity : scanned.entities) {
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
            region.entities.push_back(std::move(entity));
            ++state.processed;
            report();
        }
        result->entityCount = scanned.entities.size();
        result->skippedRecords = scanned.skippedRecords;
        scanned.entities.clear();
        state.stage = WorldEntityScanStage::Summaries;
        state.processed = 0;
        state.total = result->entityCount;
        report(true);
        result->regions.reserve(regions.size());
        for (auto& [pos, region] : regions) {
            if (stop.load()) return result;
            std::map<std::string, uint64_t> counts;
            for (const auto& entity : region.entities) {
                if (stop.load()) return result;
                ++counts[entity.identifier];
                ++state.processed;
                report();
            }
            QStringList summary;
            for (const auto& [identifier, count] : counts) {
                const auto id = QString::fromStdString(identifier);
                const auto label = resources ? resources->actorLabel(id) : id;
                summary.append(label + QStringLiteral(" × ") + QString::number(count));
            }
            region.summary = summary.join(QStringLiteral("; "));
            result->regions.push_back(std::move(region));
            report();
        }
        report(true);
    } catch (const std::exception& error) {
        result->error = tr("Could not scan world entities: %1").arg(QString::fromUtf8(error.what()));
    } catch (...) {
        result->error = tr("Could not scan world entities.");
    }
    return result;
}
