#include "CMap.h"
#include "SpinLock.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

static int g_sl_failures = 0;
static int g_sl_checks   = 0;

#define SL_CHECK(cond) do {                                  \
    ++g_sl_checks;                                           \
    if (!(cond)) {                                           \
        ++g_sl_failures;                                     \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                        \
} while (0)

// ---------------------------------------------------------------------------
// CMap tests
// ---------------------------------------------------------------------------

static void test_cmap_basic_crud() {
    using namespace StreamLine::Containers;
    CMap<uint64_t> m(64);

    // empty map Find
    SL_CHECK(!m.Find("missing").has_value());

    // insert
    auto r = m.Set("alpha", 1ull);
    SL_CHECK(r.err == CMapError::NO_ERR);
    SL_CHECK(r.inserted == true);

    // find
    auto v = m.Find("alpha");
    SL_CHECK(v.has_value() && *v == 1ull);

    // update (existing key)
    r = m.Set("alpha", 2ull);
    SL_CHECK(r.err == CMapError::NO_ERR);
    SL_CHECK(r.inserted == false);
    v = m.Find("alpha");
    SL_CHECK(v.has_value() && *v == 2ull);

    // erase present
    auto e = m.FindAndErase("alpha");
    SL_CHECK(e.err == CMapError::NO_ERR);
    SL_CHECK(e.value.has_value() && *e.value == 2ull);
    SL_CHECK(!m.Find("alpha").has_value());

    // erase missing
    e = m.FindAndErase("nope");
    SL_CHECK(e.err == CMapError::KEY_NOT_FOUND);
    SL_CHECK(!e.value.has_value());
}

static void test_cmap_capacity_and_tombstones() {
    using namespace StreamLine::Containers;
    // capacity 16 gets 32 slots (load factor <= 7/8). Fill the requested capacity.
    CMap<uint64_t> m(16);
    for (uint64_t i = 0; i < 16; i++) {
        std::string k = "k" + std::to_string(i);
        auto r = m.Set(k, i);
        SL_CHECK(r.err == CMapError::NO_ERR);
    }
    // Repeatedly setting NEW keys should eventually hit MAP_FULL or ABANDONED_SET.
    // Verify MAP_FULL is reachable for at least one new key.
    bool sawMapFull = false;
    for (uint64_t i = 16; i < 256; i++) {
        std::string k = "k" + std::to_string(i);
        auto r = m.Set(k, i);
        if (r.err == CMapError::MAP_FULL) { sawMapFull = true; break; }
        SL_CHECK(r.err == CMapError::NO_ERR);
    }
    SL_CHECK(sawMapFull);

    // tombstone reuse: erase some keys, then inserts should reuse deleted slots.
    for (uint64_t i = 0; i < 8; i++) {
        m.FindAndErase("k" + std::to_string(i));
    }
    bool reused = false;
    for (uint64_t i = 0; i < 8; i++) {
        std::string k = "r" + std::to_string(i);
        auto r = m.Set(k, i);
        if (r.err == CMapError::NO_ERR) reused = true;
    }
    SL_CHECK(reused);
}

