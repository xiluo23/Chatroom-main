#include"ParserPool.h"


ParserPool* ParserPool::getInstance() {
        static ParserPool instance;
        return &instance;
    }

void ParserPool::init(int threadCount, std::unordered_map<int, std::unique_ptr<ClientBuffer>>* clientBuffers, 
          pthread_mutex_t* bufferMutex, ThreadAndConnectionPool* workerPool) {
    _client_buffers = clientBuffers;
    _buffer_mutex = bufferMutex;
    _worker_pool = workerPool;
    _thread_count = threadCount;
}

void ParserPool::start() {
    if (!_running.exchange(true)) {
        for (int i = 0; i < _thread_count; ++i) {
            _threads.emplace_back(&ParserPool::workerLoop, this);
        }
    }
}

void ParserPool::stop() {
    if (_running.exchange(false)) {
        _cv.notify_all();
        for (auto& t : _threads) {
            if (t.joinable()) t.join();
        }
        _threads.clear();
    }
}

void ParserPool::addFd(int fd) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // 始终添加到待处理集合中。workerLoop 会负责确保同一 FD 不会被多个线程同时处理。
        if (_pending_fds.insert(fd).second) {
            _cv.notify_one();
        }
    }
}

void ParserPool::clearFd(int fd) {
    std::lock_guard<std::mutex> lock(_mutex);
    _pending_fds.erase(fd);
}

void ParserPool::workerLoop() {
    while (_running) {
        int fd = -1;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            // 等待有待处理的 FD，且该 FD 不在处理中
            _cv.wait(lock, [this] { 
                if (!_running) return true;
                for (int p_fd : _pending_fds) {
                    if (_processing_fds.count(p_fd) == 0) return true;
                }
                return false;
            });
            
            if (!_running && _pending_fds.empty()) break;
            
            // 找到一个待处理且不在处理中的 FD
            for (auto it = _pending_fds.begin(); it != _pending_fds.end(); ++it) {
                if (_processing_fds.count(*it) == 0) {
                    fd = *it;
                    _pending_fds.erase(it);
                    _processing_fds.insert(fd);
                    break;
                }
            }
        }

        if (fd != -1) {
            processBuffer(fd);
            
            // 处理完成后移除标记，并再次通知（因为此时该 FD 可能已重新进入 _pending_fds）
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _processing_fds.erase(fd);
            }
            _cv.notify_all(); // 通知其他线程，可能现在可以处理该 FD 了
        }
    }
}

void ParserPool::processBuffer(int fd) {
    while (true) {
        // 每次处理一条消息都重新加锁，并检查 fd 是否还存在（防止在解析过程中被关闭）
        pthread_mutex_lock(_buffer_mutex);
        auto it = _client_buffers->find(fd);
        if (it == _client_buffers->end()) {
            pthread_mutex_unlock(_buffer_mutex);
            break;
        }
        ClientBuffer &buf = *(it->second);

        std::string message;
        int consumed = extractMessageCircular(buf.buffer, PROTOCOL_MAX_RECV_BUFFER_SIZE, buf.head, buf.tail, message);
        
        if (consumed == -1) {
            // 数据不完整，等待更多数据
            pthread_mutex_unlock(_buffer_mutex);
            break;
        } else if (consumed == -2) {
            // 无效长度，移位 1 字节
            buf.head = (buf.head + 1) % PROTOCOL_MAX_RECV_BUFFER_SIZE;
            pthread_mutex_unlock(_buffer_mutex);
            continue; // 继续解析
        } else {
            // 完整消息 -> head 已更新
            
            // 重要：先解锁再执行可能耗时的 addTask，避免阻塞其他 Parser 线程和 Reactor 线程
            pthread_mutex_unlock(_buffer_mutex);

            Task task;
            task.fd = fd;
            task.message = std::move(message);
            task.type = CLIENT_MSG;
            _worker_pool->addTask(std::move(task));
            
            // 继续解析缓冲区中剩余的消息
        }
    }
}