#pragma once
#include <iostream>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#if defined(__GNUC__) || defined(__clang__)
// GCC/Clang specific
#include <x86intrin.h>   // or <arm_neon.h> depending on architecture
#elif defined(_MSC_VER)
// MSVC specific
#include <intrin.h>
#endif

namespace StreamLine{
        class ThreadPool final {
        private:
            /**
            * @brief The number of threads in the pool.
            */
            static inline unsigned int ThreadCount;
            /**
            * @brief The threads in the pool.
            */
            static inline std::vector<std::thread> Threads;
        public:
            static void InitalizePool(unsigned int threadCount =  std::thread::hardware_concurrency() - 1) {
                ThreadCount = std::min(threadCount, std::thread::hardware_concurrency() - 1);
                std::cout << ThreadCount <<" Threads Allocated";
                for (unsigned int i = 0; i < ThreadCount; i++) {
                    Threads.emplace_back([]() {
                        while (true) {
                            // Wait for a task to be assigned
                            // Execute the task
                        }
                        });
                }
            }
        }; 
}