static void test_cmap_hash_path_and_aux() {
    using namespace StreamLine::Containers;
    CMap<uint64_t> m(64);
    m.Set("hello", 7ull);

    // FindByHash round-trip
    HashPair h = HashKey("hello");
    auto v = m.FindByHash(h);
    SL_CHECK(v.has_value() && *v == 7ull);

    // FindWithAux shows zeroed aux on a fresh insert
    auto a = m.FindWithAux(h);
    SL_CHECK(a.has_value() && a->value == 7ull);
    bool auxZero = true;
    for (int i = 0; i < 7; i++) if (a->aux[i] != 0) auxZero = false;
    SL_CHECK(auxZero);

    // bump aux[0] atomically (same mechanism S3-FIFO uses in the source's Find),
    // then EvictWithAux returns the snapshot. Find the slot's aux address via
    // FindWithAux and add; the aux pointer is valid until slot reuse.
    {
        auto a2 = m.FindWithAux(h);
        SL_CHECK(a2.has_value());
        std::atomic_ref<uint8_t> counter(*const_cast<uint8_t*>(a2->aux));
        counter.fetch_add(1, std::memory_order_relaxed);
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    auto ev = m.EvictWithAux(h);
    SL_CHECK(ev.has_value());
    SL_CHECK(ev->value == 7ull);
    SL_CHECK(ev->aux[0] == 2);
    // after evict, Find returns nothing
    SL_CHECK(!m.Find("hello").has_value());
}

static void test_cmap_update_in_place() {
    using namespace StreamLine::Containers;
    CMap<uint64_t> m(64);
    m.Set("counter", 100ull);

    // mutate under the seqlock via fn
    auto err = m.UpdateInPlace("counter", [](uint64_t& v) { v += 5; });
    SL_CHECK(err == CMapError::NO_ERR);
    auto v = m.Find("counter");
    SL_CHECK(v.has_value() && *v == 105ull);

    // missing key -> KEY_NOT_FOUND (checked before locking)
    err = m.UpdateInPlace("ghost", [](uint64_t&) {});
    SL_CHECK(err == CMapError::KEY_NOT_FOUND);
}

static void test_cmap_probe_sequence() {
    using namespace StreamLine::Containers;
    CMap<uint64_t> m(64);
    // force collisions/probing: insert many keys
    std::vector<std::string> keys;
    for (int i = 0; i < 40; i++) {
        keys.push_back("key_" + std::to_string(i));
        m.Set(keys.back(), static_cast<uint64_t>(i));
    }
    // erase a middle key, confirm neighbors still findable
    m.FindAndErase("key_20");
    SL_CHECK(!m.Find("key_20").has_value());
    SL_CHECK(m.Find("key_19").has_value());
    SL_CHECK(m.Find("key_21").has_value());
    // re-check all survivors
    int found = 0;
    for (int i = 0; i < 40; i++) {
        if (i == 20) continue;
        if (m.Find("key_" + std::to_string(i)).has_value()) ++found;
    }
    SL_CHECK(found == 39);
}

static void test_cmap_small_capacities() {
    using namespace StreamLine::Containers;
    // Capacities below one 16-slot group used to yield zero groups, so every
    // Set reported MAP_FULL.
    for (size_t cap : {1u, 2u, 8u, 15u, 16u, 17u}) {
        CMap<uint64_t> m(cap);
        bool ok = true;
        for (uint64_t i = 0; i < cap; i++)
            ok &= m.Set("c" + std::to_string(i), i).err == CMapError::NO_ERR;
        for (uint64_t i = 0; i < cap; i++) {
            auto v = m.Find("c" + std::to_string(i));
            ok &= v.has_value() && *v == i;
        }
        SL_CHECK(ok);
    }
}

// Erase/insert churn at full capacity must not fill the table with tombstones;
// once every free slot is a tombstone, each miss scans the whole table.
static void test_cmap_tombstones_bounded_under_churn() {
    using namespace StreamLine::Containers;
    const size_t slots = 4096;
    const size_t live = slots * 7 / 8;  // the most CMap(live) will ever hold
    CMap<uint64_t> m(live);
    SL_CHECK(m.SlotCount() == slots);

    for (uint64_t i = 0; i < live; i++) m.Set("k" + std::to_string(i), i);
    bool ok = true;
    for (uint64_t next = live; next < live + 50 * slots; next++) {
        ok &= m.Erase("k" + std::to_string(next - live)) == CMapError::NO_ERR;
        ok &= m.Set("k" + std::to_string(next), next).err == CMapError::NO_ERR;
    }
    SL_CHECK(ok);
    SL_CHECK(m.TombstoneCount() < slots / 16);

    // Purges move entries: every live key must still map to its own value.
    uint64_t first = 50 * slots;
    int found = 0;
    for (uint64_t i = first; i < first + live; i++) {
        auto v = m.Find("k" + std::to_string(i));
        found += v.has_value() && *v == i;
    }
    SL_CHECK(found == static_cast<int>(live));
    SL_CHECK(!m.Find("k" + std::to_string(first - 1)).has_value());
}

// Many threads inserting distinct keys concurrently. Before writers were
// serialized, an inserter could overwrite a slot another thread had just
// claimed and lose that key (a few rounds in a thousand, at any -O level).
static void test_cmap_concurrent_writers_lose_nothing() {
    using namespace StreamLine::Containers;
    constexpr int kRounds = 300, N = 8, K = 100;
    int badRounds = 0;
    for (int round = 0; round < kRounds; round++) {
        CMap<uint64_t> m(N * K);
        std::atomic<int> failedSets{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < N; t++) {
            ts.emplace_back([&m, t, &failedSets]() {
                for (int i = 0; i < K; i++) {
                    std::string k = "t" + std::to_string(t) + "_k" + std::to_string(i);
                    if (m.Set(k, static_cast<uint64_t>(t * 1000 + i)).err != CMapError::NO_ERR)
                        ++failedSets;
                }
            });
        }
        for (auto& th : ts) th.join();

        int found = 0;
        for (int t = 0; t < N; t++)
            for (int i = 0; i < K; i++) {
                auto v = m.Find("t" + std::to_string(t) + "_k" + std::to_string(i));
                found += v.has_value() && *v == static_cast<uint64_t>(t * 1000 + i);
            }
        if (failedSets.load() != 0 || found != N * K) ++badRounds;
    }
    SL_CHECK(badRounds == 0);
}

// Concurrent writers racing on the SAME keys must never leave duplicates:
// after erasing each key once, it must be gone.
static void test_cmap_concurrent_same_key_no_duplicates() {
    using namespace StreamLine::Containers;
    constexpr int N = 8, K = 200;
    CMap<uint64_t> m(K);
    for (int round = 0; round < 50; round++) {
        std::vector<std::thread> ts;
        for (int t = 0; t < N; t++)
            ts.emplace_back([&m, t]() {
                for (int i = 0; i < K; i++) {
                    std::string k = "s" + std::to_string(i);
                    m.Set(k, static_cast<uint64_t>(i));
                    if ((i + t) % 3 == 0) m.Erase(k);
                }
            });
        for (auto& th : ts) th.join();
    }
    bool ok = true;
    for (int i = 0; i < K; i++) {
        std::string k = "s" + std::to_string(i);
        m.Erase(k);
        ok &= !m.Find(k).has_value();
    }
    SL_CHECK(ok);
}

// Lock-free readers under erase/insert churn must never see another key's
// value: a slot can be erased and reused between Probe() and the read.
static void test_cmap_readers_never_see_other_keys_value() {
    using namespace StreamLine::Containers;
    CMap<uint64_t> m(4096);
    constexpr uint64_t live = 2048;
    for (uint64_t i = 0; i < live; i++) m.Set("k" + std::to_string(i), i);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> wrong{0};
    std::vector<std::thread> readers;
    unsigned nReaders = std::max(2u, std::thread::hardware_concurrency()) - 1;
    for (unsigned r = 0; r < nReaders; r++)
        readers.emplace_back([&, r]() {
            uint64_t x = 7919 * (r + 1);
            while (!stop.load(std::memory_order_relaxed)) {
                x = x * 6364136223846793005ull + 1442695040888963407ull;
                uint64_t id = (x >> 33) % 200000;
                auto v = m.Find("k" + std::to_string(id));
                if (v && *v != id) ++wrong;
                m.UpdateInPlace("k" + std::to_string(id), [id](uint64_t& val) {
                    if (val == id) val = id;  // touch the slot under its seqlock
                });
            }
        });

    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t next = live; std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2); next++) {
        m.Erase("k" + std::to_string(next - live));
        m.Set("k" + std::to_string(next), next);
    }
    stop = true;
    for (auto& th : readers) th.join();
    SL_CHECK(wrong.load() == 0);
}

