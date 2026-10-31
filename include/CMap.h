#pragma once

// CMap: concurrent Swiss-table hash map with lock-free reads.
// Ported from Furrballs (Furrballs/include/CMap.h) as a one-way port.
// Deltas from the source:
//   - Namespace moved to StreamLine::Containers.
//   - Error codes are a minimal scoped CMapError enum (no Furrballs Error.h);
//     CACHE_FULL renamed to MAP_FULL (CMap is a map, not a cache).
//   - Writers (Set / Erase / EvictWithAux) serialize on an internal WriterLock.
//     Furrballs relies on its eviction policy's lock for that; a standalone map
//     must provide it itself. Reads never take the lock.
//   - UpdateInPlace / UpdateInPlaceByHash have no Page::DataOffset null-check;
//     a generic map has no "found-but-invalid" state.
//   - Eviction policies (ConcurrentARC/LRU/S3FIFO, FlatList, ArcLists, OpenIdx,
//     PromoteBuf) are NOT ported — they live in Furrballs.
//   - SIMD intrinsics go through StreamLine::Detail (SimdPort.h) for x86-64/ARM64.
//
// Concurrency model:
//   - Find / FindByHash / FindWithAux: lock-free; per-slot seqlock reads that
//     re-check the full 64-bit fingerprint, so a slot erased and reused by
//     another key between probe and read reads as a miss.
//   - Set / Erase / FindAndErase* / EvictWithAux: take WriterLock. They wait
//     out in-place updaters rather than failing, so with a writer lock they
//     only fail with MAP_FULL or KEY_NOT_FOUND.
//   - UpdateInPlace*: lock-free; holds only the slot's seqlock while fn runs.
//     Returns ABANDONED_SET if another updater or a tombstone purge holds it.

#include "Concept.h"
#include "SimdPort.h"
#include "SpinLock.h"
#include "third_party/xxhash/xxh3.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>
#include <vector>

namespace StreamLine::Containers {

    struct HashPair {
        uint64_t h1, h2;
        bool operator==(const HashPair& o) const noexcept { return h1 == o.h1 && h2 == o.h2; }
    };

    // Minimal error codes for CMap's return paths. Scoped to CMap — callers wrap
    // these however they like (we do not impose a Result<T,E> type).
    enum class CMapError {
        NO_ERR,
        KEY_NOT_FOUND,
        MAP_FULL,        // probe chain saturated; no empty/deleted slot available.
        ABANDONED_SET,   // UpdateInPlace lost the slot's seqlock; caller may retry.
    };

    struct CMapSetResult {
        CMapError err;
        bool inserted; // true = INSERT (new slot), false = UPDATE (existing slot)
    };

    enum class CMapCtrlState : uint8_t {
        kEmpty    = 0x80,
        kDeleted  = 0xFE,
        kSentinel = 0xFF,
    };

    struct ProbeResult {
        size_t matchSlot   = SIZE_MAX;
        size_t deletedSlot = SIZE_MAX;
        size_t emptySlot   = SIZE_MAX;
    };

    // h2 is also used for the per-slot fingerprint to reduce collisions. We are
    // reducing the probability, but we can't remove it.
    inline HashPair HashKey(std::string_view key) {
        XXH128_hash_t h = XXH3_128bits(key.data(), key.size());
        return {h.high64, h.low64};
    }

    using CMapAllocFn = void*(*)(size_t);
    using CMapFreeFn  = void(*)(void*, size_t);

    inline void* CMapDefaultAlloc(size_t size) {
        return ::operator new(size, std::align_val_t{64});
    }
    inline void CMapDefaultFree(void* ptr, size_t) {
        ::operator delete(ptr, std::align_val_t{64});
    }

    template <class Value, Lockable WriterLock = Locks::SpinLock>
        requires std::is_move_constructible_v<Value> && std::is_trivially_copyable_v<Value>
    class CMap {
      private:
        static constexpr unsigned int kGroupSize = 16;

        struct alignas(64) Slot {
            static constexpr bool kNeedsSplit = (24 + sizeof(Value)) > 64;

            std::atomic<uint8_t> seq;
            uint8_t aux[7];               // policy-owned scratchpad (zeroed on insert)
            std::atomic<uint64_t> fingerprint;
            uint64_t h1;                  // home-group hash, for in-place rehash

