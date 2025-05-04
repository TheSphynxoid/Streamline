#pragma once
#include <iostream>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>

namespace StreamLine{
    class ThreadOwnershipException : public std::exception {
    public:
        const char* what() const noexcept override {
            return "Wait can only be called by the owner thread";
        }
    };
    class WaitGroupUseAfterWait : public std::exception {
    public:
        const char* what() const noexcept override {
            return "Cannot add work after waiting has begun";
        }
    };

    class CongregatedException : public std::exception {
    private:
        std::vector<std::exception_ptr> exceptions = std::vector<std::exception_ptr>();
        mutable std::string message_cache = "";
    public:
        CongregatedException() noexcept = default;

        explicit CongregatedException(std::vector<std::exception_ptr> exps) noexcept
            : exceptions(std::move(exps))
        {
            message_cache = "Multiple exceptions occurred (count: " + 
                        std::to_string(exceptions.size()) + ")";
        }

        const std::vector<std::exception_ptr>& getExceptions() const noexcept {
            return exceptions;
        }

        const char* what() const noexcept override {
            return message_cache.c_str();
        }
    };

    //TODO: add a templated lock. template<typename Lock = std::mutex>
    

    class WaitGroup {
        private:

            mutable std::atomic<unsigned int> count{ 0 };            // Counter for tracking the number of tasks
			std::thread::id owner = std::this_thread::get_id(); // ID of the thread that created the WaitGroup
            mutable std::mutex mtx;                         // Mutex for coordinating condition variable
            mutable std::condition_variable cv;             // Condition variable for wait signaling
            std::atomic<bool> waiting{ false };     // Flag to prevent multiple waits
            mutable std::vector<std::exception_ptr> collected;

            //For reset.
            int def_count = 0;

            /**
             * @brief Construct a new Wait Group object, This is needed for Move() to work
             */
            WaitGroup(WaitGroup&& wg) noexcept{
                owner = std::this_thread::get_id();
                count.store(wg.count.load(std::memory_order_acquire), 
                std::memory_order_release);
                collected = std::move(wg.collected);
            };

            /**
             * @brief This operator is here because the move constructor is manually defined.
             */
            WaitGroup& operator=(WaitGroup&& wg) noexcept{
                owner = std::this_thread::get_id();
                count.store(wg.count.load(std::memory_order_acquire), 
                std::memory_order_release);
                collected = std::move(wg.collected);
                return *this;
            };
            
        public:
            WaitGroup() = default;
            ~WaitGroup() = default;
            WaitGroup(const WaitGroup&) = delete;
            WaitGroup& operator=(const WaitGroup&) = delete;
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

                def_count = count.load(std::memory_order_relaxed);
            }
    
            /**
             * @brief Decrement the counter and notify waiting threads if it reaches zero, Called by working threads.
             * 
             * @note This function must under no circumstance fail, as long as the WaitGroup is valid (exists).
             * 
             * @param ex if ex is not NULL, the waitgroup is considered not successful. 
             */
            void Done(std::exception_ptr ex = nullptr)const noexcept {
                if(std::this_thread::get_id() == owner){
#ifdef DEBUG
                    //Reaching this is a API design violation, but yeah, safeguards. safeguards.
                    std::cout << "Done is called by the owner, This is not the correct usage of WaitGroup\n";
#endif
                    return;
                }
                if(ex){
                    std::unique_lock<std::mutex> lock(mtx);
                    collected.push_back(ex);
                }
                if (count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    cv.notify_all();  // Notify waiting threads
                }
            }

            /**
             * @brief Explicitly move a non-waiting WaitGroup to a new owner thread
             * 
             * @note This implementation relies on Return Value Optimization (RVO) to
             * avoid invoking the private move constructor when returning the new WaitGroup.
             * Modern C++ compilers should perform this optimization automatically.
             * 
             * @param original The original WaitGroup to move from
             * @return WaitGroup A new WaitGroup with transferred state
             * @throws WaitGroupUseAfterWait if the original WaitGroup is already waiting
             */
            static WaitGroup Transfer(WaitGroup& original) {
                // Can't move a WaitGroup that's already waiting
                if (original.waiting.load(std::memory_order_acquire)) {
                    throw WaitGroupUseAfterWait();
                }
                
                WaitGroup new_wg;
                
                // Transfer ownership to current thread
                new_wg.owner = std::this_thread::get_id();
                
                std::unique_lock<std::mutex> lock(original.mtx);

                // Transfer count
                new_wg.count.store(original.count.load(std::memory_order_acquire), 
                                std::memory_order_release);
                original.count.store(0, std::memory_order_release);
                
                // Transfer success state
                new_wg.collected = std::move(original.collected);


                new_wg.def_count = original.def_count;
                original.def_count = 0;
                
                // Reset original state
                original.collected.clear();
                //Rely on RVO (or NRVO)
                return new_wg;
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

                if(!collected.empty()) {
                    throw CongregatedException(std::move(collected));
                }
            }

            [[nodiscard]]
            bool WaitFor(std::chrono::milliseconds timeout) {
                if (std::this_thread::get_id() != owner) {
                    throw ThreadOwnershipException();
                }
            
                bool expected = false;
                if (!waiting.compare_exchange_strong(expected, true)) {
                    throw std::runtime_error("WaitGroup instance is one-use only.");
                }
            
                std::unique_lock<std::mutex> lock(mtx);
                bool success = cv.wait_for(lock, timeout, [&] {
                    return count.load(std::memory_order_acquire) == 0;
                });
            
                if(success && !collected.empty()) {
                    throw CongregatedException(std::move(collected));
                }
                // You could optionally set `waiting = false` here to allow reuse in case of timeout, 
                // but that would deviate from your current one-shot policy.
            
                return success;
            }

            void Reset(){
                if(owner != std::this_thread::get_id()){
                    throw ThreadOwnershipException();
                }

                std::unique_lock<std::mutex> lock(mtx);

                if(count.load(std::memory_order_acquire) != 0 && waiting.load(std::memory_order_acquire) == true){
                    throw std::runtime_error("Cannot reset WaitGroup that hasn't finished waiting.");
                }
                count.store(def_count, std::memory_order_release);
                waiting.store(false, std::memory_order_release);
                collected.clear();
            }

            [[nodiscard]]
            bool HasFinishedSuccessfully()const noexcept{
                return !collected.size();
            }

            inline const std::vector<std::exception_ptr>& GetExceptions()const noexcept{
                return collected;
            }

            /**
             * @brief Get the Count object, Use only for information before waiting.
             */
            inline const unsigned int GetCount()const noexcept{
                return count.load(std::memory_order_relaxed);
            }
        };
    
}