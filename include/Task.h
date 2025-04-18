#pragma once
#include "WaitGroup.h"
#include "Callable.h"
#include <functional>
#include <future>

namespace StreamLine{
    template<class T>
    class Task{
        private:
        std::function<T()> task;
        std::promise<T> result;
        WaitGroup* wg = nullptr;
    public:
        Task(std::function<T()> f, WaitGroup* wg){
            task = [wg](){
                f();
                wg->Done();
            };
        }

        std::promise<T> GetPromise(){
            return result;
        }

        void Abandon(){
            wg->Done();
            result.set_exception(std::future_error());

        }

    };
}