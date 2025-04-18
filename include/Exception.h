#pragma once
#include <exception>

class InvalidOperation : public std::exception{
public:
    InvalidOperation() noexcept = default;
    
    const char* what() const noexcept override {
        return "Invalid operation";
    }
};