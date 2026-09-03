#include "find4w/thread_pool.hpp"

namespace f4w {

struct TaskWrapper {
    std::function<void()> func;
};

ThreadPool::ThreadPool(int num_threads) {
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, num_threads);
    threads_.resize(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads_[i] = CreateThread(nullptr, 0, worker_proc, this, 0, nullptr);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::submit(std::function<void()> task) {
    auto* wrapper = new TaskWrapper{std::move(task)};
    PostQueuedCompletionStatus(iocp_, 0, reinterpret_cast<ULONG_PTR>(wrapper), nullptr);
}

void ThreadPool::shutdown() {
    if (stopping_.exchange(true)) return;

    for (size_t i = 0; i < threads_.size(); ++i)
        PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);

    WaitForMultipleObjects((DWORD)threads_.size(), threads_.data(), TRUE, INFINITE);

    for (auto h : threads_)
        CloseHandle(h);
    threads_.clear();

    CloseHandle(iocp_);
}

DWORD WINAPI ThreadPool::worker_proc(LPVOID param) {
    auto* pool = static_cast<ThreadPool*>(param);
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED ov;

    while (GetQueuedCompletionStatus(pool->iocp_, &bytes, &key, &ov, INFINITE)) {
        if (key == 0) break;
        auto* wrapper = reinterpret_cast<TaskWrapper*>(key);
        wrapper->func();
        delete wrapper;
    }
    return 0;
}

} // namespace f4w
