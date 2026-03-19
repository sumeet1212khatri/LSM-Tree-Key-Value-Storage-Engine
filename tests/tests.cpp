#include <gtest/gtest.h>
#include "kvstore.h"
#include "bloom_filter.h"
#include "block_cache.h"
#include "wal.h"
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static KVConfig test_config(const std::string& dir) {
    KVConfig cfg;
    cfg.db_path       = dir;
    cfg.memtable_size = 10;   // Chota rakho — SSTable flush jaldi ho
    cfg.cache_size    = 50;
    return cfg;
}

static void cleanup(const std::string& dir) {
    fs::remove_all(dir);
}

// ─── Bloom Filter Tests ────────────────────────────────────────────────────────

TEST(BloomFilterTest, BasicAddAndCheck) {
    BloomFilter bf;
    bf.add("hello");
    bf.add("world");
    EXPECT_TRUE(bf.possibly_contains("hello"));
    EXPECT_TRUE(bf.possibly_contains("world"));
}

TEST(BloomFilterTest, MissingKeyReturnsFalse) {
    BloomFilter bf;
    bf.add("present");
    // "absent" kabhi add nahi kiya — mostly false hoga
    // Note: false positive possible hai but very rare
    int false_positives = 0;
    for (int i = 0; i < 100; i++) {
        if (bf.possibly_contains("missing_key_" + std::to_string(i)))
            false_positives++;
    }
    EXPECT_LT(false_positives, 10);  // <10% false positive rate
}

TEST(BloomFilterTest, Reset) {
    BloomFilter bf;
    bf.add("key1");
    bf.reset();
    // After reset, false positives almost zero
    EXPECT_FALSE(bf.possibly_contains("key1"));
}

// ─── Block Cache Tests ─────────────────────────────────────────────────────────

TEST(BlockCacheTest, PutAndGet) {
    BlockCache cache(100);
    cache.put("k1", "v1");
    auto v = cache.get("k1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), "v1");
}

TEST(BlockCacheTest, MissReturnsNullopt) {
    BlockCache cache(100);
    EXPECT_FALSE(cache.get("nonexistent").has_value());
}

TEST(BlockCacheTest, LRUEviction) {
    BlockCache cache(3);  // sirf 3 entries
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    cache.put("d", "4");  // "a" evict hona chahiye
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_TRUE(cache.get("b").has_value());
    EXPECT_TRUE(cache.get("c").has_value());
    EXPECT_TRUE(cache.get("d").has_value());
}

TEST(BlockCacheTest, RecentlyUsedNotEvicted) {
    BlockCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    cache.get("a");        // "a" ko access karo — recently used
    cache.put("d", "4");   // "b" evict hona chahiye (LRU)
    EXPECT_TRUE(cache.get("a").has_value());   // "a" still hai
    EXPECT_FALSE(cache.get("b").has_value());  // "b" gone
}

TEST(BlockCacheTest, HitMissStats) {
    BlockCache cache(100);
    cache.put("x", "y");
    cache.get("x");   // hit
    cache.get("z");   // miss
    EXPECT_EQ(cache.hits(), 1);
    EXPECT_EQ(cache.misses(), 1);
}

// ─── WAL Tests ────────────────────────────────────────────────────────────────

TEST(WALTest, LogAndRecover) {
    std::string path = "/tmp/test_wal.log";
    fs::remove(path);

    {
        WAL wal(path);
        wal.log_put("name", "Rahul");
        wal.log_put("city", "Mumbai");
        wal.log_delete("name");
    }

    WAL wal2(path);
    std::vector<WAL::Record> records;
    wal2.recover([&](const WAL::Record& r) {
        records.push_back(r);
    });

    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].key, "name");
    EXPECT_EQ(records[0].op, WAL::OpType::PUT);
    EXPECT_EQ(records[1].key, "city");
    EXPECT_EQ(records[2].op, WAL::OpType::DEL);

    fs::remove(path);
}

// ─── KVStore Core Tests ────────────────────────────────────────────────────────

class KVStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup(dir_);
        db_ = std::make_unique<KVStore>(test_config(dir_));
    }
    void TearDown() override {
        db_.reset();
        cleanup(dir_);
    }
    std::string dir_ = "/tmp/kvtest_basic";
    std::unique_ptr<KVStore> db_;
};

TEST_F(KVStoreTest, PutAndGet) {
    EXPECT_TRUE(db_->put("foo", "bar"));
    auto v = db_->get("foo");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), "bar");
}

TEST_F(KVStoreTest, GetMissingKey) {
    EXPECT_FALSE(db_->get("ghost").has_value());
}

TEST_F(KVStoreTest, Overwrite) {
    db_->put("key", "v1");
    db_->put("key", "v2");
    auto v = db_->get("key");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), "v2");
}

TEST_F(KVStoreTest, Delete) {
    db_->put("del_me", "value");
    EXPECT_TRUE(db_->contains("del_me"));
    db_->del("del_me");
    EXPECT_FALSE(db_->contains("del_me"));
}

TEST_F(KVStoreTest, MultipleKeys) {
    for (int i = 0; i < 100; i++) {
        db_->put("key" + std::to_string(i), "val" + std::to_string(i));
    }
    for (int i = 0; i < 100; i++) {
        auto v = db_->get("key" + std::to_string(i));
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(v.value(), "val" + std::to_string(i));
    }
}

// ─── Crash Recovery Test ──────────────────────────────────────────────────────

TEST(CrashRecoveryTest, RecoverAfterCrash) {
    std::string dir = "/tmp/kvtest_crash";
    cleanup(dir);

    // DB mein data daalo
    {
        KVStore db(test_config(dir));
        db.put("survivor", "alive");
        db.put("another", "data");
        // Deliberately NAHI close karte — crash simulate karo
        // Destructor call hoga, WAL file remain karegi
    }

    // Naya db instance — WAL se recover hona chahiye
    {
        KVStore db2(test_config(dir));
        auto v = db2.get("survivor");
        ASSERT_TRUE(v.has_value()) << "Data WAL se recover hona chahiye tha";
        EXPECT_EQ(v.value(), "alive");
    }

    cleanup(dir);
}

// ─── Flush & Persistence Test ─────────────────────────────────────────────────

TEST(PersistenceTest, DataSurvivesRestart) {
    std::string dir = "/tmp/kvtest_persist";
    cleanup(dir);

    {
        KVStore db(test_config(dir));
        for (int i = 0; i < 50; i++) {  // memtable_size=10 so will flush
            db.put("p" + std::to_string(i), "v" + std::to_string(i));
        }
        db.flush();
    }

    // Restart
    {
        KVStore db2(test_config(dir));
        for (int i = 0; i < 50; i++) {
            auto v = db2.get("p" + std::to_string(i));
            ASSERT_TRUE(v.has_value()) << "Key p" << i << " missing after restart";
        }
    }

    cleanup(dir);
}

// ─── Stats Test ───────────────────────────────────────────────────────────────

TEST(StatsTest, WriteReadCounts) {
    std::string dir = "/tmp/kvtest_stats";
    cleanup(dir);
    KVStore db(test_config(dir));

    db.put("a", "1");
    db.put("b", "2");
    db.get("a");
    db.get("a");
    db.del("b");

    auto s = db.stats();
    EXPECT_EQ(s.writes, 2u);
    EXPECT_EQ(s.reads, 2u);
    EXPECT_EQ(s.deletes, 1u);

    cleanup(dir);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
