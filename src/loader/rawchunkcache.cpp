#include "loader/rawchunkcache.h"

#include "chunk.h"
#include "loguru/loguru.hpp"

std::pair<int, int> RawChunkCache::chunkCounts() const {
    int empty = 0, nonEmpty = 0;
    for (const auto& kv : cache_) {
        if (kv.second.entityData && !kv.second.shouldDelete) continue;
        if (!kv.second.shouldDelete && kv.second.chunk && !kv.second.chunk->get_sub_chunks().empty()) {
            nonEmpty++;
        } else {
            empty++;
        }
    }
    return {empty, nonEmpty};
}

void RawChunkCache::putChunk(const bl::chunk_pos& pos, const bl::raw_chunk& chunk) {
    auto previous = cache_.find(pos);
    std::set<std::string> removed;
    bl::raw_chunk replacement(chunk);
    if (previous != cache_.end()) {
        removed = previous->second.removedActorUids;
        if (previous->second.entityData) {
            const auto& entities = *previous->second.entityData;
            for (const auto& [key, bytes] : entities.actors) removed.insert(key);
            const bool replacesEntities = chunk.get_normal_data().contains(bl::chunk_key::Entity) ||
                !chunk.get_actor_digest().empty() || !chunk.get_entities().empty();
            if (!previous->second.shouldDelete && !replacesEntities) {
                std::string payload = entities.legacy;
                for (const auto& [key, bytes] : entities.actors) payload += bytes;
                bl::raw_chunk entitySource(pos);
                entitySource.set_normal(bl::chunk_key::Entity, payload);
                bl::chunk parsed(pos);
                parsed.load_from_raw_chunk(entitySource, bl::chunk_load_policy::Actor);
                replacement.set_entities(parsed.entities());
            } else replacement.set_normal(bl::chunk_key::Entity, chunk.get_normal_key(bl::chunk_key::Entity));
        }
    }
    cache_.insert_or_assign(pos, CachedChunk{replacement, false, {}, std::nullopt});
    cache_.at(pos).removedActorUids = std::move(removed);
}

void RawChunkCache::putEntities(const bl::chunk_pos& pos, EntityData entities, std::set<std::string> removed) {
    auto& entry = cache_[pos];
    entry.entityData = std::move(entities);
    entry.removedActorUids.merge(removed);
}

void RawChunkCache::clearChunkEntities(const bl::chunk_pos& pos, bl::raw_chunk chunk) {
    std::set<std::string> removed;
    for (const auto& [uid, raw] : chunk.get_entities()) removed.insert(uid);
    chunk.set_normal(bl::chunk_key::Entity, "");
    chunk.clear_entities();
    putChunk(pos, chunk);
    auto& pending = cache_.at(pos).removedActorUids;
    pending.insert(removed.begin(), removed.end());
}

bl::chunk* RawChunkCache::getChunk(const bl::chunk_pos& pos, bl::chunk_load_policy policy) const {
    auto it = cache_.find(pos);
    // if a chunk are mark as deleted in cache, we think is does not exist
    if (it == cache_.end() || it->second.shouldDelete || !it->second.chunk) return nullptr;
    auto* c = new bl::chunk(pos);
    c->load_from_raw_chunk(*it->second.chunk, policy);
    return c;
}

std::optional<bl::raw_chunk> RawChunkCache::getRawChunk(const bl::chunk_pos& pos) const {
    auto it = cache_.find(pos);
    // if a chunk are mark as deleted in cache, we think is does not exist
    if (it == cache_.end() || it->second.shouldDelete || !it->second.chunk) return std::nullopt;
    return *it->second.chunk;
}

bool RawChunkCache::hasChunk(const bl::chunk_pos& pos) const { return cache_.find(pos) != cache_.end(); }

void RawChunkCache::putMissing(const bl::chunk_pos& pos) {
    auto it = cache_.find(pos);
    if (it != cache_.end()) {
        it->second.shouldDelete = true;
        it->second.chunk.reset();
    } else {
        cache_.emplace(pos, CachedChunk{std::nullopt, true, {}, std::nullopt});
    }
}

void RawChunkCache::commit(bl::bedrock_level& level, leveldb::WriteBatch& batch) {
    LOG_F(INFO, "Cache size is %llu", static_cast<unsigned long long>(cache_.size()));
    for (auto& [pos, entry] : cache_) {
        if (entry.shouldDelete) {
            bl::raw_chunk raw(pos);
            if (raw.read(level)) raw.write(batch, true);
        } else if (entry.chunk) {
            entry.chunk->write(batch, false);
        }
        if (entry.entityData) {
            const auto& entities = *entry.entityData;
            const auto legacyKey = bl::chunk_key{bl::chunk_key::Entity, pos}.to_raw();
            if (entry.shouldDelete || entities.legacy.empty()) batch.Delete(legacyKey);
            else batch.Put(legacyKey, entities.legacy);
            std::string digest;
            for (const auto& [key, bytes] : entities.actors) {
                const auto actorKey = std::string(bl::storage_key::actor) + key;
                if (entry.shouldDelete) batch.Delete(actorKey);
                else {
                    digest += key;
                    batch.Put(actorKey, bytes);
                }
            }
            const auto digestKey = bl::actor_digest_key{pos}.to_raw();
            if (digest.empty()) batch.Delete(digestKey);
            else batch.Put(digestKey, digest);
        }
        for (const auto& uid : entry.removedActorUids) {
            if (!entry.shouldDelete) {
                if (entry.chunk && entry.chunk->get_entities().contains(uid)) continue;
                if (entry.entityData && entry.entityData->actors.contains(uid)) continue;
            }
            batch.Delete(std::string(bl::storage_key::actor) + uid);
        }
    }
}
