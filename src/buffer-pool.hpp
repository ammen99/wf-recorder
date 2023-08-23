#pragma once

#include <array>
#include <mutex>
#include <atomic>
#include <type_traits>

class buffer_pool_buf
{
public:
    bool ready_capture() const
    {
        return released;
    }

    bool ready_encode() const
    {
        return available;
    }

    std::atomic<bool> released{true}; // if the buffer can be used to store new pending frames
    std::atomic<bool> available{false}; // if the buffer can be used to feed the encoder
};

template <class T, int N>
class buffer_pool
{
public:
    static_assert(std::is_base_of<buffer_pool_buf, T>::value, "T must be subclass of buffer_pool_buf");

    buffer_pool()
    {
        for (size_t i = 0; i < bufs_size; ++i) {
            bufs[i] = new T;
        }
    }

    ~buffer_pool()
    {
        for (size_t i = 0; i < N; ++i) {
            delete bufs[i];
        }
    }

    size_t size() const
    {
        return N;
    }

    const T* at(size_t i) const
    {
        return bufs[i];
    }

    T& capture()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return *bufs[capture_idx];
    }

    T& encode()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return *bufs[encode_idx];
    }

    // Signal that the current capture buffer has been successfully obtained
    // from the compositor and select the next buffer to capture in.
    T& next_capture()
    {
        std::lock_guard<std::mutex> lock(mutex);
        bufs[capture_idx]->released = false;
        bufs[capture_idx]->available = true;
        size_t next = (capture_idx + 1) % bufs_size;
        if (!bufs[next]->ready_capture() && bufs_size < N) {
            bufs_size++;
            next = (capture_idx + 1) % bufs_size;
            for (size_t i = N - 1; i > next; --i) {
                bufs[i] = bufs[i - 1];
                if (encode_idx == i - 1) {
                    encode_idx = i;
                }
            }
            bufs[next] = new T;
        }
        capture_idx = next;
        return *bufs[capture_idx];
    }

    // Signal that the encode buffer has been submitted for encoding
    // and select the next buffer for encoding.
    T& next_encode()
    {
        std::lock_guard<std::mutex> lock(mutex);
        bufs[encode_idx]->available = false;
        bufs[encode_idx]->released = true;
        encode_idx = (encode_idx + 1) % bufs_size;
        return *bufs[encode_idx];
    }

private:
    std::mutex mutex;
    std::array<T*, N> bufs;
    size_t bufs_size = 2;
    size_t capture_idx = 0;
    size_t encode_idx = 0;
};