            alignas(kNeedsSplit ? 64 : alignof(Value)) Value value;
        };

        const uint8_t* Ctrl() const { return ctrl_; }
        Slot* Slots() { return slots_; }
        const Slot* Slots() const { return slots_; }

        Slot* slots_ = nullptr;
        uint8_t* ctrl_ = nullptr;
        CMapAllocFn allocFn_ = nullptr;
        CMapFreeFn freeFn_ = nullptr;
        size_t numGroups_ = 0;
        size_t slotCount_ = 0;
        size_t tombstones_ = 0;  // guarded by writerLock_
        WriterLock writerLock_;

        int MatchCtrlMask(Detail::sl_m128i ctrl_vec, uint8_t target) const noexcept {
            return Detail::sl_movemask_epi8(
                Detail::sl_cmpeq_epi8(ctrl_vec, Detail::sl_set1_epi8(static_cast<int8_t>(target))));
        }

        // Ctrl bytes are read by SIMD loads without synchronization; writes go
        // through atomic_ref so they are at least single-byte atomic stores.
        void StoreCtrl(size_t slotIdx, uint8_t v) noexcept {
            std::atomic_ref<uint8_t>(ctrl_[slotIdx]).store(v, std::memory_order_release);
        }

        // Spins until the slot's seqlock is held. Under writerLock_ the only
        // other holders are in-place updaters (a short fn call), so waiting
        // is bounded. Returns the even sequence value that was locked.
        static uint8_t LockSlot(Slot& slot) noexcept {
            uint8_t s = slot.seq.load(std::memory_order_relaxed);
            while ((s & 1) || !slot.seq.compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                                              std::memory_order_relaxed)) {
                Detail::sl_pause();
                s = slot.seq.load(std::memory_order_relaxed);
            }
            return s;
        }

        // Ctrl state for a slot being erased. Probe stops at the first group
        // holding an EMPTY, so if this group still has one, no probe chain has
        // ever continued past it and the slot can go straight back to EMPTY.
        // Only groups that were once completely full accumulate tombstones.
        uint8_t ErasedCtrl(size_t slotIdx) const noexcept {
            size_t group = slotIdx / kGroupSize;
            Detail::sl_m128i ctrl_vec = Detail::sl_loadu_si128(Ctrl() + group * kGroupSize);
            int emptyMask = MatchCtrlMask(ctrl_vec, static_cast<uint8_t>(CMapCtrlState::kEmpty));
            return emptyMask != 0 ? static_cast<uint8_t>(CMapCtrlState::kEmpty)
                                  : static_cast<uint8_t>(CMapCtrlState::kDeleted);
        }

        // Removes the entry in slotIdx. Requires writerLock_.
        void EraseSlotLocked(size_t slotIdx, std::byte* valueOut, uint8_t* auxOut) noexcept {
            Slot& slot = Slots()[slotIdx];
            uint8_t s = LockSlot(slot);
            uint8_t erased = ErasedCtrl(slotIdx);
            StoreCtrl(slotIdx, erased);
            if (erased == static_cast<uint8_t>(CMapCtrlState::kDeleted)) ++tombstones_;
            if (valueOut) std::memcpy(valueOut, &slot.value, sizeof(Value));
            if (auxOut) std::memcpy(auxOut, slot.aux, 7);
            slot.fingerprint.store(0, std::memory_order_relaxed);
            std::memset(&slot.value, 0, sizeof(Value));
            slot.seq.store(s + 2, std::memory_order_release);
            MaybePurgeTombstones();
        }

        // Groups that were ever full never regain an EMPTY, so under churn every
        // free slot eventually becomes a tombstone and each miss scans the whole
        // table. Purge once tombstones reach 1/16 of the slots.
        void MaybePurgeTombstones() noexcept {
            if (tombstones_ * 16 < slotCount_) return;
            try {
                PurgeTombstones();
            } catch (const std::bad_alloc&) {
                // Keep serving with tombstones; the next erase retries.
            }
        }

