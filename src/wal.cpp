#include "wal.h"
#include <stdexcept>
#include <fstream>

WAL::WAL(const std::string& path) : path_(path) {
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    if (!file_.is_open()) {
        std::ofstream create(path, std::ios::binary); create.close();
        file_.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    }
    if (!file_.is_open()) throw std::runtime_error("WAL: cannot open: " + path);
}

WAL::~WAL() { if (file_.is_open()) file_.close(); }

bool WAL::log_put(const std::string& key, const std::string& value) {
    return write_record({OpType::PUT, key, value});
}

bool WAL::log_delete(const std::string& key) {
    return write_record({OpType::DEL, key, ""});
}

bool WAL::write_record(const Record& rec) {
    uint8_t  op      = static_cast<uint8_t>(rec.op);
    uint32_t key_len = static_cast<uint32_t>(rec.key.size());
    uint32_t val_len = static_cast<uint32_t>(rec.value.size());
    std::string buf;
    buf.reserve(1 + 4 + key_len + 4 + val_len);
    buf.append(reinterpret_cast<const char*>(&op),      1);
    buf.append(reinterpret_cast<const char*>(&key_len), 4);
    buf.append(rec.key);
    buf.append(reinterpret_cast<const char*>(&val_len), 4);
    buf.append(rec.value);
    file_.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    file_.flush();
    return file_.good();
}

bool WAL::recover(std::function<void(const Record&)> apply_fn) {
    file_.close();
    std::fstream reader(path_, std::ios::in | std::ios::binary);
    if (!reader.is_open()) return false;
    Record rec;
    while (reader.peek() != EOF) {
        if (!read_record(reader, rec)) break;
        apply_fn(rec);
    }
    reader.close();
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    return file_.is_open();
}

bool WAL::read_record(std::fstream& f, Record& rec) {
    uint8_t  op;
    uint32_t key_len, val_len;
    if (!f.read(reinterpret_cast<char*>(&op),      1)) return false;
    if (!f.read(reinterpret_cast<char*>(&key_len), 4)) return false;
    rec.op = static_cast<OpType>(op);
    rec.key.resize(key_len);
    if (!f.read(rec.key.data(), key_len)) return false;
    if (!f.read(reinterpret_cast<char*>(&val_len), 4)) return false;
    rec.value.resize(val_len);
    if (val_len > 0 && !f.read(rec.value.data(), val_len)) return false;
    return true;
}

bool WAL::clear() {
    file_.close();
    std::ofstream trunc(path_, std::ios::trunc | std::ios::binary); trunc.close();
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    return file_.is_open();
}
