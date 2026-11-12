#include "loader/rawchunkcache.h"

#include "chunk.h"
#include "loguru/loguru.hpp"

std::pair<int, int> RawChunkCache::chunkCounts() const {
    int empty = 0, nonEmpty = 0;
    for (const auto& kv : cache_) {
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
    if (previous != cache_.end()) removed = std::move(previous->second.removedActorUids);
    cache_.insert_or_assign(pos, CachedChunk{chunk, false, {}});
    cache_.at(pos).removedActorUids = std::move(removed);
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
        cache_.emplace(pos, CachedChunk{std::nullopt, true, {}});
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
            for (const auto& uid : entry.removedActorUids) {
                if (!entry.chunk->get_entities().contains(uid)) batch.Delete(std::string(bl::storage_key::actor) + uid);
            }
        }
    }
}
