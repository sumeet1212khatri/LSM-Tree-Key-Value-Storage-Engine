#include "sstable.h"
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>

// SSTable file format:
// [num_entries: 4 bytes]
// for each entry:
//   [is_deleted: 1 byte][key_len: 4][key][val_len: 4][val]
// [bloom_bits_count: 4 bytes]
// [bloom bit array: ceil(bits/8) bytes]

static void write_str(std::ofstream& f, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    f.write(reinterpret_cast<const char*>(&len), 4);
    f.write(s.data(), len);
}

static std::string read_str(std::ifstream& f) {
    uint32_t len;
    f.read(reinterpret_cast<char*>(&len), 4);
    std::string s(len, '\0');
    f.read(s.data(), len);
    return s;
}

bool SSTable::write(const std::string& path,
                    const std::map<std::string, std::string>& memtable,
                    const std::map<std::string, bool>& deleted_keys) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;

    // Total entries = live + deleted
    uint32_t num = static_cast<uint32_t>(memtable.size() + deleted_keys.size());

    // Merge karo dono ko ek sorted map mein
    std::map<std::string, SSTable::Entry> merged;
    for (auto& [k, v] : memtable)
        merged[k] = {k, v, false};
    for (auto& [k, _] : deleted_keys)
        merged[k] = {k, "", true};

    uint32_t actual_num = static_cast<uint32_t>(merged.size());
    BloomFilter bloom(actual_num > 0 ? actual_num : 1000, 0.01);
    f.write(reinterpret_cast<const char*>(&actual_num), 4);

    for (auto& [k, entry] : merged) {
        uint8_t del = entry.is_deleted ? 1 : 0;
        f.write(reinterpret_cast<const char*>(&del), 1);
        write_str(f, entry.key);
        write_str(f, entry.value);
        bloom.add(entry.key);
    }

    // Bloom filter serialize karo
    // Simplified: store as vector<bool> via byte packing
    // We store 8192 bits = 1024 bytes
    const size_t BITS = 8192;
    std::vector<uint8_t> bloom_bytes(BITS / 8, 0);
    // Re-add to extract bits (simple approach)
    // We serialize bloom state by re-checking each position
    // Instead: bloom filter bit array ko direct serialize karo via add+check trick
    // For simplicity, store bloom size then re-build on load
    uint32_t bloom_entries = actual_num;
    f.write(reinterpret_cast<const char*>(&bloom_entries), 4);

    f.flush();
    return f.good();
}

SSTable::SSTable(const std::string& path) : path_(path), bloom_(1000, 0.01) {
    load_bloom();
}

void SSTable::load_bloom() {
    // Poori file padho aur bloom mein keys add karo
    auto entries = read_all();
    for (auto& e : entries) bloom_.add(e.key);
}

std::vector<SSTable::Entry> SSTable::read_all() {
    std::ifstream f(path_, std::ios::binary);
    if (!f.is_open()) return {};

    uint32_t num;
    f.read(reinterpret_cast<char*>(&num), 4);

    std::vector<Entry> result;
    result.reserve(num);

    for (uint32_t i = 0; i < num; i++) {
        uint8_t del;
        f.read(reinterpret_cast<char*>(&del), 1);
        std::string key = read_str(f);
        std::string val = read_str(f);
        result.push_back({key, val, del == 1});
    }
    return result;
}

std::optional<SSTable::Entry> SSTable::get(const std::string& key) {
    // Bloom filter pehle check karo — agar nahi mila toh disk skip
    if (!bloom_.possibly_contains(key)) return std::nullopt;

    // Binary search possible hai kyunki sorted hai
    // Simple linear scan for now (production mein index hota hai)
    auto entries = read_all();
    for (auto& e : entries) {
        if (e.key == key) return e;
        if (e.key > key) break;  // sorted hai — aage nahi milega
    }
    return std::nullopt;
}

uint64_t SSTable::file_size() const {
    struct stat st;
    if (stat(path_.c_str(), &st) == 0) return st.st_size;
    return 0;
}
