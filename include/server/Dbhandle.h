#pragma once
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <string>
#include <atomic>
#include "MyDb.h"
#include "Config.h"

class DbHandle {
public:
    static DbHandle* getInstance();

    // 启动异步写入线程
    void start();

    // 停止线程
    void stop();

    // 将 SQL 语句加入异步队列
    void add_task(const std::string& sql);

private:
    DbHandle() : _running(false) {}
    ~DbHandle() { stop(); }

    void handle_db();

    std::queue<std::string> _tasks;
    std::mutex _mutex;
    std::condition_variable _cv;
    std::thread _db_thread;
    std::atomic<bool> _running;
};
