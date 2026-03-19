#pragma once
#include "bloom_filter.h"
#include <string>
#include <map>
#include <vector>
#include <optional>
#include <cstdint>

// SSTable — Sorted String Table
// MemTable jab bhar jaaye, disk pe ek sorted file mein likha jaata hai
// Ek baar likh diya toh read-only — kabhi modify nahi hota

class SSTable {
public:
    struct Entry {
        std::string key;
        std::string value;
        bool is_deleted;   // tombstone — DELETE operation ke liye
    };

    // Memory se SSTable disk pe likho
    static bool write(const std::string& path,
                      const std::map<std::string, std::string>& memtable,
                      const std::map<std::string, bool>& deleted_keys);

    // Disk se SSTable padho
    explicit SSTable(const std::string& path);

    // Ek key dhundho — bloom filter pehle check karta hai
    std::optional<Entry> get(const std::string& key);

    // Saari entries iterate karo (compaction ke liye)
    std::vector<Entry> read_all();

    const std::string& path() const { return path_; }
    uint64_t file_size() const;

private:
    std::string path_;
    BloomFilter bloom_{1000, 0.01};

    // Bloom filter ko file se load karo
    void load_bloom();
};
