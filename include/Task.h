#pragma once
#include "WaitGroup.h"
#include "Callable.h"
#include "Awaitable.h"
#include "TaskScheduler.h"
#include <functional>
#include <future>

namespace StreamLine{
    //TODO: The lambda create (and later passed to the scheduler) uses a std::promise in a task instance,
    //This can be a safety issues, if the task is destroyed before the task exection (or mid-execution).
    //Either find a way to make the task always outlive it's execution (since halting it can be a race condition).
    //Or make the promise not related to an instance of a task. (Avoid make_shared, since heap allocation of each task can get slow).
    template<class T>
    class Task : public Awaitable{
        private:
        std::function<T()> task;
        std::promise<T> result;
        WaitGroup* wg = nullptr;
        Ticket ticket = 0;
    public:
        Task(std::function<T()> f, WaitGroup* waitgroup){
            wg = waitgroup;
            task = [f = std::move(f), wg = waitgroup, result = &this->result]() -> void{
                try{
                    T rt_val = f();
                    wg->Done();
                    result.set_value(rt_val);
                }catch(const std::exception& e){
                    result.set_exception(std::current_exception());
                }
            };
        }
        ~Task(){
            //Task has been added in scheduler
            if(ticket != 0){
                if(TaskScheduler::GetTaskState(ticket) == TaskState::Executing){
                    TaskScheduler::WaitForTask(ticket);
                }
                else {
                    TaskScheduler::CancelTask(ticket);
                }
            }
        }
        
        void Execute(){
            ticket = TaskScheduler::AddTask(task);
        }

        std::future<T> GetFuture() {
            return result.get_future();
        }

        void Abandon(){
            wg->Done();
            result.set_exception(std::make_exception_ptr(std::future_error(std::future_errc::broken_promise)));
            TaskState state = TaskScheduler::CancelTask(ticket);
            if(state == TaskState::Executing){
                //Find a way to discard the result or inform user
            }else if(state == TaskState::Failed){
                //The task started executing and failed.
            }else if(state == TaskState::Complete){
                //Task is already complete
            }else if(state == TaskState::Abandonned){
                //Since task is abandonned, WaitGroup.Done will never be called.
                //That is handled here.
                wg.Done();
            }
        }

    };
}