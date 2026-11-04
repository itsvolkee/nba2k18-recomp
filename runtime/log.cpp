#include "log.h"
#include <chrono>
#include <thread>
#include <functional>

namespace logging
{
    static std::mutex s_lock;
    static FILE* s_file = nullptr;
    static const auto s_start = std::chrono::steady_clock::now();

    void OpenFile(const char* path)
    {
        std::lock_guard lock(s_lock);
        s_file = fopen(path, "w");
    }

    void Write(const char* level, const std::string& msg)
    {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 10000;
        std::lock_guard lock(s_lock);
        fprintf(stderr, "[%9.3f %04zu %s] %s\n", t, tid, level, msg.c_str());
        if (s_file)
        {
            fprintf(s_file, "[%9.3f %04zu %s] %s\n", t, tid, level, msg.c_str());
            fflush(s_file);
        }
    }
}
