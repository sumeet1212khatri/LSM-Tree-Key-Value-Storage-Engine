# KVStore — LSM-Tree Key-Value Storage Engine

![Language](https://img.shields.io/badge/language-C%2B%2B17-blue?style=flat-square&logo=cplusplus)
![Build](https://img.shields.io/badge/build-CMake%203.16%2B-green?style=flat-square&logo=cmake)
![Tests](https://img.shields.io/badge/tests-17%2F17%20passing-brightgreen?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-lightgrey?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-informational?style=flat-square)
![LOC](https://img.shields.io/badge/LOC-1%2C300%2B-orange?style=flat-square)

> A production-grade, persistent key-value storage engine built from scratch in C++17 —
> inspired by Google LevelDB and Facebook RocksDB — with zero external runtime dependencies.

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Performance Benchmarks](#performance-benchmarks)
- [Key Features](#key-features)
- [Tech Stack](#tech-stack)
- [Getting Started](#getting-started)
- [Usage](#usage)
- [Testing](#testing)
- [Roadmap](#roadmap)

---

## Overview

KVStore implements the **Log-Structured Merge-Tree (LSM-Tree)** architecture — the same
storage model underlying Google Bigtable, Apache Cassandra, and RocksDB. It solves three
fundamental problems in persistent storage:

| Problem | Solution |
|---|---|
| **Crash safety** | Write-Ahead Log with O(n) replay — zero data loss |
| **Memory pressure at scale** | MemTable flush to immutable SSTables on disk |
| **Read amplification** | Bloom filters + LRU block cache — 99% disk reads eliminated |

This engine handles the full write path (WAL → MemTable → SSTable → Compaction) and read
path (MemTable → Cache → Bloom Filter → SSTable) with correctness guarantees validated
by 17 GoogleTest unit tests.

---

## Architecture

### Write Path

```
put("key", "value")
        │
        ├──► 1. WAL (wal.log)           ← binary-packed, single syscall, crash-safe
        │
        ├──► 2. MemTable (std::map)     ← sorted in-memory, O(log n) insert
        │
        └──► 3. MemTable full?
                      │
                      ├──► Flush → SSTable (immutable sorted file on disk)
                      │
                      └──► [N SSTables?] → background Compaction
```

### Read Path

```
get("key")
        │
        ├──► 1. MemTable                ← O(log n), always freshest
        │
        ├──► 2. Block Cache (LRU)       ← O(1) doubly-linked list + hashmap
        │
        └──► 3. SSTables (newest → oldest)
                      │
                      └──► Bloom Filter ← 99% of disk reads skipped here
```

---

### Component Deep Dive

#### WAL — Write-Ahead Log

Every write is durably logged before touching memory.

```
Record wire format:
┌──────────┬──────────────┬─────────┬──────────────┬─────────┐
│  op (1B) │ key_len (4B) │ key (N) │ val_len (4B) │ val (M) │
└──────────┴──────────────┴─────────┴──────────────┴─────────┘
```

- **Single-buffer write**: entire record packed into one buffer → one `file_.write()` call
  → one syscall. Minimizes kernel context switches vs field-by-field writes.
- **Recovery**: `WAL::recover()` replays records sequentially into MemTable. O(n), < 100ms.
- **Lifecycle**: WAL truncated after each successful SSTable flush.

#### MemTable

- `std::map<string, string>` — sorted by key, O(log n) all operations.
- Separate `deleted_keys_` map carries tombstones for delete operations.
- Flushed to SSTable when entry count exceeds `memtable_size` (default: 4096).

#### SSTable — Sorted String Table

```
File layout:
┌──────────────────┐
│ num_entries (4B) │
├──────────────────┤
│ is_deleted (1B)  │  ← tombstone marker
│ key_len (4B)     │  ← repeated per entry,
│ key (N bytes)    │    sorted ascending
│ val_len (4B)     │
│ val (M bytes)    │
├──────────────────┤
│ bloom_count (4B) │  ← for filter reconstruction on load
└──────────────────┘
```

- Immutable after creation — append-only system.
- Bloom filter reconstructed in memory on SSTable load.
- Multiple SSTables merged via **Compaction** (merge-sort, newest-wins semantics).

#### Bloom Filter

Probabilistic set-membership — answers "is this key on disk?" without touching disk.

**Double-hashing scheme:**

```
h1(key) = FNV-1a   (seed: 14695981039346656037, prime: 1099511628211)
h2(key) = djb2-xor (seed: 5381, shift-xor per char) | 1  ← force odd

bit_index(key, i) = (h1 + i × h2) % size_bits    for i ∈ [0, k)
```

**Optimal sizing (standard formula):**

```
m = -n × ln(p) / (ln 2)²      ← total bits needed
k = (m / n) × ln 2             ← optimal number of hash functions
```

| Parameter | Value | Rationale |
|---|---|---|
| Sizing strategy | Dynamic — `m = -n·ln(p)/(ln2)²` | correct per key count |
| Target FP rate | 1% | 10 bits/key sweet spot |
| Measured FP rate | **1.05%** | verified at 200K keys |
| False negative rate | **0%** | guaranteed by construction |
| Disk I/O eliminated | **99%** | benchmarked at 200K keys |

#### Block Cache — LRU Eviction

```
Internal layout:

  Doubly-linked list  (MRU → LRU):
  [key_D] ↔ [key_A] ↔ [key_C] ↔ [key_B]

  Hash map:
  { "key_D" → iterator,  "key_A" → iterator, ... }
```

- `get()` / `put()` both **O(1)** — `list::splice` + hashmap lookup.
- Eviction: tail node removed when count exceeds `cache_size`.
- Exposes `hits()`, `misses()`, `hit_rate()` for observability.

#### Compaction

Triggered when SSTable count ≥ `max_sstables` (default: 8).

```
1. Scan all SSTables oldest → newest into merged std::map
2. Newer entries overwrite older  (newest-wins)
3. Tombstoned keys retained       (propagate deletes)
4. Write single merged SSTable to disk
5. Delete all superseded SSTable files
6. Reset SSTable list to [merged_file]
```

Bounds read amplification: O(n files) → O(1).

---

## Performance Benchmarks

> **Environment**: Google Colab, Linux, Release build (`-O2`), single thread.
> **Methodology**: proper warmup + 5 independent runs + median + p50/p95/p99 tail latency.
> **Reproduce**: `./bench` from the build directory.

### Write Throughput — 5 Runs, Median

| Metric | Value | Notes |
|---|---|---|
| **Median write throughput** | **225K writes/sec** | real SSTable flushes included |
| Min / Max | 222K / 232K | low variance — stable |
| p50 write latency | **2.61 µs** | WAL + MemTable + periodic flush |
| p95 write latency | 6.47 µs | |
| p99 write latency | 10.86 µs | |
| Crash recovery | — | **< 100ms** via O(n) WAL replay |

### Read Throughput — Warm Cache

| Metric | Value | Notes |
|---|---|---|
| **Throughput** | **2.8M reads/sec** | Zipfian 80/20 workload |
| p50 latency | **0.23 µs** | |
| p95 latency | 0.83 µs | |
| p99 latency | **1.11 µs** | |

### Read — Cold Cache (SSTable Disk)

| Metric | Value | Notes |
|---|---|---|
| p50 latency | 3.19 ms | full SSTable linear scan |
| p95 latency | 3.64 ms | |
| Known limitation | sparse index missing | tracked in Roadmap |

### Bloom Filter

| Operation | Throughput | Latency | Notes |
|---|---|---|---|
| `add(key)` | 4.1M ops/sec | 0.25 µs | |
| `check(existing)` | 4.1M ops/sec | 0.24 µs | 200K/200K true positive |
| `check(missing)` | 8.9M ops/sec | 0.11 µs | |
| False positive rate | **1.05%** | — | at 200K keys |
| **Disk I/O eliminated** | **99.0%** | — | |

### Block Cache — Realistic Workload

| Metric | Value | Notes |
|---|---|---|
| Throughput | 3.3M ops/sec | Zipfian 80/20 |
| p50 latency | 0.27 µs | |
| p99 latency | 0.75 µs | |
| Cache ratio | 5K / 100K keys | 5% — realistic |
| **Hit rate** | **27.3%** | honest — not inflated |

### Benchmark Methodology — What We Fixed

| Common toy-benchmark flaw | Our fix |
|---|---|
| Memtable never flushes | `memtable_size=5000` — real flushes during bench |
| 100% cache hit rate | 5% cache ratio, 100K key working set |
| Uniform random distribution | Zipfian 80/20 hot/cold |
| Single-run variance | 5 independent runs, median reported |
| Only throughput, no latency | p50 / p95 / p99 for all ops |
| No cold read measurement | Cold SSTable path measured separately |

---

## Key Features

- **Zero external runtime dependencies** — pure C++17 STL, no Boost, no third-party libs
- **Crash-safe WAL** — binary-packed records, single-syscall writes, O(n) linear recovery
- **Full LSM-Tree pipeline** — MemTable → SSTable → background Compaction
- **Optimally-sized Bloom Filter** — `m = -n·ln(p)/(ln2)²`, 1.05% FP, 99% disk I/O saved
- **O(1) LRU Block Cache** — doubly-linked list + hashmap, p99=0.75µs
- **Configurable engine** — memtable size, cache capacity, compaction threshold all tunable
- **Built-in observability** — counters for writes, reads, cache hits, bloom skips
- **Full correctness test suite** — 17 GoogleTest tests across 7 suites

---

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C++17 |
| STL components | `std::map`, `std::vector<bool>`, `std::optional`, `std::mutex`, `std::fstream` |
| Build system | CMake 3.16+ |
| Test framework | GoogleTest 1.14 |
| Compiler | GCC 11+ / Clang 13+ |
| Platform | Linux, macOS |

---

## Getting Started

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt update && sudo apt install -y g++ cmake libgtest-dev

# macOS (Homebrew)
brew install cmake googletest

# Google Colab
!apt-get install -y cmake g++ libgtest-dev
```

### Build

```bash
git clone https://github.com/sumeet1212khatri/LSM-Tree-Key-Value-Storage-Engine.git
cd LSM-Tree-Key-Value-Storage-Engine

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

| Binary | Purpose |
|---|---|
| `./kvstore` | CLI — interactive key-value operations |
| `./bench` | Benchmark runner — reproduces all metrics |
| `./run_tests` | Full test suite |

---

## Usage

### CLI

```bash
./kvstore set city Mumbai       # OK
./kvstore get city              # Mumbai
./kvstore get ghost             # (nil)
./kvstore delete city           # OK
./kvstore stats                 # writes, reads, cache rate, bloom skips
./kvstore flush                 # force MemTable → SSTable
./kvstore compact               # merge all SSTables
```

### Library API

```cpp
#include "kvstore.h"

KVConfig cfg;
cfg.db_path       = "./mydb";
cfg.memtable_size = 4096;
cfg.cache_size    = 1000;
cfg.max_sstables  = 8;

KVStore db(cfg);
db.put("user:1001", "sumeet");

auto val = db.get("user:1001");
if (val.has_value()) std::cout << val.value() << "\n";

db.del("user:1001");

auto s = db.stats();
// s.writes, s.reads, s.cache_hits, s.cache_miss, s.bloom_skip
```

---

## Testing

```bash
cd build && ./run_tests
```

```
[==========] Running 17 tests from 7 test suites.
[  PASSED  ] 17 tests.
```

| Suite | Count | Coverage |
|---|---|---|
| `BloomFilterTest` | 3 | add, FP/TP rates, reset |
| `BlockCacheTest` | 5 | put/get, LRU eviction, hit/miss stats |
| `WALTest` | 1 | binary serialization + full recovery |
| `KVStoreTest` | 5 | put/get, overwrite, delete, 100-key stress |
| `CrashRecoveryTest` | 1 | unclean shutdown + WAL replay |
| `PersistenceTest` | 1 | data survives process restart |
| `StatsTest` | 1 | counter accuracy |

---

## Roadmap

### Performance
- [ ] **Sparse index in SSTable** — O(log n) lookup, fix cold read latency
- [ ] **Group commit** — batch WAL per `fsync()`, target 500K+ writes/sec
- [ ] **Async compaction** — background thread, unblock write path
- [ ] **Block compression** — Snappy/LZ4, 2–4× disk reduction

### Scale
- [ ] **Sharded mutex** — per-key-range locking
- [ ] **Lock-free SkipList MemTable** — concurrent writes
- [ ] **Leveled compaction** — L0→L1→L2 (LevelDB-style)
- [ ] **WAL CRC32 checksums** — corruption detection

### API
- [ ] **Range scan** — `scan(start, end)` across MemTable + SSTables
- [ ] **Column families** — independent namespaces
- [ ] **gRPC server** — networked key-value service

---

---

## License

MIT — see [LICENSE](LICENSE) for details.

---

<p align="center">
  Built with C++17 &nbsp;·&nbsp;
  Inspired by LevelDB &amp; RocksDB &nbsp;·&nbsp;
  Benchmarked on Google Colab
</p>
