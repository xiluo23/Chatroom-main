#include "server/ThreadAndConnectionPool.h"
#include <iostream>
#include <unistd.h>
#include <atomic>
#include <memory>

// Mock process_clint_data
std::atomic<int> tasks_processed{0};

void process_clint_data(Task &task) {
    tasks_processed++;
    // Simulate work
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

int main() {
    std::cout << "Starting ThreadAndConnectionPool test..." << std::endl;

    // Test constructor
    ThreadAndConnectionPool pool(4); // max threads = 4

    // Test connection acquisition
    std::unique_ptr<MyDb> conn = pool.get_conn();
    if (conn) {
        std::cout << "Got connection!" << std::endl;
        pool.en_conn(std::move(conn));
        std::cout << "Returned connection!" << std::endl;
    } else {
        std::cerr << "Failed to get connection!" << std::endl;
        return 1;
    }

    // Test task submission
    for (int i = 0; i < 10; ++i) {
        Task t;
        t.fd = i;
        pool.addTask(t);
    }

    // Wait for tasks to process
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    std::cout << "Tasks processed: " << tasks_processed << std::endl;
    if (tasks_processed == 10) {
        std::cout << "All tasks processed successfully." << std::endl;
    } else {
        std::cerr << "Error: Not all tasks processed." << std::endl;
        return 1;
    }

    std::cout << "Stopping pool..." << std::endl;
    pool.stop();
    std::cout << "Pool stopped." << std::endl;

    return 0;
}
