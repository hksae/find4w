#pragma once

#include <atomic>
#include <vector>
#include <optional>
#include <cstdint>
#include <immintrin.h>

namespace f4w {

template <typename T>
class ConcurrentQueue {
public:
    explicit ConcurrentQueue(size_t capacity = 4096)
        : capacity_(next_power_of_2(capacity))
        , mask_(capacity_ - 1)
        , buffer_(capacity_)
        , head_(0)
        , tail_(0)
    {
        for (size_t i = 0; i < capacity_; ++i)
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool try_push(const T& item) {
        Cell* cell;
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& item) {
        Cell* cell;
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(item);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> try_pop() {
        Cell* cell;
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return std::nullopt;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
        T result = std::move(cell->data);
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return result;
    }

    void push(T&& item, const std::atomic<bool>& done) {
        while (!try_push(std::move(item))) {
            if (done.load(std::memory_order_relaxed)) return;
            _mm_pause();
        }
    }

    void push(const T& item, const std::atomic<bool>& done) {
        while (!try_push(item)) {
            if (done.load(std::memory_order_relaxed)) return;
            _mm_pause();
        }
    }

    std::optional<T> pop(const std::atomic<bool>& done) {
        std::optional<T> result;
        while (!(result = try_pop())) {
            if (done.load(std::memory_order_relaxed))
                return try_pop();
            _mm_pause();
        }
        return result;
    }

    bool empty() const {
        return head_.load(std::memory_order_relaxed) >= tail_.load(std::memory_order_relaxed);
    }

    size_t size_approx() const {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        return t > h ? (t - h) : 0;
    }

private:
    static size_t next_power_of_2(size_t n) {
        n--; n |= n >> 1; n |= n >> 2; n |= n >> 4;
        n |= n >> 8; n |= n >> 16; n |= n >> 32;
        return n + 1;
    }

    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    size_t            capacity_;
    size_t            mask_;
    std::vector<Cell> buffer_;

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

} // namespace f4w