// ---------------------------------------------------------------------------
// Lock tests
// ---------------------------------------------------------------------------

static void test_spinlock_mutual_exclusion() {
    using namespace StreamLine::Locks;
    SpinLock lk;
    std::atomic<uint64_t> counter{0};
    constexpr int N = 8;
    constexpr int K = 1000;
    std::vector<std::thread> ts;
    for (int t = 0; t < N; t++) {
        ts.emplace_back([&]() {
            for (int i = 0; i < K; i++) {
                std::lock_guard<SpinLock> g(lk);
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts) th.join();
    SL_CHECK(counter.load() == static_cast<uint64_t>(N) * K);
}

static void test_mcslock_mutual_exclusion() {
    using namespace StreamLine::Locks;
    MCSLock lk;
    std::atomic<uint64_t> counter{0};
    constexpr int N = 8;
    constexpr int K = 1000;
    std::vector<std::thread> ts;
    for (int t = 0; t < N; t++) {
        ts.emplace_back([&]() {
            for (int i = 0; i < K; i++) {
                lk.lock();
                counter.fetch_add(1, std::memory_order_relaxed);
                lk.unlock();
            }
        });
    }
    for (auto& th : ts) th.join();
    SL_CHECK(counter.load() == static_cast<uint64_t>(N) * K);
}

int main() {
    // NOTE: The original test.cpp called StreamLine::Bootstrap::Initialize here,
    // but that pulls in StreamLine.h -> Task.h, which is mid-refactor upstream
    // (pre-existing build break, not caused by the CMap port). The CMap/lock
    // tests below don't need the ThreadPool bootstrap, so we omit it to keep
    // the port's tests green independently of the Task.h work-in-progress.

    test_cmap_basic_crud();
    test_cmap_capacity_and_tombstones();
    test_cmap_hash_path_and_aux();
    test_cmap_update_in_place();
    test_cmap_probe_sequence();
    test_cmap_small_capacities();
    test_cmap_tombstones_bounded_under_churn();
    test_cmap_concurrent_writers_lose_nothing();
    test_cmap_concurrent_same_key_no_duplicates();
    test_cmap_readers_never_see_other_keys_value();
    test_spinlock_mutual_exclusion();
    test_mcslock_mutual_exclusion();

    std::printf("Streamline tests: %d checks, %d failures\n", g_sl_checks, g_sl_failures);
    return g_sl_failures == 0 ? 0 : 1;
}
