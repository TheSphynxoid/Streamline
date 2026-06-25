#pragma once

// CMap: concurrent Swiss-table hash map. Ported from Furrballs
// (commit 73d5d77, Furrballs/include/CMap.h) as a one-way port.
// Deltas from the source:
//   - Namespace moved to StreamLine::Containers.
//   - Error codes are a minimal scoped CMapError enum (no Furrballs Error.h);
//     CACHE_FULL renamed to MAP_FULL (CMap is a map, not a cache).
//   - UpdateInPlace / UpdateInPlaceByHash no longer reference a Page::DataOffset
//     null-check; a generic map has no "found-but-invalid" state. fn() is the
//     only user code under the seqlock.
//   - Eviction policies (ConcurrentARC/LRU/S3FIFO, FlatList, ArcLists, OpenIdx,
//     PromoteBuf) are NOT ported — they live in Furrballs.
//   - SIMD intrinsics go through StreamLine::Detail (SimdPort.h) for x86-64/ARM64.

#include "SimdPort.h"
#include "third_party/xxhash/xxh3.h"

#include <bit>
#include <atomic>
#include <optional>
#include <cstring>
#include <cassert>
#include <new>
#include <string>
#include <algorithm>
#include <cstdint>

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
        ABANDONED_SET,   // lost a seqlock/CAS race; caller may retry.
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

    template <class Value>
        requires std::is_move_constructible_v<Value> && std::is_trivially_copyable_v<Value>
    class CMap {
      private:
        static constexpr unsigned int kGroupSize = 16;

        struct alignas(64) Slot {
            static constexpr bool kNeedsSplit = (16 + sizeof(Value)) > 64;

            std::atomic<uint8_t> seq;
            uint8_t aux[7];               // policy-owned scratchpad (zeroed on insert)
            std::atomic<uint64_t> fingerprint;

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

      public:
        CMap(size_t capacity,
             CMapAllocFn af = CMapDefaultAlloc,
             CMapFreeFn ff = CMapDefaultFree)
            : allocFn_(af), freeFn_(ff)
        {
            size_t allocCap = 1;
            while (allocCap < capacity) allocCap <<= 1;

            numGroups_ = allocCap / kGroupSize;
            slotCount_ = allocCap;

            slots_ = static_cast<Slot*>(allocFn_(slotCount_ * sizeof(Slot)));
            for (size_t i = 0; i < slotCount_; ++i)
                new (&slots_[i]) Slot{};
            ctrl_ = static_cast<uint8_t*>(allocFn_(slotCount_ + kGroupSize));

            memset(ctrl_, 0x80, slotCount_ + kGroupSize);
            for (size_t i = slotCount_; i < slotCount_ + kGroupSize; i++) {
                ctrl_[i] = 0xFF;
            }
        }

        ~CMap() {
            if (slots_) freeFn_(slots_, slotCount_ * sizeof(Slot));
            if (ctrl_) freeFn_(ctrl_, slotCount_ + kGroupSize);
        }

        CMap(const CMap&) = delete;
        CMap& operator=(const CMap&) = delete;

        int MatchCtrlMask(Detail::sl_m128i ctrl_vec, uint8_t target) const noexcept {
            return Detail::sl_movemask_epi8(
                Detail::sl_cmpeq_epi8(ctrl_vec, Detail::sl_set1_epi8(static_cast<int8_t>(target))));
        }

        // x86-64 guarantees byte-aligned stores are atomic.
        // SIMD load + concurrent CAS on same ctrl byte is safe in practice.
        bool CasCtrl(size_t slotIdx, uint8_t expected, uint8_t desired) noexcept {
            return __atomic_compare_exchange_n(
                &ctrl_[slotIdx], &expected, desired,
                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
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
            HashPair pair = HashKey(key);
            return FindByHash(pair);
        }

        std::optional<Value> FindByHash(const HashPair& pair) const noexcept {
            ProbeResult result = Probe<false>(pair);
            if (result.matchSlot == SIZE_MAX) return std::nullopt;
            const Slot& slot = Slots()[result.matchSlot];
            alignas(Value) std::byte buf[sizeof(Value)];
            uint8_t s1;
            do {
                s1 = slot.seq.load(std::memory_order_acquire);
                if (s1 & 1) { Detail::sl_pause(); continue; }
                std::memcpy(buf, &slot.value, sizeof(Value));
            } while (s1 != slot.seq.load(std::memory_order_acquire));
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
            uint8_t s1;
            do {
                s1 = slot.seq.load(std::memory_order_acquire);
                if (s1 & 1) { Detail::sl_pause(); continue; }
                std::memcpy(buf, &slot.value, sizeof(Value));
            } while (s1 != slot.seq.load(std::memory_order_acquire));
            return AuxRef{std::bit_cast<Value>(buf), slot.aux};
        }

        struct AuxEvictRef {
            Value value;
            uint8_t aux[7];      // snapshot at eviction time
        };

        std::optional<AuxEvictRef> EvictWithAux(const HashPair& hashes) noexcept {
            ProbeResult result = Probe<false>(hashes);
            if (result.matchSlot == SIZE_MAX) return std::nullopt;

            Slot& targetSlot = Slots()[result.matchSlot];
            uint8_t expected = targetSlot.seq.load(std::memory_order_acquire);
            if (expected & 1) return std::nullopt;
            if (!targetSlot.seq.compare_exchange_strong(expected, expected + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return std::nullopt;
            }

            if (!CasCtrl(result.matchSlot, static_cast<uint8_t>(hashes.h2 >> 57),
                    static_cast<uint8_t>(CMapCtrlState::kDeleted))) {
                targetSlot.seq.store(expected, std::memory_order_release);
                return std::nullopt;
            }

            alignas(Value) std::byte buf[sizeof(Value)];
            std::memcpy(buf, &targetSlot.value, sizeof(Value));
            uint8_t auxCopy[7];
            std::memcpy(auxCopy, targetSlot.aux, 7);
            targetSlot.fingerprint.store(0, std::memory_order_relaxed);
            std::memset(&targetSlot.value, 0, sizeof(Value));
            targetSlot.seq.store(expected + 2, std::memory_order_release);
            AuxEvictRef out{std::bit_cast<Value>(buf), {}};
            std::memcpy(out.aux, auxCopy, 7);
            return out;
        }

        CMapSetResult Set(std::string_view key, const Value& val) noexcept {
            HashPair pair = HashKey(key);
            auto h2_short = static_cast<uint8_t>(pair.h2 >> 57);
            ProbeResult result = Probe<true>(pair);
            if (result.deletedSlot == SIZE_MAX && result.matchSlot == SIZE_MAX && result.emptySlot == SIZE_MAX) {
                return {CMapError::MAP_FULL, false};
            }

            if (result.matchSlot != SIZE_MAX) {
                Slot& targetSlot = Slots()[result.matchSlot];
                uint8_t expected = targetSlot.seq.load(std::memory_order_acquire);
                if (expected & 1) return {CMapError::ABANDONED_SET, false};
                if (!targetSlot.seq.compare_exchange_strong(expected, expected + 1,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                    return {CMapError::ABANDONED_SET, false};

                targetSlot.fingerprint.store(pair.h2, std::memory_order_relaxed);
                std::memcpy(&targetSlot.value, &val, sizeof(Value));
                targetSlot.seq.store(expected + 2, std::memory_order_release);
                return {CMapError::NO_ERR, false};
            }

            size_t targetIdx = result.deletedSlot != SIZE_MAX ? result.deletedSlot : result.emptySlot;
            Slot& targetSlot = Slots()[targetIdx];

            uint8_t expected = targetSlot.seq.load(std::memory_order_acquire);
            if (expected & 1) return {CMapError::ABANDONED_SET, false};
            if (!targetSlot.seq.compare_exchange_strong(expected, expected + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return {CMapError::ABANDONED_SET, false};

            targetSlot.fingerprint.store(pair.h2, std::memory_order_relaxed);
            std::memset(targetSlot.aux, 0, 7);
            if (!CasCtrl(targetIdx, static_cast<uint8_t>(CMapCtrlState::kDeleted), h2_short) &&
                !CasCtrl(targetIdx, static_cast<uint8_t>(CMapCtrlState::kEmpty), h2_short)) {
                targetSlot.seq.store(expected, std::memory_order_release);
                return {CMapError::ABANDONED_SET, false};
            }

            std::memcpy(&targetSlot.value, &val, sizeof(Value));
            targetSlot.seq.store(expected + 2, std::memory_order_release);
            return {CMapError::NO_ERR, true};
        }

        // UpdateInPlace: acquires the slot's seqlock and calls fn(value).
        // fn() is the ONLY user code under the seqlock. The source's
        // Page::DataOffset null-check is intentionally absent: a generic map
        // has no "found-but-invalid" state. If fn needs to bail, it does so on
        // its own terms.
        template <typename Fn>
        CMapError UpdateInPlace(std::string_view key, Fn&& fn) noexcept {
            HashPair pair = HashKey(key);
            ProbeResult result = Probe<false>(pair);
            if (result.matchSlot == SIZE_MAX) return CMapError::KEY_NOT_FOUND;

            Slot& targetSlot = Slots()[result.matchSlot];
            uint8_t expected = targetSlot.seq.load(std::memory_order_acquire);
            if (expected & 1) return CMapError::ABANDONED_SET;
            if (!targetSlot.seq.compare_exchange_strong(expected, expected + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return CMapError::ABANDONED_SET;

            fn(targetSlot.value);

            targetSlot.seq.store(expected + 2, std::memory_order_release);
            return CMapError::NO_ERR;
        }

        template <typename Fn>
        CMapError UpdateInPlaceByHash(const HashPair& hashes, Fn&& fn) noexcept {
            ProbeResult result = Probe<false>(hashes);
            if (result.matchSlot == SIZE_MAX) return CMapError::KEY_NOT_FOUND;
            Slot& targetSlot = Slots()[result.matchSlot];
            uint8_t expected = targetSlot.seq.load(std::memory_order_acquire);
            if (expected & 1) return CMapError::ABANDONED_SET;
            if (!targetSlot.seq.compare_exchange_strong(expected, expected + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return CMapError::ABANDONED_SET;

            fn(targetSlot.value);

            targetSlot.seq.store(expected + 2, std::memory_order_release);
            return CMapError::NO_ERR;
        }

        CMapError Erase(std::string_view key) noexcept {
            return FindAndErase(key).err;
        }

        struct FindAndEraseResult {
            CMapError err = CMapError::KEY_NOT_FOUND;
            std::optional<Value> value = std::nullopt;
        };

        FindAndEraseResult FindAndErase(std::string_view key) noexcept {
            FindAndEraseResult out;
            HashPair pair = HashKey(key);
            ProbeResult result = Probe<false>(pair);
            if (result.matchSlot == SIZE_MAX) return out;

            Slot& targetSlot = Slots()[result.matchSlot];
            uint8_t expected = targetSlot.seq.load(std::memory_order_acquire);
            if (expected & 1) { out.err = CMapError::ABANDONED_SET; return out; }
            if (!targetSlot.seq.compare_exchange_strong(expected, expected + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                out.err = CMapError::ABANDONED_SET;
                return out;
            }

            if (!CasCtrl(result.matchSlot, static_cast<uint8_t>(pair.h2 >> 57),
                    static_cast<uint8_t>(CMapCtrlState::kDeleted))) {
                targetSlot.seq.store(expected, std::memory_order_release);
                out.err = CMapError::ABANDONED_SET;
                return out;
            }

            alignas(Value) std::byte buf[sizeof(Value)];
            std::memcpy(buf, &targetSlot.value, sizeof(Value));
            targetSlot.fingerprint.store(0, std::memory_order_relaxed);
            std::memset(&targetSlot.value, 0, sizeof(Value));
            targetSlot.seq.store(expected + 2, std::memory_order_release);
            out.err = CMapError::NO_ERR;
            out.value = std::bit_cast<Value>(buf);
            return out;
        }

        FindAndEraseResult FindAndEraseByHash(const HashPair& hashes) noexcept {
            FindAndEraseResult out;
            ProbeResult result = Probe<false>(hashes);
            if (result.matchSlot == SIZE_MAX) return out;

            Slot& targetSlot = Slots()[result.matchSlot];
            uint8_t expected = targetSlot.seq.load(std::memory_order_acquire);
            if (expected & 1) { out.err = CMapError::ABANDONED_SET; return out; }
            if (!targetSlot.seq.compare_exchange_strong(expected, expected + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                out.err = CMapError::ABANDONED_SET;
                return out;
            }

            if (!CasCtrl(result.matchSlot, static_cast<uint8_t>(hashes.h2 >> 57),
                    static_cast<uint8_t>(CMapCtrlState::kDeleted))) {
                targetSlot.seq.store(expected, std::memory_order_release);
                out.err = CMapError::ABANDONED_SET;
                return out;
            }

            alignas(Value) std::byte buf[sizeof(Value)];
            std::memcpy(buf, &targetSlot.value, sizeof(Value));
            targetSlot.fingerprint.store(0, std::memory_order_relaxed);
            std::memset(&targetSlot.value, 0, sizeof(Value));
            targetSlot.seq.store(expected + 2, std::memory_order_release);
            out.err = CMapError::NO_ERR;
            out.value = std::bit_cast<Value>(buf);
            return out;
        }
    };

} // namespace StreamLine::Containers
