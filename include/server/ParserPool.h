#pragma once
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <string>
#include "Protocol.h"
#include "ThreadAndConnectionPool.h"
#include "epoll_ser.h"

class ParserPool {
public:
    static ParserPool* getInstance();

    void init(int threadCount, std::unordered_map<int, std::unique_ptr<ClientBuffer>>* clientBuffers, 
              pthread_mutex_t* bufferMutex, ThreadAndConnectionPool* workerPool);

    void start();

    void stop();

    void addFd(int fd);

    void clearFd(int fd);

private:
    ParserPool() : _running(false), _thread_count(0), _client_buffers(nullptr), 
                  _buffer_mutex(nullptr), _worker_pool(nullptr) {}
    ~ParserPool() { stop(); }

    void workerLoop();

    void processBuffer(int fd);

    std::unordered_set<int> _pending_fds;
    std::unordered_set<int> _processing_fds; // 正在处理中的 FD
    std::mutex _mutex;
    std::condition_variable _cv;
    std::vector<std::thread> _threads;
    std::atomic<bool> _running;
    int _thread_count;

    // 外部引用
    std::unordered_map<int, std::unique_ptr<ClientBuffer>>* _client_buffers;
    pthread_mutex_t* _buffer_mutex;
    ThreadAndConnectionPool* _worker_pool;
};
