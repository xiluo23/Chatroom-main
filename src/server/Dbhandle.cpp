#include"Dbhandle.h"

DbHandle*DbHandle::getInstance() {
    static DbHandle instance;
    return &instance;
}
// 启动异步写入线程
void DbHandle::start() {
    if (!_running.exchange(true)) {
        _db_thread = std::thread(&DbHandle::handle_db, this);
    }
}
// 停止线程
void DbHandle::stop() {
    if (_running.exchange(false)) {
        _cv.notify_all();
        if (_db_thread.joinable()) {
            _db_thread.join();
        }
    }
}
// 将 SQL 语句加入异步队列
void DbHandle::add_task(const std::string& sql) {
{
    std::lock_guard<std::mutex> lock(_mutex);
    _tasks.push(sql);
}
    _cv.notify_one();
}

void DbHandle::handle_db() {
    MyDb db;
    Config* conf = Config::getInstance();
    if (!db.initDB(conf->getString("mysql_host", "127.0.0.1"),
                  conf->getString("mysql_user", "ftpuser"),
                  conf->getString("mysql_password", "926472"),
                  conf->getString("mysql_dbname", "Chatroom"),
                  conf->getInt("mysql_port", 3306))) {
        return;
    }

    while (_running) {
        std::string sql;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] { return !_tasks.empty() || !_running; });
        
            if (!_running && _tasks.empty()) break;
        
            if (!_tasks.empty()) {
                sql = std::move(_tasks.front());
                _tasks.pop();
            }
        }

        if (!sql.empty()) {
                db.exeSQL(sql);
        }
    }
}