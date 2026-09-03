#pragma once

#include "types.hpp"
#include "queue.hpp"
#include <functional>

namespace f4w {

class ThreadPool {
public:
    explicit ThreadPool(int num_threads);
    ~ThreadPool();

    void submit(std::function<void()> task);
    void shutdown();

private:
    HANDLE                          iocp_;
    std::vector<HANDLE>             threads_;
    std::atomic<bool>               stopping_{false};

    static DWORD WINAPI worker_proc(LPVOID param);
};

} // namespace f4w
