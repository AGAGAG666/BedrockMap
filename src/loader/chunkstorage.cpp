#include "loader/chunkstorage.h"
#include "loader/chunkcoordsservice.h"
#include <set>

#include <leveldb/write_batch.h>

#include <memory>
#include <cmath>
#include <chrono>
#include <limits>
#include <stdexcept>
#include "actor.h"

#include "io/documentio.h"
#include "loguru/loguru.hpp"

bl::chunk* ChunkStorage::getChunk(const bl::chunk_pos& pos, bl::chunk_load_policy policy) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.hasChunk(pos)) return cache_.getChunk(pos, policy);
    }
    return level_.get_chunk(pos, policy);
}

std::optional<bl::raw_chunk> ChunkStorage::getRawChunk(const bl::chunk_pos& pos) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.hasChunk(pos)) return cache_.getRawChunk(pos);
    }
    bl::raw_chunk raw(pos);
    if (raw.read(level_)) return raw;
    return std::nullopt;
}

void ChunkStorage::putMissing(const bl::chunk_pos& pos) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.putMissing(pos);
}

void ChunkStorage::putRawChunk(const bl::raw_chunk& raw) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.putChunk(raw.pos(), raw);
    if (!raw.get_normal_key(bl::chunk_key::Entity).empty()) added_legacy_entity_chunks_.insert(raw.pos());
}

bool ChunkStorage::clearChunkEntities(const bl::chunk_pos& pos) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<bl::raw_chunk> raw;
    if (cache_.hasChunk(pos)) {
        raw = cache_.getRawChunk(pos);
    } else {
        bl::raw_chunk stored(pos);
        if (stored.read(level_)) raw = std::move(stored);
    }
    if (!raw) return true;
    cache_.clearChunkEntities(pos, std::move(*raw));
    return true;
}

bool ChunkStorage::deleteRegionEntities(const std::vector<WorldEntityTarget>& targets, const bl::chunk_pos& region) {
    struct Plan {
        bl::raw_chunk chunk;
        std::set<std::string> removed;
    };
    const std::lock_guard lock(mutex_);
    std::map<bl::chunk_pos, std::set<int64_t>> byChunk;
    for (const auto& target : targets) {
        if (target.sourceChunk.dim != region.dim) return false;
        byChunk[target.sourceChunk].insert(target.uid);
    }
    std::vector<Plan> plans;
    const double left = region.x * 16.0, top = region.z * 16.0;
    for (const auto& [pos, ids] : byChunk) {
        auto raw = [&]() -> std::optional<bl::raw_chunk> {
            if (cache_.hasChunk(pos)) return cache_.getRawChunk(pos);
            bl::raw_chunk stored(pos);
            if (!stored.read(level_)) return std::nullopt;
            return stored;
        }();
        if (!raw) return false;
        std::vector<std::unique_ptr<bl::actor>> actors;
        const auto append = [&](std::string_view bytes) {
            if (bytes.empty()) return true;
            auto parsed = bl::nbt::parse_all(std::span<const byte_t>(bytes.data(), bytes.size()));
            if (!parsed) return false;
            std::vector<std::unique_ptr<bl::nbt::compound_tag>> roots;
            for (auto* root : parsed.roots) roots.emplace_back(root);
            for (auto& root : roots) {
                auto actor = std::make_unique<bl::actor>();
                if (!actor->load_from_nbt_owned(root.get())) return false;
                root.release();
                actors.push_back(std::move(actor));
            }
            return true;
        };
        if (!append(raw->get_normal_key(bl::chunk_key::Entity))) return false;
        bl::actor_digest_list digest;
        if (!digest.load(raw->get_actor_digest())) return false;
        for (const auto& key : digest.actor_digests_) if (!raw->get_entities().contains(key)) return false;
        std::set<std::string> previousKeys;
        for (const auto& [key, bytes] : raw->get_entities()) {
            previousKeys.insert(key);
            if (!append(bytes)) return false;
        }
        std::set<int64_t> found;
        std::vector<bl::actor*> remaining;
        for (const auto& actor : actors) {
            if (!ids.contains(actor->uid())) { remaining.push_back(actor.get()); continue; }
            const auto position = actor->pos();
            if (!(position.x >= left && position.x < left + 16 && position.z >= top && position.z < top + 16)) return false;
            found.insert(actor->uid());
        }
        if (found != ids) return false;
        raw->set_entities(remaining);
        std::set<std::string> removed;
        for (const auto& key : previousKeys) if (!raw->get_entities().contains(key)) removed.insert(key);
        plans.push_back({std::move(*raw), std::move(removed)});
    }
    for (auto& plan : plans) {
        const auto pos = plan.chunk.pos();
        cache_.putChunk(pos, plan.chunk);
        if (!plan.chunk.get_normal_key(bl::chunk_key::Entity).empty()) added_legacy_entity_chunks_.insert(pos);
        auto& removed = cache_.cache_.at(pos).removedActorUids;
        removed.insert(plan.removed.begin(), plan.removed.end());
    }
    return true;
}

