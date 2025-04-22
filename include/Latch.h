#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace StreamLine::Locks
{
    /// @brief A Spin-lock latch with a mutex/cv fallback.
    class HybridLatch {
    private:
        std::atomic<bool> ready{false};
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<unsigned int> spinCount{100}; // Default value
    public:
        void SetSpinCount(unsigned int count)noexcept {
            spinCount.store(count, std::memory_order_relaxed);
        }
        void Wait() {
            const unsigned int currentSpinCount = spinCount.load(std::memory_order_relaxed);
            // Spin phase
            for (int i = 0; i < currentSpinCount; ++i) {
                if (ready.load(std::memory_order_acquire)) return;
                std::this_thread::yield(); // Give CPU to other threads
            }

            // Fallback to CV
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] { return ready.load(std::memory_order_acquire); });
        }

        void Signal() {
            std::lock_guard<std::mutex> lock(mtx);
            ready.store(true, std::memory_order_release);
            cv.notify_all(); // Only wakes if someone already in the cv wait
        }

        void Reset() {
            std::lock_guard<std::mutex> lock(mtx);
            ready.store(false, std::memory_order_release);
        }

        bool IsReady() const {
            return ready.load(std::memory_order_acquire);
        }
    };
    /// @brief A Spin-lock latch. Useful for short wait loops.
    class SpinLatch {
    private:
        std::atomic<bool> ready{false};
    public:
        void Wait() {
            while (!ready.load(std::memory_order_acquire)) {
                std::this_thread::yield(); // Let scheduler threads run
            }
        }

        void Signal() {
            ready.store(true, std::memory_order_release);
        }

        void Reset(){
            ready.store(false, std::memory_order_release);
        }

        bool IsReady() const {
            return ready.load(std::memory_order_acquire);
        }            
    };

    /// @brief A latch with a mutex/cv.
    class Latch {
    private:
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> ready = false;

    public:
        void Wait() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] { return ready.load(std::memory_order_acquire); });
        }

        void Signal() {
            std::lock_guard<std::mutex> lock(mtx);
            ready = true;
            cv.notify_all();
        }

        void Reset() {
            std::lock_guard<std::mutex> lock(mtx);
            ready.store(false, std::memory_order_release);
        }

        bool IsReady() const {
            return ready.load(std::memory_order_acquire);
        }
    };
} // namespace StreamLine::Locks