        // Rebuilds the table in place without tombstones. Requires writerLock_.
        // Every slot's seqlock is held for the duration, so readers spin rather
        // than observe half-moved entries and in-place updates report
        // ABANDONED_SET. O(slots), amortized over the slots/16 erasures that
        // trigger it.
        void PurgeTombstones() {
            struct Entry {
                uint64_t h1, h2;
                uint8_t aux[7];
                alignas(Value) std::byte value[sizeof(Value)];
            };
            std::vector<Entry> live;
            live.reserve(slotCount_);  // may throw: nothing is locked yet

            for (size_t i = 0; i < slotCount_; ++i) LockSlot(slots_[i]);

            for (size_t i = 0; i < slotCount_; ++i) {
                if (ctrl_[i] & 0x80) continue;  // EMPTY or DELETED
                Slot& slot = slots_[i];
                Entry& e = live.emplace_back();
                e.h1 = slot.h1;
                e.h2 = slot.fingerprint.load(std::memory_order_relaxed);
                std::memcpy(e.aux, slot.aux, 7);
                std::memcpy(e.value, &slot.value, sizeof(Value));
            }
            for (size_t i = 0; i < slotCount_; ++i) {
                StoreCtrl(i, static_cast<uint8_t>(CMapCtrlState::kEmpty));
                slots_[i].fingerprint.store(0, std::memory_order_relaxed);
            }
            for (const Entry& e : live) {
                size_t group = e.h1 & (numGroups_ - 1);
                int emptyMask;
                for (;;) {
                    Detail::sl_m128i ctrl_vec = Detail::sl_loadu_si128(Ctrl() + group * kGroupSize);
                    emptyMask = MatchCtrlMask(ctrl_vec, static_cast<uint8_t>(CMapCtrlState::kEmpty));
                    if (emptyMask != 0) break;
                    group = (group + 1) & (numGroups_ - 1);
                }
                size_t idx = group * kGroupSize + static_cast<size_t>(std::countr_zero(static_cast<unsigned>(emptyMask)));
                Slot& slot = slots_[idx];
                slot.h1 = e.h1;
                slot.fingerprint.store(e.h2, std::memory_order_relaxed);
                std::memcpy(slot.aux, e.aux, 7);
                std::memcpy(&slot.value, e.value, sizeof(Value));
                StoreCtrl(idx, static_cast<uint8_t>(e.h2 >> 57));
            }
            tombstones_ = 0;

            for (size_t i = 0; i < slotCount_; ++i)
                slots_[i].seq.fetch_add(1, std::memory_order_release);
        }

        // Seqlock read of a slot's value. The fingerprint is re-checked inside
        // the read window: between Probe() and this read the slot may have been
        // erased and reused by another key, which must read as a miss.
        bool ReadSlot(const Slot& slot, uint64_t h2, std::byte* buf) const noexcept {
            uint8_t s1;
            uint64_t fp = 0;
            do {
                s1 = slot.seq.load(std::memory_order_acquire);
                if (s1 & 1) { Detail::sl_pause(); continue; }
                fp = slot.fingerprint.load(std::memory_order_relaxed);
                std::memcpy(buf, &slot.value, sizeof(Value));
                std::atomic_thread_fence(std::memory_order_acquire);
            } while ((s1 & 1) || s1 != slot.seq.load(std::memory_order_relaxed));
            return fp == h2;
        }

