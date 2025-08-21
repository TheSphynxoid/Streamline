#pragma once
#include "ThreadPool.h"
#include "WaitGroup.h"
#include "Task.h"
#include <map>

namespace StreamLine{
    /**
     * @brief Contains needed details to configure the bootstrap to users needs.
     * 
     */
    struct InstanceConfiguration{
        bool InitThreadPool = false;
        unsigned int ThreadCount = std::thread::hardware_concurrency();

    };

    /// @brief This is not an optimized structure (Uses mutex), it is used for debugging.
    class ThreadName_DbgExt{
    private:
        inline static bool Initalized = false;
        inline static std::thread::id MainThreadID = std::this_thread::get_id();
        inline static std::unordered_map<std::thread::id, std::string> NameMap;
        inline static std::mutex mtx;
    public:
        static void Init() {
            if(Initalized)return;
            MainThreadID = std::this_thread::get_id();
            //Assume that the thread count are atleast equal to the hardware limit.
            NameMap.reserve(std::thread::hardware_concurrency());
            Initalized = true;
        }

        static inline const std::thread::id GetMainThreadID() noexcept{ return MainThreadID;}

        static const std::string& GetThreadName(std::thread::id id) noexcept{
            std::unique_lock ul(mtx);
            static const std::string UnknownThread = "Unknown Thread";
            auto it = NameMap.find(id);
            return (it != NameMap.end()) ? it->second : UnknownThread;
        }

        static void AddOrReplaceThreadName(std::thread::id id, const std::string& name){
            std::unique_lock ul(mtx);
            std::cout << "Thread added: " << name << " (ID: " << id << ")" << std::endl;
            NameMap[id] = name;
        }
    };
    /**
     * @brief This class sets up the StreamLine automatically instead of full manual control.
     * 
     */
    class Bootstrap{
    public:
        static void Initialize(const InstanceConfiguration& config = InstanceConfiguration()){
            if(config.InitThreadPool){
                ThreadPool::InitalizePool(config.ThreadCount);
#ifdef _DEBUG
                ThreadName_DbgExt::Init();
#endif
            }
        }
    };
}