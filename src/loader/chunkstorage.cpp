#include "loader/chunkstorage.h"
#include "loader/chunkcoordsservice.h"
#include <set>
#include <algorithm>

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
        auto& cached = cache_.cache_.at(pos);
        if (cached.entityData && !cached.shouldDelete) {
            for (const auto& [key, bytes] : cached.entityData->actors) cached.removedActorUids.insert(key);
            cached.entityData->legacy.clear();
            cached.entityData->actors.clear();
            return true;
        }
        raw = cache_.getRawChunk(pos);
    } else {
        bl::raw_chunk stored(pos);
        if (stored.read(level_)) raw = std::move(stored);
    }
    if (!raw) return true;
    cache_.clearChunkEntities(pos, std::move(*raw));
    return true;
}

WorldEntityDeleteResult ChunkStorage::deleteRegionEntities(const std::vector<WorldEntityTarget>& targets, const bl::chunk_pos& region) {
    using Error = WorldEntityDeleteResult::Error;
    struct Plan {
        bl::chunk_pos source;
        std::optional<bl::raw_chunk> chunk;
        std::optional<RawChunkCache::EntityData> entities;
        std::set<std::string> removed;
        uint64_t missingReferences;
    };
    const std::lock_guard lock(mutex_);
    LOG_F(INFO, "Entity deletion started: targets=%llu", static_cast<unsigned long long>(targets.size()));
    std::map<bl::chunk_pos, std::vector<int64_t>> byChunk;
    for (const auto& target : targets) {
        if (target.sourceChunk.dim != region.dim) return {Error::DimensionMismatch, target.sourceChunk, {}};
        byChunk[target.sourceChunk].push_back(target.uid);
    }
    std::vector<Plan> plans;
    const double left = region.x * 16.0, top = region.z * 16.0;
    for (auto& [pos, ids] : byChunk) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        LOG_F(INFO, "Entity deletion source: chunk=%s targets=%llu", pos.to_string().c_str(),
              static_cast<unsigned long long>(ids.size()));
        std::vector<uint8_t> found(ids.size(), 0);
        size_t foundCount = 0;
        std::vector<std::unique_ptr<bl::actor>> retainedActors;
        WorldEntityDeleteResult failure;
        const auto filter = [&](std::string_view bytes, std::string* retainedBytes) {
            size_t offset = 0;
            while (offset < bytes.size()) {
                const auto input = bytes.substr(offset);
                auto parsed = bl::nbt::parse_one(std::span<const byte_t>(input.data(), input.size()));
                std::unique_ptr<bl::nbt::compound_tag> root(parsed.root);
                if (!parsed || parsed.consumed_length == 0 || parsed.consumed_length > input.size()) {
                    failure = {Error::InvalidNbt, pos, bl::nbt::parse_error_to_string(parsed.error_code)};
                    return false;
                }
                auto actor = std::make_unique<bl::actor>();
                if (!actor->load_from_nbt_owned(root.get())) {
                    failure = {Error::InvalidActor, pos, {}};
                    return false;
                }
                root.release();
                const auto target = std::lower_bound(ids.begin(), ids.end(), actor->uid());
                if (target == ids.end() || *target != actor->uid()) {
                    if (retainedBytes) retainedBytes->append(input.data(), parsed.consumed_length);
                    else retainedActors.push_back(std::move(actor));
                } else {
                    const auto position = actor->pos();
                    if (!(position.x >= left && position.x < left + 16 && position.z >= top && position.z < top + 16)) {
                        failure = {Error::TargetMoved, pos, {}};
                        return false;
                    }
                    const auto index = static_cast<size_t>(target - ids.begin());
                    if (!found[index]) { found[index] = 1; ++foundCount; }
                }
                offset += parsed.consumed_length;
            }
            return true;
        };
        const auto cached = cache_.cache_.find(pos);
        const bool pendingEntities = cached != cache_.cache_.end() && !cached->second.shouldDelete && cached->second.entityData;
        std::optional<bl::raw_chunk> raw;
        if (!pendingEntities) {
            if (cached != cache_.cache_.end()) {
                if (!cached->second.shouldDelete && cached->second.chunk) raw.emplace(*cached->second.chunk);
            } else {
                raw.emplace(pos);
                if (!raw->read(level_)) raw.reset();
            }
        }
        uint64_t missingReferences = 0;
        std::set<std::string> removed;
        if (pendingEntities || (!raw && cached == cache_.cache_.end())) {
            RawChunkCache::EntityData entities;
            const auto keepActor = [&](const std::string& key, std::string_view bytes) {
                std::string remaining;
                if (!filter(bytes, &remaining)) return false;
                if (remaining.empty()) removed.insert(key);
                else entities.actors.emplace(key, std::move(remaining));
                return true;
            };
            if (pendingEntities) {
                const auto& previous = *cached->second.entityData;
                if (!filter(previous.legacy, &entities.legacy)) return failure;
                for (const auto& [key, bytes] : previous.actors)
                    if (!keepActor(key, bytes)) return failure;
            } else {
                const auto options = level_.bulk_read_options();
                for (const auto type : bl::raw_chunk::MARKER_KEYS) {
                    std::string value;
                    const auto status = level_.db()->Get(options, bl::chunk_key{type, pos}.to_raw(), &value);
                    if (!status.ok() && !status.IsNotFound()) return {Error::DatabaseRead, pos, status.ToString()};
                }
                const auto read = [&](const std::string& key, std::string& value) -> WorldEntityDeleteResult {
                    const auto status = level_.db()->Get(options, key, &value);
                    if (status.ok() || status.IsNotFound()) return {};
                    return {Error::DatabaseRead, pos, status.ToString()};
                };
                std::string legacy;
                auto status = read(bl::chunk_key{bl::chunk_key::Entity, pos}.to_raw(), legacy);
                if (!status.success()) return status;
                if (!filter(legacy, &entities.legacy)) return failure;
                std::string digest;
                status = read(bl::actor_digest_key{pos}.to_raw(), digest);
                if (!status.success()) return status;
                if (digest.size() % 8 != 0) return {Error::InvalidDigest, pos, {}};
                for (size_t offset = 0; offset < digest.size(); offset += 8) {
                    const auto key = digest.substr(offset, 8);
                    std::string bytes;
                    status = read(std::string(bl::storage_key::actor) + key, bytes);
                    if (!status.success()) return status;
                    if (bytes.empty()) { ++missingReferences; continue; }
                    if (!keepActor(key, bytes)) return failure;
                }
            }
            if (foundCount != ids.size()) return {Error::TargetMissing, pos, {}};
            plans.push_back({pos, std::nullopt, std::move(entities), std::move(removed), missingReferences});
        } else {
            if (!raw) return {Error::SourceChunkUnavailable, pos, {}};
            if (!filter(raw->get_normal_key(bl::chunk_key::Entity), nullptr)) return failure;
            const auto& digest = raw->get_actor_digest();
            if (digest.size() % 8 != 0) return {Error::InvalidDigest, pos, {}};
            for (size_t offset = 0; offset < digest.size(); offset += 8) {
                const auto key = digest.substr(offset, 8);
                if (raw->get_entities().contains(key)) continue;
                std::string value;
                const auto status = level_.db()->Get(level_.bulk_read_options(), std::string(bl::storage_key::actor) + key, &value);
                if (status.IsNotFound() || (status.ok() && value.empty())) { ++missingReferences; continue; }
                if (!status.ok()) return {Error::DatabaseRead, pos, status.ToString()};
                return {Error::TargetMissing, pos, {}};
            }
            for (const auto& [key, bytes] : raw->get_entities()) {
                removed.insert(key);
                if (!filter(bytes, nullptr)) return failure;
            }
            if (foundCount != ids.size()) return {Error::TargetMissing, pos, {}};
            std::vector<bl::actor*> remaining;
            remaining.reserve(retainedActors.size());
            for (const auto& actor : retainedActors) remaining.push_back(actor.get());
            raw->set_entities(remaining);
            for (const auto& [key, bytes] : raw->get_entities()) removed.erase(key);
            plans.push_back({pos, std::move(*raw), std::nullopt, std::move(removed), missingReferences});
        }
    }
    for (auto& plan : plans) {
        const auto pos = plan.source;
        if (plan.entities) cache_.putEntities(pos, std::move(*plan.entities), std::move(plan.removed));
        else {
            cache_.putChunk(pos, *plan.chunk);
            if (!plan.chunk->get_normal_key(bl::chunk_key::Entity).empty()) added_legacy_entity_chunks_.insert(pos);
            cache_.cache_.at(pos).removedActorUids.merge(plan.removed);
        }
        if (plan.missingReferences > 0)
            LOG_F(INFO, "Entity deletion removed %llu stale digest references in chunk %s",
                  static_cast<unsigned long long>(plan.missingReferences), pos.to_string().c_str());
    }
    LOG_F(INFO, "Entity deletion completed: targets=%llu sources=%llu", static_cast<unsigned long long>(targets.size()),
          static_cast<unsigned long long>(plans.size()));
    return {};
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
    std::map<bl::chunk_pos, RawChunkCache::EntityData> pending;
    std::set<bl::chunk_pos> legacyPositions;
    if (legacyChunks) legacyPositions.insert(legacyChunks->begin(), legacyChunks->end());
    {
        const std::lock_guard lock(mutex_);
        snapshot.reset(db->GetSnapshot());
        legacyPositions.insert(added_legacy_entity_chunks_.begin(), added_legacy_entity_chunks_.end());
        for (const auto& [pos, cached] : cache_.cache_) {
            auto& entry = pending[pos];
            if (cached.shouldDelete) continue;
            if (cached.entityData) entry = *cached.entityData;
            else if (cached.chunk) {
                entry.legacy = cached.chunk->get_normal_key(bl::chunk_key::Entity);
                entry.actors = cached.chunk->get_entities();
            }
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
