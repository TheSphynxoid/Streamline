#pragma once

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <csignal>
#else
#error "Unsupported platform for debug break"
#endif

inline void sl_dbg_breakpoint()
{
#ifdef _DEBUG || DEBUG
#if defined(_MSC_VER) // MSVC, Windows
    __debugbreak();
#elif defined(__GNUC__) || defined(__clang__) // GCC/Clang on Unix/macOS
#if defined(SIGTRAP)
    raise(SIGTRAP);
#else
    __builtin_trap();
#endif
#else
#error "Unsupported compiler for debug breakpoint"
#endif
#endif //_DEBUG
}