std::vector<bl::chunk_pos> ChunkStorage::pendingChunkPositions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<bl::chunk_pos> result;
    for (const auto& [pos, entry] : cache_.cache_) result.push_back(pos);
    return result;
}

void ChunkStorage::replayChunkIndex(ChunkCoordsService& coords, const std::vector<bl::chunk_pos>& beforeScan) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<bl::chunk_pos> positions(beforeScan.begin(), beforeScan.end());
    for (const auto& [pos, entry] : cache_.cache_) positions.insert(pos);
    for (const auto& pos : positions) {
        std::optional<bl::raw_chunk> raw;
        if (cache_.hasChunk(pos)) raw = cache_.getRawChunk(pos);
        else {
            bl::raw_chunk stored(pos);
            if (stored.read(level_)) raw = std::move(stored);
        }
        coords.update(pos, raw.has_value());
        if (raw) coords.updateOverview(*raw);
    }
}

WorldEntityScanResult ChunkStorage::scanWorldEntities(const std::atomic_bool& stop,
    const std::function<void(const WorldEntityScanProgress&)>& progress,
    const std::optional<std::vector<bl::chunk_pos>>& legacyChunks) {
    struct PendingEntities {
        std::string legacy;
        std::map<std::string, std::string> actors;
    };
    WorldEntityScanResult result;
    WorldEntityScanProgress state;
    auto lastReport = std::chrono::steady_clock::now();
    const auto report = [&](bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastReport < std::chrono::milliseconds(100)) return;
        state.entities = result.entities.size();
        progress(state);
        lastReport = now;
    };
    report(true);
    auto* db = level_.db();
    if (!db) throw std::runtime_error("The world database is closed.");
    const auto release = [db](const leveldb::Snapshot* snapshot) { if (snapshot) db->ReleaseSnapshot(snapshot); };
    std::unique_ptr<const leveldb::Snapshot, decltype(release)> snapshot(nullptr, release);
    std::map<bl::chunk_pos, PendingEntities> pending;
    std::set<bl::chunk_pos> legacyPositions;
    if (legacyChunks) legacyPositions.insert(legacyChunks->begin(), legacyChunks->end());
    {
        const std::lock_guard lock(mutex_);
        snapshot.reset(db->GetSnapshot());
        legacyPositions.insert(added_legacy_entity_chunks_.begin(), added_legacy_entity_chunks_.end());
        for (const auto& [pos, cached] : cache_.cache_) {
            auto& entry = pending[pos];
            if (cached.shouldDelete || !cached.chunk) continue;
            entry.legacy = cached.chunk->get_normal_key(bl::chunk_key::Entity);
            entry.actors = cached.chunk->get_entities();
        }
    }
    auto options = level_.bulk_read_options();
    options.snapshot = snapshot.get();
    std::unordered_set<int64_t> seen;
    const auto append = [&](std::string_view raw, const bl::chunk_pos& source) {
        if (raw.empty() || stop.load()) return;
        auto parsed = bl::nbt::parse_all(std::span<const byte_t>(raw.data(), raw.size()));
        if (!parsed) { ++result.skippedRecords; return; }
        for (auto* tag : parsed.roots) {
            std::unique_ptr<bl::nbt::compound_tag> root(tag);
            if (stop.load()) continue;
            bl::actor actor;
            if (!actor.load_from_nbt_owned(root.get())) { ++result.skippedRecords; continue; }
            root.release();
            const auto pos = actor.pos();
            const auto coordinateValid = [](float value) {
                return std::isfinite(value) && static_cast<double>(value) >= std::numeric_limits<int>::min() &&
                       static_cast<double>(value) < static_cast<double>(std::numeric_limits<int>::max()) - 16;
            };
            if (!coordinateValid(pos.x) || !coordinateValid(pos.z) || !std::isfinite(pos.y)) {
                ++result.skippedRecords;
                continue;
            }
            if (!seen.insert(actor.uid()).second) continue;
            std::string name;
            if (const auto* custom = actor.root()->get("CustomName")) {
                if (const auto* value = custom->as<const bl::nbt::string_tag*>()) name = value->value;
            }
            result.entities.push_back({source.dim, pos.x, pos.y, pos.z, std::string(actor.identifier()), std::move(name), source, actor.uid()});
            report();
        }
    };
    // Pending chunks take precedence over every stored copy of the same actor.
    for (const auto& [pos, entry] : pending) {
        if (stop.load()) return result;
        append(entry.legacy, pos);
        for (const auto& [uid, raw] : entry.actors) {
            if (stop.load()) return result;
            append(raw, pos);
        }
    }
    state.stage = WorldEntityScanStage::ModernEntities;
    state.processed = 0;
    report(true);
    const leveldb::Slice prefix(bl::storage_key::actor_digest.data(), bl::storage_key::actor_digest.size());
    std::unique_ptr<leveldb::Iterator> iterator(db->NewIterator(options));
    for (iterator->Seek(prefix); iterator->Valid() && iterator->key().starts_with(prefix) && !stop.load(); iterator->Next()) {
        ++state.keys;
        ++state.processed;
        report();
        const auto key = iterator->key();
        const auto digestKey = bl::actor_digest_key::parse(std::string_view(key.data(), key.size()));
        if (!digestKey.valid()) { ++result.skippedRecords; continue; }
        if (pending.contains(digestKey.cp)) continue;
        bl::actor_digest_list digest;
        if (!digest.load(std::string_view(iterator->value().data(), iterator->value().size()))) {
            ++result.skippedRecords;
            continue;
        }
        for (const auto& uid : digest.actor_digests_) {
            if (stop.load()) return result;
            std::string raw;
            const auto status = db->Get(options, std::string(bl::storage_key::actor) + uid, &raw);
            ++state.actorReads;
            report();
            if (status.IsNotFound()) { ++result.skippedRecords; continue; }
            if (!status.ok()) throw std::runtime_error(status.ToString());
            if (raw.empty()) { ++result.skippedRecords; continue; }
            append(raw, digestKey.cp);
        }
    }
    if (!iterator->status().ok()) throw std::runtime_error(iterator->status().ToString());
    if (stop.load()) return result;
    state.stage = WorldEntityScanStage::LegacyEntities;
    state.processed = 0;
    if (legacyChunks) state.total = legacyPositions.size();
    report(true);
    if (legacyChunks) {
        for (const auto& pos : legacyPositions) {
            if (stop.load()) return result;
            ++state.keys;
            ++state.processed;
            report();
            if (pending.contains(pos)) continue;
            std::string raw;
            const auto status = db->Get(options, bl::chunk_key{bl::chunk_key::Entity, pos}.to_raw(), &raw);
            if (status.IsNotFound()) continue;
            if (!status.ok()) throw std::runtime_error(status.ToString());
            append(raw, pos);
        }
    } else {
        // Without a preloaded index, discover legacy keys while skipping modern actor runs.
        auto actorEnd = std::string(bl::storage_key::actor);
        auto digestEnd = std::string(bl::storage_key::actor_digest);
        ++actorEnd.back();
        ++digestEnd.back();
        for (iterator->SeekToFirst(); iterator->Valid() && !stop.load();) {
            ++state.keys;
            ++state.processed;
            report();
            const auto key = iterator->key();
            const std::string_view keyView(key.data(), key.size());
            const auto chunkKey = bl::chunk_key::parse(keyView);
            if (chunkKey.valid()) {
                if (chunkKey.type == bl::chunk_key::Entity && !pending.contains(chunkKey.cp))
                    append(std::string_view(iterator->value().data(), iterator->value().size()), chunkKey.cp);
            } else if (keyView.starts_with(bl::storage_key::actor)) {
                iterator->Seek(actorEnd);
                continue;
            } else if (keyView.starts_with(bl::storage_key::actor_digest)) {
                iterator->Seek(digestEnd);
                continue;
            }
            iterator->Next();
        }
        if (!iterator->status().ok()) throw std::runtime_error(iterator->status().ToString());
    }
    report(true);
    return result;
}

