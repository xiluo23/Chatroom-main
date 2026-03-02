#pragma once 
#include"ThreadAndConnectionPool.h"
#include <memory>

// 连接守卫类 - 自动管理连接的获取和释放（RAII模式）
class DbConnectionGuard {
private:
    std::unique_ptr<MyDb> conn;
    ThreadAndConnectionPool* pool;
    bool owned;
public:
    DbConnectionGuard(ThreadAndConnectionPool* p) : pool(p), owned(true) {
        conn = pool->get_conn();
        if(!conn){
            cerr << "Failed to acquire database connection" << endl;
            owned = false;
        }
    }
    
    // Disable copying
    DbConnectionGuard(const DbConnectionGuard&) = delete;
    DbConnectionGuard& operator=(const DbConnectionGuard&) = delete;

    // Allow moving
    DbConnectionGuard(DbConnectionGuard&&) = default;
    DbConnectionGuard& operator=(DbConnectionGuard&&) = default;
    
    ~DbConnectionGuard() {
        if(owned && conn){
            pool->en_conn(std::move(conn));
        }
    }
    
    MyDb* get() const {
        return conn.get();
    }
    
    bool is_valid() const {
        return conn != nullptr;
    }
};
