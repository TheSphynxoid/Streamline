#pragma once
#include <functional>
#include <thread>

namespace StreamLine
{
    typedef size_t Ticket;
    enum class TaskState : unsigned int{
        Waiting, Executing, Complete, Abandonned, Failed
    };
    struct TaskPackage{
        std::thread::id executingThread;
        TaskState state;
        std::exception_ptr exception = nullptr; 
    };
    class TaskScheduler{
        static const Ticket NullTicket = 0;
    public:
        static const Ticket& AddTask(std::function<void()> f){
            return NullTicket;
        }
        static const TaskState GetTaskState(const Ticket& ticket) noexcept{
            return TaskState::Failed;
        }
        static void WaitForTask(const Ticket& ticket)noexcept{

        }
        static TaskState CancelTask(const Ticket& ticket){
            return TaskState::Failed;
        }
    };
} // namespace StreamLine::Internal
