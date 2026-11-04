#pragma once
#include <cstdio>
#include <format>
#include <mutex>
#include <string>

namespace logging
{
    void Write(const char* level, const std::string& msg);
    void OpenFile(const char* path);
}

#define LOG_INFO(...)  ::logging::Write("info ", std::format(__VA_ARGS__))
#define LOG_WARN(...)  ::logging::Write("warn ", std::format(__VA_ARGS__))
#define LOG_ERROR(...) ::logging::Write("ERROR", std::format(__VA_ARGS__))
#define LOG_TRACE(...) ::logging::Write("trace", std::format(__VA_ARGS__))
