#pragma once
#include "ThreadPool.h"
#include "WaitGroup.h"
#include "Task.h"

namespace StreamLine{
    /**
     * @brief Contains needed details to configure the bootstrap to users needs.
     * 
     */
    struct InstanceConfiguration{
        bool InitThreadPool = false;
        unsigned int ThreadCount = std::thread::hardware_concurrency();
    };
    /**
     * @brief This class sets up the StreamLine automatically instead of full manual control.
     * 
     */
    class Bootstrap{
    public:
        static void Initialize(InstanceConfiguration& config = InstanceConfiguration()){
            if(config.InitThreadPool){
                ThreadPool::InitalizePool(config.ThreadCount);
            }
        }
    };
}