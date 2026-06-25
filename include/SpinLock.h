#pragma once

#include <atomic>
#include "SimdPort.h"

namespace StreamLine::Locks {

    // TTAS (test-and-test-and-set) spinlock. Satisfies the Lockable concept
    // (Concept.h), so it is a drop-in for WaitGroup<SpinLock>.
    class SpinLock {
        std::atomic<bool> locked_{false};
    public:
        void lock() noexcept {
            for (;;) {
                while (locked_.load(std::memory_order_relaxed)) {
                    Detail::sl_pause();
                }
                if (!locked_.exchange(true, std::memory_order_acquire)) return;
            }
        }
        bool try_lock() noexcept {
            return !locked_.exchange(true, std::memory_order_acquire);
        }
        void unlock() noexcept {
            locked_.store(false, std::memory_order_release);
        }
    };

    // MCS lock: fair (FIFO), queue-based. Each thread spins on its own
    // thread_local node's flag, so contention scales better than SpinLock
    // under high load.
    class alignas(64) MCSLock {
        struct Node {
            std::atomic<bool> waiting{true};
            Node* next{nullptr};
        };

        std::atomic<Node*> tail_{nullptr};

    public:
        void lock() noexcept {
            Node* me = &localNode_;
            me->waiting.store(true, std::memory_order_relaxed);
            me->next = nullptr;

            Node* prev = tail_.exchange(me, std::memory_order_acq_rel);
            if (!prev) return;

            prev->next = me;
            while (me->waiting.load(std::memory_order_acquire)) {
                Detail::sl_pause();
            }
        }

        void unlock() noexcept {
            Node* me = &localNode_;
            if (!me->next) {
                Node* expected = me;
                if (tail_.compare_exchange_strong(expected, nullptr,
                        std::memory_order_release, std::memory_order_relaxed)) {
                    return;
                }
                while (!me->next) {
                    Detail::sl_pause();
                }
            }
            me->next->waiting.store(false, std::memory_order_release);
            me->next = nullptr;
        }

    private:
        static thread_local Node localNode_;
    };

} // namespace StreamLine::Locks
