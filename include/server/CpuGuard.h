#pragma once
#include <mutex>
#include <condition_variable>

/**
 * @brief 用于限制高 CPU 任务（如 crypt_r）的并发数，防止 2 核环境下 Reactor 线程被饿死
 */
class CpuGuard {
public:
    static CpuGuard* getInstance() {
        static CpuGuard instance(2); // 2 核环境下限制为 2 个并发计算
        return &instance;
    }

    void enter() {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this] { return _current < _max; });
        _current++;
    }

    void leave() {
        std::lock_guard<std::mutex> lock(_mutex);
        _current--;
        _cv.notify_one();
    }

private:
    CpuGuard(int max) : _max(max), _current(0) {}
    int _max;
    int _current;
    std::mutex _mutex;
    std::condition_variable _cv;
};

class CpuScopedGuard {
public:
    CpuScopedGuard() {
        CpuGuard::getInstance()->enter();
    }
    ~CpuScopedGuard() {
        CpuGuard::getInstance()->leave();
    }
};
