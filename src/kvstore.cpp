#include "kvstore.h"
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

KVStore::KVStore(const KVConfig& config)
    : config_(config), cache_(config.cache_size) {

    // DB directory banao agar nahi hai
    fs::create_directories(config_.db_path);

    // WAL initialize karo
    wal_ = std::make_unique<WAL>(config_.db_path + "/wal.log");

    // Pehle se existing SSTables load karo
    for (auto& entry : fs::directory_iterator(config_.db_path)) {
        if (entry.path().extension() == ".sst") {
            sstables_.push_back(std::make_unique<SSTable>(entry.path().string()));
            sstable_counter_++;
        }
    }

    // SSTables ko order mein sort karo (newest first)
    std::sort(sstables_.begin(), sstables_.end(),
        [](const auto& a, const auto& b) {
            return a->path() > b->path();  // lexicographic = time order
        });

    // WAL se crash recovery
    recover_from_wal();
}

KVStore::~KVStore() {
    close();
}

void KVStore::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!memtable_.empty() || !deleted_keys_.empty()) {
        // Bacha hua data flush karo
        SSTable::write(next_sstable_path(), memtable_, deleted_keys_);
        memtable_.clear();
        deleted_keys_.clear();
        if (wal_) wal_->clear();
    }
}

// ─── WRITE PATH ───────────────────────────────────────────────────────────────

bool KVStore::put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);

    // Step 1: WAL mein likho (crash safe)
    if (!wal_->log_put(key, value)) return false;

    // Step 2: MemTable mein daalo
    memtable_[key] = value;
    deleted_keys_.erase(key);  // agar pehle delete tha

    // Step 3: Cache update karo
    cache_.put(key, value);

    stats_.writes++;

    // Step 4: Bhar gayi toh flush karo
    maybe_flush();
    return true;
}

bool KVStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);

    // WAL mein likho
    if (!wal_->log_delete(key)) return false;

    // MemTable se hatao, tombstone lagao
    memtable_.erase(key);
    deleted_keys_[key] = true;
    cache_.remove(key);

    stats_.deletes++;
    maybe_flush();
    return true;
}

// ─── READ PATH ────────────────────────────────────────────────────────────────

std::optional<std::string> KVStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.reads++;

    // Step 1: Pehle MemTable mein dekho (sabse fresh data)
    if (deleted_keys_.count(key)) return std::nullopt;  // deleted
    auto it = memtable_.find(key);
    if (it != memtable_.end()) return it->second;

    // Step 2: Block Cache check karo
    auto cached = cache_.get(key);
    if (cached.has_value()) {
        stats_.cache_hits++;
        return cached;
    }
    stats_.cache_miss++;

    // Step 3: SSTables mein dhundho (newest first)
    for (auto& sst : sstables_) {
        auto result = sst->get(key);
        if (result.has_value()) {
            if (result->is_deleted) return std::nullopt;
            // Cache mein daalo agle read ke liye
            cache_.put(key, result->value);
            return result->value;
        }
        // Bloom filter ne skip kiya = stats update
        stats_.bloom_skip++;
    }

    return std::nullopt;  // key exist hi nahi karti
}

bool KVStore::contains(const std::string& key) {
    return get(key).has_value();
}

// ─── FLUSH ────────────────────────────────────────────────────────────────────

void KVStore::flush() {
    std::lock_guard<std::mutex> lock(mu_);
    if (memtable_.empty() && deleted_keys_.empty()) return;

    std::string sst_path = next_sstable_path();
    if (SSTable::write(sst_path, memtable_, deleted_keys_)) {
        sstables_.insert(sstables_.begin(),
            std::make_unique<SSTable>(sst_path));
        memtable_.clear();
        deleted_keys_.clear();
        wal_->clear();
        stats_.sstable_cnt = static_cast<int>(sstables_.size());
    }
}

void KVStore::maybe_flush() {
    // mutex already held by caller
    if (memtable_.size() + deleted_keys_.size() >= config_.memtable_size) {
        std::string sst_path = next_sstable_path();
        if (SSTable::write(sst_path, memtable_, deleted_keys_)) {
            sstables_.insert(sstables_.begin(),
                std::make_unique<SSTable>(sst_path));
            memtable_.clear();
            deleted_keys_.clear();
            wal_->clear();
            stats_.sstable_cnt = static_cast<int>(sstables_.size());
            maybe_compact();
        }
    }
}

// ─── COMPACTION ───────────────────────────────────────────────────────────────
// Multiple SSTables ko ek badi SSTable mein merge karo
// Duplicate keys mein newest wala jeetega
// Tombstones (deleted keys) hata do

void KVStore::compact() {
    std::lock_guard<std::mutex> lock(mu_);
    maybe_compact();
}

void KVStore::maybe_compact() {
    if ((int)sstables_.size() < config_.max_sstables) return;

    // Saare SSTables ko merge karo
    std::map<std::string, SSTable::Entry> merged;

    // Purane se nayi ki taraf — nayi entries purani ko overwrite karengi
    for (int i = (int)sstables_.size() - 1; i >= 0; i--) {
        auto entries = sstables_[i]->read_all();
        for (auto& e : entries) merged[e.key] = e;
    }

    // Merged data se nayi SSTable banao
    std::map<std::string, std::string> live_data;
    std::map<std::string, bool> dead_data;
    for (auto& [k, e] : merged) {
        if (e.is_deleted) dead_data[k] = true;
        else live_data[k] = e.value;
    }

    std::string new_path = next_sstable_path();
    if (!SSTable::write(new_path, live_data, dead_data)) return;

    // Purane SSTables delete karo
    for (auto& sst : sstables_) {
        fs::remove(sst->path());
    }
    sstables_.clear();
    sstables_.push_back(std::make_unique<SSTable>(new_path));
    stats_.sstable_cnt = 1;
}

// ─── CRASH RECOVERY ───────────────────────────────────────────────────────────

void KVStore::recover_from_wal() {
    int recovered = 0;
    wal_->recover([&](const WAL::Record& rec) {
        if (rec.op == WAL::OpType::PUT) {
            memtable_[rec.key] = rec.value;
            deleted_keys_.erase(rec.key);
        } else {
            memtable_.erase(rec.key);
            deleted_keys_[rec.key] = true;
        }
        recovered++;
    });
    if (recovered > 0) {
        std::cout << "[KVStore] Recovered " << recovered
                  << " records from WAL\n";
    }
}

// ─── STATS ────────────────────────────────────────────────────────────────────

KVStore::Stats KVStore::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stats s = stats_;
    s.cache_hits  = cache_.hits();
    s.cache_miss  = cache_.misses();
    s.sstable_cnt = static_cast<int>(sstables_.size());
    return s;
}

std::string KVStore::next_sstable_path() {
    return config_.db_path + "/sst_" +
           std::to_string(++sstable_counter_) + ".sst";
}
