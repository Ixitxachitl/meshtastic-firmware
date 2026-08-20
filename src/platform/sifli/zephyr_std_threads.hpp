#pragma once

// std::mutex, std::condition_variable and std::thread over Zephyr primitives.
//
// The Zephyr SDK's libstdc++ for arm-zephyr-eabi is built without gthreads
// (_GLIBCXX_HAS_GTHREADS is undefined in its c++config.h), so those three
// types are absent from <mutex>, <condition_variable> and <thread> no matter
// what CONFIG_POSIX_THREADS says. std::lock_guard and std::unique_lock are
// defined outside that guard, so they work as soon as a Lockable exists.
//
// Adding names to namespace std is not something the standard allows, but the
// alternative is patching every library that expects a hosted C++ threading
// implementation. This covers only what device-ui and tftSetup.cpp use; it is
// not a general implementation - no timed locks, no recursive mutexes, and
// thread has no exception behaviour to speak of, since Zephyr builds here have
// exceptions disabled.

#include <chrono>
#include <functional>
#include <mutex> // std::lock_guard and std::unique_lock survive without gthreads
#include <utility>
#include <zephyr/kernel.h>

// libstdc++ still declares a partial std::thread and this_thread without
// gthreads - enough to clash with the definitions below, not enough to use.
// Claiming the include guards keeps those headers out and leaves this file as
// the single definition.
#define _GLIBCXX_THREAD 1
#define _GLIBCXX_THREAD_H 1
#define _GLIBCXX_THIS_THREAD_SLEEP_H 1

namespace std
{

// cv_status lives inside the gthreads guard in <condition_variable>, so it
// comes along with the condition variable below.
enum class cv_status { no_timeout, timeout };

class mutex
{
  public:
    mutex() { k_mutex_init(&_m); }
    mutex(const mutex &) = delete;
    mutex &operator=(const mutex &) = delete;

    void lock() { k_mutex_lock(&_m, K_FOREVER); }
    bool try_lock() { return k_mutex_lock(&_m, K_NO_WAIT) == 0; }
    void unlock() { k_mutex_unlock(&_m); }

    k_mutex *native_handle() { return &_m; }

  private:
    struct k_mutex _m;
};

class condition_variable
{
  public:
    condition_variable() { k_condvar_init(&_c); }
    condition_variable(const condition_variable &) = delete;
    condition_variable &operator=(const condition_variable &) = delete;

    void notify_one() { k_condvar_signal(&_c); }
    void notify_all() { k_condvar_broadcast(&_c); }

    void wait(unique_lock<mutex> &lock) { k_condvar_wait(&_c, lock.mutex()->native_handle(), K_FOREVER); }

    template <class Predicate> void wait(unique_lock<mutex> &lock, Predicate pred)
    {
        while (!pred()) {
            wait(lock);
        }
    }

    template <class Rep, class Period> cv_status wait_for(unique_lock<mutex> &lock, const chrono::duration<Rep, Period> &d)
    {
        const auto ms = chrono::duration_cast<chrono::milliseconds>(d).count();
        const int rc = k_condvar_wait(&_c, lock.mutex()->native_handle(), K_MSEC(ms));
        return rc == 0 ? cv_status::no_timeout : cv_status::timeout;
    }

    template <class Rep, class Period, class Predicate>
    bool wait_for(unique_lock<mutex> &lock, const chrono::duration<Rep, Period> &d, Predicate pred)
    {
        const auto deadline = chrono::steady_clock::now() + d;
        while (!pred()) {
            const auto left = deadline - chrono::steady_clock::now();
            if (left <= chrono::steady_clock::duration::zero())
                return pred();
            wait_for(lock, left);
        }
        return true;
    }

  private:
    struct k_condvar _c;
};

class thread
{
  public:
    class id
    {
      public:
        id() = default;
        explicit id(k_tid_t t) : _t(t) {}
        friend bool operator==(id a, id b) { return a._t == b._t; }
        friend bool operator!=(id a, id b) { return a._t != b._t; }
        friend bool operator<(id a, id b) { return a._t < b._t; }

      private:
        k_tid_t _t = nullptr;
    };

    // Stack size for threads started this way. device-ui's UI task runs LVGL
    // rendering, which is the deepest user of this.
    static constexpr size_t STACK_SIZE = 8192;

    thread() = default;
    thread(const thread &) = delete;
    thread &operator=(const thread &) = delete;

    template <class Function, class... Args> explicit thread(Function &&f, Args &&...args)
    {
        auto *work =
            new function<void()>([f = forward<Function>(f), ... args = forward<Args>(args)]() mutable { invoke(f, args...); });
        start(work);
    }

    thread(thread &&other) noexcept { swap(other); }

    thread &operator=(thread &&other) noexcept
    {
        if (this != &other) {
            // A joinable thread being overwritten would terminate() in a
            // hosted implementation; detaching is the closest thing that keeps
            // a display task alive here.
            detach();
            swap(other);
        }
        return *this;
    }

    ~thread() { detach(); }

    bool joinable() const { return _tid != nullptr; }
    id get_id() const { return id(_tid); }

    void join()
    {
        if (_tid) {
            k_thread_join(_tid, K_FOREVER);
            release();
        }
    }

    void detach() { release(); }

    void swap(thread &other) noexcept
    {
        auto tid = _tid;
        auto tcb = _tcb;
        auto stack = _stack;
        _tid = other._tid;
        _tcb = other._tcb;
        _stack = other._stack;
        other._tid = tid;
        other._tcb = tcb;
        other._stack = stack;
    }

  private:
    static void trampoline(void *p1, void *, void *)
    {
        auto *work = static_cast<function<void()> *>(p1);
        (*work)();
        delete work;
    }

    void start(function<void()> *work)
    {
        _stack = k_thread_stack_alloc(STACK_SIZE, 0);
        if (!_stack) {
            delete work;
            return;
        }
        _tcb = new k_thread();
        _tid = k_thread_create(_tcb, _stack, STACK_SIZE, trampoline, work, nullptr, nullptr, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
    }

    // Only drops this object's handle on the thread; the thread keeps running
    // and its stack is intentionally not freed, because nothing here tracks
    // when it has finished with it.
    void release()
    {
        _tid = nullptr;
        _tcb = nullptr;
        _stack = nullptr;
    }

    k_tid_t _tid = nullptr;
    k_thread *_tcb = nullptr;
    k_thread_stack_t *_stack = nullptr;
};

namespace this_thread
{

inline thread::id get_id()
{
    return thread::id(k_current_get());
}

inline void yield()
{
    k_yield();
}

template <class Rep, class Period> inline void sleep_for(const chrono::duration<Rep, Period> &d)
{
    const auto ms = chrono::duration_cast<chrono::milliseconds>(d).count();
    if (ms > 0) {
        k_msleep((int32_t)ms);
    } else {
        k_yield();
    }
}

} // namespace this_thread

} // namespace std
