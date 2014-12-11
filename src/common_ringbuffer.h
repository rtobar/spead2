/**
 * @file
 *
 * Definition of @ref spead::ringbuffer.
 */

#ifndef SPEAD_COMMON_RINGBUFFER_H
#define SPEAD_COMMON_RINGBUFFER_H

#include <mutex>
#include <condition_variable>
#include <type_traits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <cassert>

namespace spead
{

/// Thrown when attempting to do a non-blocking push to a full ringbuffer
class ringbuffer_full : public std::runtime_error
{
public:
    ringbuffer_full() : std::runtime_error("ring buffer is full") {}
};

/// Thrown when attempting to do a non-blocking pop from a full ringbuffer
class ringbuffer_empty : public std::runtime_error
{
public:
    ringbuffer_empty() : std::runtime_error("ring buffer is empty") {}
};

/// Thrown when attempting to do a pop from an empty ringbuffer that has been shut down
class ringbuffer_stopped : public std::runtime_error
{
public:
    ringbuffer_stopped() : std::runtime_error("ring buffer has been shut down") {}
};

/**
 * Thread-safe ring buffer with blocking and non-blocking pop, but only
 * non-blocking push. It supports non-copyable objects using move semantics.
 *
 * The producer may signal that it has finished producing data by calling
 * @ref stop, which will gracefully shut down the consumer.
 */
template<typename T>
class ringbuffer
{
private:
    typedef typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_type;

    std::unique_ptr<storage_type[]> storage;
    const std::size_t cap;  ///< Number of slots
    std::size_t head = 0;   ///< First slot with data
    std::size_t tail = 0;   ///< First free slot
    std::size_t len = 0;    ///< Number of slots with data
    bool stopped = false;

    /// Gets pointer to the slot number @a idx
    T *get(std::size_t idx);

    /// Increments @a idx, wrapping around.
    std::size_t next(std::size_t idx)
    {
        idx++;
        if (idx == cap)
            idx = 0;
        return idx;
    }

protected:
    // This is all protected rather than private to allow alternative
    // blocking strategies

    /// Protects access to the internal fields
    std::mutex mutex;
    /// Signalled when data is added or @a stop is called
    std::condition_variable data_cond;

    /**
     * Checks whether the ringbuffer is empty.
     *
     * @throw ringbuffer_stopped if the ringbuffer is empty and has been stopped.
     * @pre The caller holds @ref mutex
     */
    bool empty_unlocked() const
    {
        if (len == 0 && stopped)
            throw ringbuffer_stopped();
        return len == 0;
    }

    /**
     * Pops an item from the ringbuffer and returns it.
     *
     * @pre The caller holds @ref mutex, and there is data available.
     */
    T pop_unlocked();

public:
    /**
     * Constructs an empty ringbuffer.
     *
     * @param cap      Maximum capacity, in items
     */
    explicit ringbuffer(std::size_t cap);
    ~ringbuffer();

    /**
     * Append an item to the queue, if there is space. It uses move
     * semantics, so on success, the original value is undefined.
     *
     * @param value    Value to move
     * @throw ringbuffer_full if there is no space
     * @throw ringbuffer_stopped if @ref stop has already been called
     */
    void try_push(T &&value);

    /**
     * Construct a new item in the queue, if there is space.
     *
     * @param args     Arguments to the constructor
     * @throw ringbuffer_full if there is no space
     * @throw ringbuffer_stopped if @ref stop has already been called
     */
    template<typename... Args>
    void try_emplace(Args&&... args);

    /**
     * Retrieve an item from the queue, if there is one.
     *
     * @throw ringbuffer_stopped if the queue is empty and @ref stop was called
     * @throw ringbuffer_empty if the queue is empty but still active
     */
    T try_pop();

    /**
     * Retrieve an item from the queue, blocking until there is one or until
     * the queue is stopped.
     *
     * @throw ringbuffer_stopped if the queue is empty and @ref stop was called
     */
    T pop();

    /**
     * Indicate that no more items will be produced. This does not immediately
     * stop consumers if there are still items in the queue; instead,
     * consumers will continue to retrieve remaining items, and will only be
     * signalled once the queue has drained.
     *
     * It is safe to call this function multiple times.
     */
    void stop();
};

template<typename T>
ringbuffer<T>::ringbuffer(std::size_t cap)
    : storage(new storage_type[cap]), cap(cap)
{
    assert(cap > 0);
}

template<typename T>
ringbuffer<T>::~ringbuffer()
{
    // Drain any remaining elements
    while (len > 0)
    {
        get(head)->~T();
        head = next(head);
        len--;
    }
}

template<typename T>
T *ringbuffer<T>::get(std::size_t idx)
{
    return reinterpret_cast<T*>(&storage[idx]);
}

template<typename T>
void ringbuffer<T>::try_push(T &&value)
{
    // Construct in-place with move constructor
    try_emplace(std::move(value));
}

template<typename T>
template<typename... Args>
void ringbuffer<T>::try_emplace(Args&&... args)
{
    std::unique_lock<std::mutex> lock(mutex);
    if (stopped)
        throw ringbuffer_stopped();
    if (len == cap)
        throw ringbuffer_full();
    // Construct in-place
    new (get(tail)) T(std::forward<Args>(args)...);
    // Advance the queue
    tail = next(tail);
    len++;
    lock.unlock();
    // Unlocking before notify avoids the woken thread from immediately
    // blocking again to obtain the mutex.
    data_cond.notify_one();
}

template<typename T>
T ringbuffer<T>::pop_unlocked()
{
    T result = std::move(*get(head));
    head = next(head);
    len--;
    return result;
}

template<typename T>
T ringbuffer<T>::try_pop()
{
    std::unique_lock<std::mutex> lock(mutex);
    if (empty_unlocked())
    {
        throw ringbuffer_empty();
    }
    return pop_unlocked();
}

template<typename T>
T ringbuffer<T>::pop()
{
    std::unique_lock<std::mutex> lock(mutex);
    while (empty_unlocked())
    {
        data_cond.wait(lock);
    }
    return pop_unlocked();
}

template<typename T>
void ringbuffer<T>::stop()
{
    std::unique_lock<std::mutex> lock(mutex);
    stopped = true;
    data_cond.notify_all();
}

} // namespace spead

#endif // SPEAD_COMMON_RINGBUFFER_H