        template <typename Fn>
        CMapError UpdateSlot(size_t slotIdx, uint64_t h2, Fn& fn) noexcept {
            Slot& targetSlot = Slots()[slotIdx];
            uint8_t expected = targetSlot.seq.load(std::memory_order_acquire);
            if (expected & 1) return CMapError::ABANDONED_SET;
            if (!targetSlot.seq.compare_exchange_strong(expected, expected + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return CMapError::ABANDONED_SET;

            // The slot may have been erased (and reused) since Probe().
            if (targetSlot.fingerprint.load(std::memory_order_relaxed) != h2) {
                targetSlot.seq.store(expected, std::memory_order_release);
                return CMapError::KEY_NOT_FOUND;
            }

            fn(targetSlot.value);

            targetSlot.seq.store(expected + 2, std::memory_order_release);
            return CMapError::NO_ERR;
        }

      public:
        CMap(size_t capacity,
             CMapAllocFn af = CMapDefaultAlloc,
             CMapFreeFn ff = CMapDefaultFree)
            : allocFn_(af), freeFn_(ff)
        {
            // Keep load factor <= 7/8 at full capacity: a near-full Swiss table
            // degrades every miss into a scan of most of the table.
            size_t target = capacity + capacity / 7;
            size_t allocCap = kGroupSize;
            while (allocCap < target) allocCap <<= 1;

            numGroups_ = allocCap / kGroupSize;
            slotCount_ = allocCap;

            slots_ = static_cast<Slot*>(allocFn_(slotCount_ * sizeof(Slot)));
            for (size_t i = 0; i < slotCount_; ++i)
                new (&slots_[i]) Slot{};
            ctrl_ = static_cast<uint8_t*>(allocFn_(slotCount_ + kGroupSize));

            std::memset(ctrl_, static_cast<int>(CMapCtrlState::kEmpty), slotCount_);
            std::memset(ctrl_ + slotCount_, static_cast<int>(CMapCtrlState::kSentinel), kGroupSize);
        }

        ~CMap() {
            if (slots_) freeFn_(slots_, slotCount_ * sizeof(Slot));
            if (ctrl_) freeFn_(ctrl_, slotCount_ + kGroupSize);
        }

        CMap(const CMap&) = delete;
        CMap& operator=(const CMap&) = delete;

        size_t SlotCount() const noexcept { return slotCount_; }

        // Diagnostic: number of tombstoned slots (O(slots), not for hot paths).
        size_t TombstoneCount() const noexcept {
            size_t n = 0;
            for (size_t i = 0; i < slotCount_; ++i)
                n += std::atomic_ref<uint8_t>(ctrl_[i]).load(std::memory_order_relaxed) ==
                     static_cast<uint8_t>(CMapCtrlState::kDeleted);
            return n;
        }

        template <bool FindDeleted = false> ProbeResult Probe(const HashPair& hashes) const noexcept {
            ProbeResult result;
            auto group = hashes.h1 & (numGroups_ - 1);
            auto h2_short = static_cast<uint8_t>(hashes.h2 >> 57);

            for (size_t probeCount = 0; probeCount < numGroups_; ++probeCount) {
                Detail::sl_m128i ctrl_vec =
                    Detail::sl_loadu_si128(Ctrl() + group * kGroupSize);
                int matchMask = MatchCtrlMask(ctrl_vec, h2_short);
                while (matchMask != 0) {
                    int i = std::countr_zero(static_cast<unsigned>(matchMask));
                    matchMask &= matchMask - 1;
                    size_t slotIdx = group * kGroupSize + static_cast<size_t>(i);
                    if (Slots()[slotIdx].fingerprint.load(std::memory_order_relaxed) == hashes.h2) {
                        result.matchSlot = slotIdx;
                        return result;
                    }
                }

                // Check for DELETED (only needed by Insert for tombstone reuse)
                if constexpr (FindDeleted) {
                    if (result.deletedSlot == SIZE_MAX) {
                        int delMask = MatchCtrlMask(ctrl_vec, static_cast<uint8_t>(CMapCtrlState::kDeleted));
                        if (delMask != 0) {
                            result.deletedSlot = group * kGroupSize +
                                static_cast<size_t>(std::countr_zero(static_cast<unsigned>(delMask)));
                        }
                    }
                }

                int emptyMask = MatchCtrlMask(ctrl_vec, static_cast<uint8_t>(CMapCtrlState::kEmpty));
                if (emptyMask != 0) {
                    result.emptySlot = group * kGroupSize +
                        static_cast<size_t>(std::countr_zero(static_cast<unsigned>(emptyMask)));
                    return result;
                }

                group = (group + 1) & (numGroups_ - 1);
            }
            return result;
        }

        std::optional<Value> Find(std::string_view key) const noexcept {
            return FindByHash(HashKey(key));
        }

        std::optional<Value> FindByHash(const HashPair& pair) const noexcept {
            ProbeResult result = Probe<false>(pair);
            if (result.matchSlot == SIZE_MAX) return std::nullopt;
            alignas(Value) std::byte buf[sizeof(Value)];
            if (!ReadSlot(Slots()[result.matchSlot], pair.h2, buf)) return std::nullopt;
            return std::bit_cast<Value>(buf);
        }

        struct AuxRef {
            Value value;
            const uint8_t* aux;  // pointer to 7 bytes, valid until slot reuse
        };

        std::optional<AuxRef> FindWithAux(const HashPair& pair) const noexcept {
            ProbeResult result = Probe<false>(pair);
            if (result.matchSlot == SIZE_MAX) return std::nullopt;
            const Slot& slot = Slots()[result.matchSlot];
            alignas(Value) std::byte buf[sizeof(Value)];
            if (!ReadSlot(slot, pair.h2, buf)) return std::nullopt;
            return AuxRef{std::bit_cast<Value>(buf), slot.aux};
        }

        struct AuxEvictRef {
            Value value;
            uint8_t aux[7];      // snapshot at eviction time
        };

        std::optional<AuxEvictRef> EvictWithAux(const HashPair& hashes) noexcept {
            std::lock_guard guard(writerLock_);
            ProbeResult result = Probe<false>(hashes);
            if (result.matchSlot == SIZE_MAX) return std::nullopt;

            alignas(Value) std::byte buf[sizeof(Value)];
            uint8_t aux[7];
            EraseSlotLocked(result.matchSlot, buf, aux);
            AuxEvictRef out{std::bit_cast<Value>(buf), {}};
            std::memcpy(out.aux, aux, 7);
            return out;
        }

        CMapSetResult Set(std::string_view key, const Value& val) noexcept {
            HashPair pair = HashKey(key);
            auto h2_short = static_cast<uint8_t>(pair.h2 >> 57);

            std::lock_guard guard(writerLock_);
            ProbeResult result = Probe<true>(pair);

            if (result.matchSlot != SIZE_MAX) {
                Slot& targetSlot = Slots()[result.matchSlot];
                uint8_t s = LockSlot(targetSlot);
                std::memcpy(&targetSlot.value, &val, sizeof(Value));
                targetSlot.seq.store(s + 2, std::memory_order_release);
                return {CMapError::NO_ERR, false};
            }

            size_t targetIdx = result.deletedSlot != SIZE_MAX ? result.deletedSlot : result.emptySlot;
            if (targetIdx == SIZE_MAX) return {CMapError::MAP_FULL, false};

            Slot& targetSlot = Slots()[targetIdx];
            uint8_t s = LockSlot(targetSlot);
            targetSlot.h1 = pair.h1;
            targetSlot.fingerprint.store(pair.h2, std::memory_order_relaxed);
            std::memset(targetSlot.aux, 0, 7);
            std::memcpy(&targetSlot.value, &val, sizeof(Value));
            if (targetIdx == result.deletedSlot) --tombstones_;
            StoreCtrl(targetIdx, h2_short);
            targetSlot.seq.store(s + 2, std::memory_order_release);
            return {CMapError::NO_ERR, true};
        }

        // UpdateInPlace: acquires the slot's seqlock and calls fn(value).
        // fn() is the ONLY user code under the seqlock; keep it short, since
        // readers of this slot and writers erasing it spin until it returns.
        template <typename Fn>
        CMapError UpdateInPlace(std::string_view key, Fn&& fn) noexcept {
            return UpdateInPlaceByHash(HashKey(key), fn);
        }

        template <typename Fn>
        CMapError UpdateInPlaceByHash(const HashPair& hashes, Fn&& fn) noexcept {
            ProbeResult result = Probe<false>(hashes);
            if (result.matchSlot == SIZE_MAX) return CMapError::KEY_NOT_FOUND;
            return UpdateSlot(result.matchSlot, hashes.h2, fn);
        }

        CMapError Erase(std::string_view key) noexcept {
            return FindAndErase(key).err;
        }

        struct FindAndEraseResult {
            CMapError err = CMapError::KEY_NOT_FOUND;
            std::optional<Value> value = std::nullopt;
        };

        FindAndEraseResult FindAndErase(std::string_view key) noexcept {
            return FindAndEraseByHash(HashKey(key));
        }

        FindAndEraseResult FindAndEraseByHash(const HashPair& hashes) noexcept {
            FindAndEraseResult out;
            std::lock_guard guard(writerLock_);
            ProbeResult result = Probe<false>(hashes);
            if (result.matchSlot == SIZE_MAX) return out;

            alignas(Value) std::byte buf[sizeof(Value)];
            EraseSlotLocked(result.matchSlot, buf, nullptr);
            out.err = CMapError::NO_ERR;
            out.value = std::bit_cast<Value>(buf);
            return out;
        }
    };

} // namespace StreamLine::Containers
