#pragma once

#include <array>
#include <mutex>
#include <atomic>
#include <functional>
#include <type_traits>

#define MAX_FRAME_FAILURES 64
#define INITIAL_BUFFERS_SIZE 2

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
        for (size_t i = 0; i < bufs_size; ++i) {
            delete bufs[i];
        }
    }

    size_t size() const
    {
        return bufs_size;
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
        int next = (capture_idx + 1) % bufs_size;
        if (!bufs[next]->ready_capture())
        {
            bufs_size++;
            if (bufs_size > MAX_FRAME_FAILURES)
            {
                std::cerr << "Too many buffers! (" << bufs_size << " > " << MAX_FRAME_FAILURES << ")" << std::endl;
                exit(EXIT_FAILURE);
            }
            std::cerr << "bufs_size: " << bufs_size << std::endl;
            bufs[bufs_size - 1] = new T;
            next = (capture_idx + 1) % bufs_size;
        }
        bufs[capture_idx]->released = false;
        bufs[capture_idx]->available = true;
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
    std::array<T*, MAX_FRAME_FAILURES> bufs;
    size_t bufs_size = INITIAL_BUFFERS_SIZE;
    int capture_idx = 0; // head
    int encode_idx = 0; // tail
};