bool ChunkStorage::commit(const std::unordered_map<std::string, std::string>& globalModifies, const bl::nbt::compound_tag* levelDat) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_commit_error_ = CommitError::None;
    if (cache_.empty() && globalModifies.empty() && !levelDat) return true;

    leveldb::WriteBatch batch;
    if (!cache_.empty()) cache_.commit(level_, batch);
    for (const auto& kv : globalModifies) {
        if (kv.second.empty()) {
            batch.Delete(kv.first);
        } else {
            batch.Put(kv.first, kv.second);
        }
    }

    // Serialize before touching either store. The copied root is installed in
    // memory only after the file write succeeds.
    std::string levelDatRaw;
    std::unique_ptr<bl::nbt::compound_tag> levelDatCopy;
    if (levelDat) {
        levelDatCopy.reset(dynamic_cast<bl::nbt::compound_tag*>(levelDat->copy()));
        if (!levelDatCopy) {
            last_commit_error_ = CommitError::LevelDatSerialization;
            LOG_F(ERROR, "Failed to copy level.dat NBT before commit");
            return false;
        }
        levelDatRaw = level_.dat().header() + levelDatCopy->serialize();
    }

    if (!cache_.empty() || !globalModifies.empty()) {
        const auto status = level_.db()->Write(leveldb::WriteOptions(), &batch);
        if (!status.ok()) {
            last_commit_error_ = CommitError::LevelDb;
            LOG_F(ERROR, "Failed to commit LevelDB changes: %s", status.ToString().c_str());
            return false;
        }
    }

    if (levelDat) {
        // level.dat is an ordinary file, so it goes through the VFS rather than LevelDB's Env.
        const auto levelDatPath = QString::fromStdString(level_.root_path() + "/" + bl::bedrock_level::LEVEL_DATA);
        const QString error = io::DocumentIo::writeBytes(QUrl::fromLocalFile(levelDatPath),
                                                         QByteArray(levelDatRaw.data(), static_cast<qsizetype>(levelDatRaw.size())), {false});
        if (!error.isEmpty()) {
            LOG_F(ERROR, "Cannot save level.dat: %s", error.toStdString().c_str());
            last_commit_error_ = CommitError::LevelDatWrite;
            if (!cache_.empty() || !globalModifies.empty()) {
                LOG_F(ERROR, "LevelDB changes committed, but level.dat could not be replaced; edits remain dirty for retry");
            }
            return false;
        }
    }

    if (levelDatCopy) level_.dat().set_nbt(levelDatCopy.release());
    cache_.clear();
    return true;
}
