#pragma once
#include <iostream>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(__GNUC__) || defined(__clang__)
// GCC/Clang specific
#include <x86intrin.h>   // or <arm_neon.h> depending on architecture
#elif defined(_MSC_VER)
// MSVC specific
#include <intrin.h>
#endif

namespace StreamLine{
    class ThreadOwnershipException : public std::exception {
    public:
        const char* what() const noexcept override {
            return "Wait can only be called by the owner thread";
        }
    };
    class WaitGroupUseAfterWait : public std::exception{
    public:
        const char* what() const noexcept override {
            return "Cannot add work after waiting has begun";
        }
    };
    class WaitGroup {
        private:
            std::atomic<int> count{ 0 };            // Counter for tracking the number of tasks
            std::mutex mtx;                         // Mutex for coordinating condition variable
            std::condition_variable cv;             // Condition variable for wait signaling
            std::atomic<bool> waiting{ false };     // Flag to prevent multiple waits
            const std::thread::id owner = std::this_thread::get_id();
        public:
            WaitGroup() = default;
    
            // Increment the counter, No other thread besides the creating thread can Add.
            // If called after Waiting has started WaitGroupUseAfterWait exception will be thrown
            void Add(int n) {
                if(std::this_thread::get_id() != owner){
                    throw ThreadOwnershipException();
                }
                if(waiting){
                    throw WaitGroupUseAfterWait(); 
                }
                count.fetch_add(n, std::memory_order_relaxed);
            }
    
            // Decrement the counter and notify waiting threads if it reaches zero
            void Done() {
                if (count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lock(mtx);
                    
                    cv.notify_all();  // Notify waiting threads
                }
            }
    
            // Wait for the counter to reach zero, usually called by the main thread
            void Wait() {
                if(std::this_thread::get_id() != owner){
                    //Only owner thread can wait (to avoid deadlocks).
                    throw ThreadOwnershipException();
                }
                bool expected = false;
                if (!waiting.compare_exchange_strong(expected, true)) {
                    throw std::runtime_error("WaitGroup instance is one-use only.");
                }
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return count.load(std::memory_order_acquire) == 0; });
    
                // Reset the waiting flag after the wait is done, out of precaution.
                waiting.store(false, std::memory_order_release);
            }
        };
    
}