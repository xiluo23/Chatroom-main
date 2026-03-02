#pragma once 
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <iostream>
#include <chrono>
#include <memory>
#include "MyDb.h"

// Configuration
#define PORT 3306
#define HOST "192.168.147.130"
#define USER "ftpuser"
#define DB_NAME "Chatroom"
#define PWD "926472"

enum TaskType { CLIENT_MSG = 0, SUB_MSG = 1 };

struct Task{
    int fd; // client_fd (for CLIENT_MSG)
    string message; // raw message
    TaskType type = CLIENT_MSG;
    int channel = 0; // for SUB_MSG: receiver user_id channel
    long long msg_id = 0; // optional message id for tracking
};
void process_clint_data(Task &task);

class ThreadAndConnectionPool {
public:
    // Constructor compatible with legacy usage (ThreadPool pool(32))
    // Sets maxThreads and maxConns to the provided value
    explicit ThreadAndConnectionPool(int maxThreads) 
        : minThreads_(4), maxThreads_(maxThreads), 
          minConns_(4), maxConns_(maxThreads), 
          scaleFactor_(4), idleTimeout_(std::chrono::seconds(2)),
          running_(true), threadCount_(0), idleThreadCount_(0),
          connectionCount_(0)
    {
        init();
    }

    // Full constructor for fine-grained control
    ThreadAndConnectionPool(int minThreads, int maxThreads, int minConns, int maxConns, 
                          std::chrono::milliseconds idleTimeout, int scaleFactor)
        : minThreads_(minThreads), maxThreads_(maxThreads),
          minConns_(minConns), maxConns_(maxConns),
          scaleFactor_(scaleFactor), idleTimeout_(idleTimeout),
          running_(true), threadCount_(0), idleThreadCount_(0),
          connectionCount_(0)
    {
        init();
    }

    ~ThreadAndConnectionPool() {
        stop();
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return; // Already stopped
        }

        cv_.notify_all();
        connCv_.notify_all();

        if (manager_.joinable()) manager_.join();
        
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        
        // Cleanup connections
        std::lock_guard<std::mutex> lock(connMutex_);
        while (!conns_.empty()) {
            conns_.pop();
        }
    }

    void addTask(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    std::unique_ptr<MyDb> get_conn() {
        std::unique_lock<std::mutex> lock(connMutex_);
        
        // Loop until we get a connection or stop
        while (running_) {
            if (!conns_.empty()) {
                std::unique_ptr<MyDb> conn = std::move(conns_.front());
                conns_.pop();
                
                // Optional: Validate connection before returning
                if (!conn->ping()) {
                    // Try to reconnect
                    if (!conn->initDB(HOST, USER, PWD, DB_NAME, PORT)) {
                        std::cerr << "Failed to reconnect database connection" << std::endl;
                        // conn is unique_ptr, will delete automatically
                        connectionCount_--;
                        // Continue loop to try getting/creating another one
                        continue;
                    }
                }
                return conn;
            }

            // Pool is empty, can we create more?
            if (connectionCount_ < maxConns_) {
                // Increment count before creation to reserve spot
                connectionCount_++;
                lock.unlock(); // Release lock during connection creation
                
                std::unique_ptr<MyDb> conn = createConnection();
                if (conn) {
                    return conn;
                } else {
                    std::cerr << "Failed to create dynamic database connection" << std::endl;
                    lock.lock();
                    connectionCount_--; // Revert count
                    // Wait a bit before retrying to avoid spinning?
                }
                lock.lock(); // Re-acquire lock if we failed
            }
            
            // Wait for a connection to be returned
            if (connCv_.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout) {
                 // Timeout
                 if (!running_) return nullptr;
                 // Loop again to check conditions
            }
        }
        return nullptr;
    }

    void en_conn(std::unique_ptr<MyDb> conn) {
        if (!conn) return;
        std::lock_guard<std::mutex> lock(connMutex_);
        conns_.push(std::move(conn));
        connCv_.notify_one();
    }

private:
    void init() {
        // Init min connections
        for (int i = 0; i < minConns_; i++) {
            std::unique_ptr<MyDb> conn = createConnection();
            if (conn) {
                std::lock_guard<std::mutex> lock(connMutex_);
                conns_.push(std::move(conn));
                connectionCount_++;
            } else {
                std::cerr << "Failed to initialize database connection " << i << std::endl;
            }
        }

        // Init min threads
        for (int i = 0; i < minThreads_; i++) {
            addThread();
        }

        manager_ = std::thread(&ThreadAndConnectionPool::managerLoop, this);
    }

    std::unique_ptr<MyDb> createConnection() {
        std::unique_ptr<MyDb> conn(new MyDb());
        if (!conn->initDB(HOST, USER, PWD, DB_NAME, PORT)) {
            return nullptr;
        }
        return conn;
    }

    void addThread() {
        threadCount_++;
        workers_.emplace_back(&ThreadAndConnectionPool::workerLoop, this);
    }

    void workerLoop() {
        while (running_) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                idleThreadCount_++;
                bool got = cv_.wait_for(lock, idleTimeout_, [&]{
                    return !tasks_.empty() || !running_;
                });
                idleThreadCount_--;

                if (!running_ && tasks_.empty()) {
                    threadCount_--;
                    return;
                }

                if (!got) {
                    // Timeout - check if we should scale down
                    if (threadCount_ > minThreads_) {
                        threadCount_--;
                        return;
                    }
                    continue;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }
            
            process_clint_data(task);
        }
        threadCount_--;
    }

    void managerLoop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // --- Manage Threads ---
            int backlog;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                backlog = tasks_.size();
            }
            int curThreads = threadCount_.load();
            int idleThreads = idleThreadCount_.load();

            // Scale up logic
            int effectiveBacklog = backlog - idleThreads;
            if (effectiveBacklog > 0 && curThreads < maxThreads_) {
                int needed = effectiveBacklog / scaleFactor_ + 1;
                int canAdd = maxThreads_ - curThreads;
                int toAdd = std::min(needed, canAdd);
                for (int i = 0; i < toAdd; i++) addThread();
            }

            // --- Manage Connections ---
            {
                std::lock_guard<std::mutex> lock(connMutex_);
                int idleConns = conns_.size();
                int curConns = connectionCount_.load();
                
                // If we have significantly more idle connections than minConns
                if (idleConns > minConns_ && curConns > minConns_) {
                    // Only remove if idle count is high (e.g., > 50% of current)
                    if (idleConns > curConns / 2) { 
                         // unique_ptr automatically deletes object when popped
                         conns_.pop();
                         connectionCount_--;
                    }
                }
            }
        }
    }

    int minThreads_;
    int maxThreads_;
    int minConns_;
    int maxConns_;
    int scaleFactor_;
    std::chrono::milliseconds idleTimeout_;

    std::atomic_bool running_;
    std::atomic_int threadCount_;
    std::atomic_int idleThreadCount_;
    std::atomic_int connectionCount_;

    std::vector<std::thread> workers_;
    std::thread manager_;

    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;

    std::queue<std::unique_ptr<MyDb>> conns_;
    std::mutex connMutex_;
    std::condition_variable connCv_;
};

// Legacy support
using ThreadPool = ThreadAndConnectionPool;
