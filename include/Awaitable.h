#pragma once

namespace StreamLine
{
    class Awaitable{
    public:
        virtual void Wait(size_t timeout) = 0;
        
    };
} // namespace StreamLine
