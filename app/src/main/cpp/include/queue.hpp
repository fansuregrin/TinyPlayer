#ifndef TINY_PLAYER_QUEUE_HPP
#define TINY_PLAYER_QUEUE_HPP

#include <deque>
#include <condition_variable>
#include <mutex>

template <typename T>
class Queue {
private:
    using lock_guard = std::lock_guard<std::mutex>;
    using unique_lock = std::unique_lock<std::mutex>;
public:
    using size_type = typename std::deque<T>::size_type;
    using value_type = T;
    using reference = value_type &;
    using const_reference = const value_type &;

    explicit Queue(size_type cap = 256);
    ~Queue();

    value_type front();

    bool empty() noexcept;
    bool full() noexcept;
    size_type size() noexcept;
    size_type capacity() noexcept;

    void clear() noexcept;
    void push(const T &ele);
    bool pop(T &ele);

    /**
     * @brief 暂停队列，既不能向中添加元素也不能从队列中获取元素
     */
    void pause();

    /**
     * @brief 恢复队列，让队列恢复到可以添加和获取元素的状态
     */
    void resume();
    void close();
private:
    std::deque<T> deq;
    std::mutex mtx;
    size_type m_cap;  // 队列的容量
    std::condition_variable producer;
    std::condition_variable consumer;
    bool is_close;  // 队列是否处于关闭状态
    bool is_pause;  // 队列是否处于暂停状态
};

template <typename T>
Queue<T>::Queue(size_type cap) : m_cap(cap), is_pause(false), is_close(false) {}

template <typename T>
Queue<T>::~Queue() {
    close();
}

template <typename T>
typename Queue<T>::value_type Queue<T>::front() {
    lock_guard lck(mtx);
    return deq.front();
}

template <typename T>
bool Queue<T>::empty() noexcept {
    lock_guard lck(mtx);
    return deq.empty();
}

template <typename T>
bool Queue<T>::full() noexcept {
    lock_guard lck(mtx);
    return deq.size() >= m_cap;
}

template <typename T>
typename Queue<T>::size_type
Queue<T>::size() noexcept {
    lock_guard lck(mtx);
    return deq.size();
}

template <typename T>
typename Queue<T>::size_type
Queue<T>::capacity() noexcept {
    lock_guard lck(mtx);
    return m_cap;
}

template <typename T>
void Queue<T>::clear() noexcept {
    lock_guard lck(mtx);
    deq.clear();
}

template <typename T>
void Queue<T>::push(const T &ele) {
    unique_lock lck(mtx);
    while (deq.size() >= m_cap || is_pause) {
        producer.wait(lck);
    }
    deq.push_back(ele);
    consumer.notify_one();
}

template <typename T>
bool Queue<T>::pop(T &ele) {
    unique_lock lck(mtx);
    while (deq.empty() || is_pause) {
        consumer.wait(lck);
        if (is_close) {
            return false;
        }
    }
    ele = deq.front();
    deq.pop_front();
    producer.notify_one();
    return true;
}

template <typename T>
void Queue<T>::pause() {
    {
        lock_guard lck(mtx);
        is_pause = true;
    }
    consumer.notify_all();
    producer.notify_all();
}

template <typename T>
void Queue<T>::resume() {
    {
        lock_guard lck(mtx);
        is_pause = false;
    }
    consumer.notify_all();
    producer.notify_all();
}

template <typename T>
void Queue<T>::close() {
    {
        lock_guard lck(mtx);
        deq.clear();
        is_close = true;
    }
    producer.notify_all();
    consumer.notify_all();
}

#endif //TINY_PLAYER_QUEUE_HPP
