#ifndef BEDROCKMAP_CHUNKSTORAGE_H
#define BEDROCKMAP_CHUNKSTORAGE_H

#include <mutex>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bedrock_level.h"
#include "chunk.h"
#include "loader/rawchunkcache.h"
#include "nbt.h"

class ChunkCoordsService;

struct WorldEntityRecord {
    int dimension;
    float x, y, z;
    std::string identifier;
    std::string customName;
};

enum class WorldEntityScanStage { Preparing, ModernEntities, LegacyEntities, Grouping, Summaries };

struct WorldEntityScanProgress {
    WorldEntityScanStage stage{WorldEntityScanStage::Preparing};
    uint64_t keys{0};
    uint64_t entities{0};
    uint64_t processed{0};
    std::optional<uint64_t> total;
    uint64_t actorReads{0};
};

struct WorldEntityScanResult {
    std::vector<WorldEntityRecord> entities;
    uint64_t skippedRecords{0};
};

class ChunkStorage {
   public:
    enum class CommitError {
        None,
        LevelDb,
        LevelDatSerialization,
        LevelDatWrite,
    };

    bool open(const std::string& path, const std::string& xor_key, std::shared_ptr<leveldb::Env> env = {}) {
        level_.set_xor_key(xor_key, std::move(env));
        return level_.open(path);
    }
    void close() { level_.close(); }

    bl::bedrock_level& level() { return level_; }
    const bl::bedrock_level& level() const { return level_; }

    bool isOpen() const { return level_.is_open(); }
    bool isDirty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.isDirty();
    }
    std::pair<int, int> chunkModifyCounts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.chunkCounts();
    }
    CommitError lastCommitError() const { return last_commit_error_; }

    bl::chunk* getChunk(const bl::chunk_pos& pos, bl::chunk_load_policy policy);
    std::optional<bl::raw_chunk> getRawChunk(const bl::chunk_pos& pos);
    void putMissing(const bl::chunk_pos& pos);
    void putRawChunk(const bl::raw_chunk& raw);
    bool clearChunkEntities(const bl::chunk_pos& pos);
    std::vector<bl::chunk_pos> pendingChunkPositions() const;
    void replayChunkIndex(ChunkCoordsService& coords, const std::vector<bl::chunk_pos>& beforeScan);
    WorldEntityScanResult scanWorldEntities(const std::atomic_bool& stop,
        const std::function<void(const WorldEntityScanProgress&)>& progress,
        const std::optional<std::vector<bl::chunk_pos>>& legacyChunks);

    /// True when the chunk has an uncommitted edit held in memory (a replacement
    /// or a delete marker) rather than in LevelDB.
    bool hasPendingEdit(const bl::chunk_pos& pos) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.hasChunk(pos);
    }

    // Commit chunk and global database edits as one LevelDB batch, then persist
    // an optional level.dat snapshot. LevelDB and level.dat are separate
    // stores, so a level.dat write failure is reported and dirty state remains.
    bool commit(const std::unordered_map<std::string, std::string>& globalModifies = {}, const bl::nbt::compound_tag* levelDat = nullptr);

   private:
    bl::bedrock_level level_{true};
    RawChunkCache cache_;
    std::unordered_set<bl::chunk_pos> added_legacy_entity_chunks_;
    mutable std::mutex mutex_;
    CommitError last_commit_error_{CommitError::None};
};

#endif  // BEDROCKMAP_CHUNKSTORAGE_H
