#pragma once
#include <iostream>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#if WARNING_AS_ERROR
#define warning_noexcept
#else
#define warning_noexcept noexcept
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
    //TODO: add a templated lock. template<typename Lock = std::mutex>
    class WaitGroup {
        private:
            std::atomic<int> count{ 0 };            // Counter for tracking the number of tasks
			const std::thread::id owner = std::this_thread::get_id(); // ID of the thread that created the WaitGroup
            std::mutex mtx;                         // Mutex for coordinating condition variable
            std::condition_variable cv;             // Condition variable for wait signaling
            std::atomic<bool> waiting{ false };     // Flag to prevent multiple waits
            bool Successful = true;                  // All code with either set this to false or not do anything. no need for atomic
        public:
            WaitGroup() = default;
    
            // Increment the counter, No other thread besides the creating thread can Add.
            // If called after Waiting has started WaitGroupUseAfterWait exception will be thrown.
            void Add(int n) {
                if(std::this_thread::get_id() != owner){
                    throw ThreadOwnershipException();
                }
                if(waiting){
                    throw WaitGroupUseAfterWait(); //TODO: i can log instead, as this is a trival error, but i don't have a logger now.
                }
                count.fetch_add(n, std::memory_order_relaxed);
            }
    
            // Decrement the counter and notify waiting threads if it reaches zero
            // This function must under no circumstance fail, as long as the WaitGroup is valid (exists).
            void Done(std::exception_ptr ex = nullptr)noexcept {
                if(ex){
                    Successful = false;
                }
                if (count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lock(mtx);
                    
                    cv.notify_all();  // Notify waiting threads
                }
            }
    
            // Wait for the counter to reach zero, this will never thrown on the main thread.
            void Wait() {
                if(std::this_thread::get_id() != owner){
                    //Only owner thread can wait (to avoid deadlocks).
                    throw ThreadOwnershipException();
                }
                bool expected = false;
                if (!waiting.compare_exchange_strong(expected, true)) {
                    //This is a recoverable error, a reused WaitGroup can simply return, albeit it can be misleading without an explicit error.
                    throw std::runtime_error("WaitGroup instance is one-use only.");
                    //TODO: I'm planning an error management system forcing the user to add a call back, this will be handled there.
                    //return;
                }
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return count.load(std::memory_order_acquire) == 0; });
                // Reset the waiting flag after the wait is done, out of precaution.
    
                //waiting.store(false, std::memory_order_release);
            }

            bool HasFinishedSuccessfully()const noexcept{
                return Successful;
            }
        };
    
}