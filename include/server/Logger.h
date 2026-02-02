/**
 * @file Logger.h
 * @brief 日志系统 - 线程安全的日志记录
 * 
 * 提供以下功能：
 * 1. 日志级别控制（TRACE, DEBUG, INFO, WARNING, ERROR, FATAL）
 * 2. 线程安全的日志写入
 * 3. 日志文件轮转
 * 4. 结构化日志记录（带时间戳、线程ID、错误码等）
 */

#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <ctime>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <pthread.h>
#include "ErrorCode.h"

using namespace std;


// 日志级别
enum class LogLevel {
    TRACE   ,  // 最详细的信息
    DEBUG   ,  // 调试信息
    INFO    ,  // 一般信息
    WARNING ,  // 警告信息
    ERROR_  ,  // 错误信息
    FATAL      // 致命错误
};

/**
 * @class Logger
 * @brief 日志管理器 - 单例模式
 */
class Logger {
private:
    // 单例实例
    static Logger* instance;
    // 日志文件流
    ofstream log_file;
    // 日志互斥锁
    pthread_mutex_t log_mutex;
    // 当前日志级别
    LogLevel current_level;
    // 日志目录
    string log_dir;
    // 日志文件名
    string log_filename;
    // 最大日志文件大小（字节）
    size_t max_file_size;
    // 当前文件大小
    size_t current_file_size;
    // 是否输出到控制台
    bool console_output;
    /**
     * @brief 获取当前时间戳
     */
    string getCurrentTimestamp();
    /**
     * @brief 获取日志级别的字符串表示
     */
    string getLevelString(LogLevel level);
    
    /**
     * @brief 检查日志文件是否需要轮转
     */
    void checkAndRotateLogFile();

    /**
     * @brief 轮转日志文件
     */
    void rotateLogFile();
    
    /**
     * @brief 内部日志写入函数
     */
    void writeLog(LogLevel level, const string& message, int error_code = 0);

public:
    /**
     * @brief 构造函数
     */
    Logger();
    
    /**
     * @brief 析构函数
     */
    ~Logger();
    
    /**
     * @brief 获取单例实例
     */
    static Logger* getInstance() {
        if (instance == nullptr) {
            instance = new Logger();
        }
        return instance;
    }
    
    /**
     * @brief 初始化日志系统
     * @param dir 日志目录
     * @param filename 日志文件名
     * @param level 初始日志级别
     * @param enable_console 是否输出到控制台
     */
    bool initialize(const string& dir = "logs", 
                   const string& filename = "chatroom.log",
                   LogLevel level = LogLevel::INFO,
                   bool enable_console = true);
    
    /**
     * @brief 设置日志级别
     */
    void setLogLevel(LogLevel level) {
        current_level = level;
    }
    
    /**
     * @brief 获取日志级别
     */
    LogLevel getLogLevel() const {
        return current_level;
    }
    
    /**
     * @brief 设置是否输出到控制台
     */
    void setConsoleOutput(bool enable) {
        console_output = enable;
    }
    
    /**
     * @brief 记录TRACE级别日志
     */
    void trace(const string& message) {
        if (current_level <= LogLevel::TRACE) {
            writeLog(LogLevel::TRACE, message);
        }
    }
    
    /**
     * @brief 记录DEBUG级别日志
     */
    void debug(const string& message) {
        if (current_level <= LogLevel::DEBUG) {
            writeLog(LogLevel::DEBUG, message);
        }
    }
    
    /**
     * @brief 记录INFO级别日志
     */
    void info(const string& message) {
        if (current_level <= LogLevel::INFO) {
            writeLog(LogLevel::INFO, message);
        }
    }
    
    /**
     * @brief 记录WARNING级别日志
     */
    void warning(const string& message) {
        if (current_level <= LogLevel::WARNING) {
            writeLog(LogLevel::WARNING, message);
        }
    }
    
    /**
     * @brief 记录ERROR级别日志（带错误码）
     */
    void error(const string& message, int error_code = ERR_UNKNOWN) {
        if (current_level <= LogLevel::ERROR_) {
            writeLog(LogLevel::ERROR_, message, error_code);
        }
    }
    
    /**
     * @brief 记录FATAL级别日志（带错误码）
     */
    void fatal(const string& message, int error_code = ERR_UNKNOWN) {
        if (current_level <= LogLevel::FATAL) {
            writeLog(LogLevel::FATAL, message, error_code);
        }
    }
    
    /**
     * @brief 记录带错误码的日志
     */
    void logWithErrorCode(LogLevel level, const string& message, int error_code) {
        if (current_level <= level) {
            writeLog(level, message, error_code);
        }
    }
    
    /**
     * @brief 记录操作日志（用于审计）
     * @param user_id 用户ID
     * @param operation 操作类型
     * @param details 操作详情
     */
    void logOperation(int user_id, const string& operation, const string& details = "");
    
    /**
     * @brief 记录数据库错误
     */
    void logDatabaseError(const string& sql, const string& error_msg, int error_code);
    
    /**
     * @brief 记录网络错误
     */
    void logNetworkError(int client_fd, const string& error_msg, int error_code);
    
    /**
     * @brief 刷新日志
     */
    void flush() {
        pthread_mutex_lock(&log_mutex);
        if (log_file.is_open()) {
            log_file.flush();
        }
        pthread_mutex_unlock(&log_mutex);
    }
    
    /**
     * @brief 关闭日志系统
     */
    void close() {
        pthread_mutex_lock(&log_mutex);
        if (log_file.is_open()) {
            log_file.close();
        }
        pthread_mutex_unlock(&log_mutex);
    }
    
    /**
     * @brief 销毁单例
     */
    static void destroy() {
        if (instance != nullptr) {
            instance->close();
            delete instance;
            instance = nullptr;
        }
    }
};

// 便捷宏定义 - 简化日志记录
#define LOG_TRACE(msg)              Logger::getInstance()->trace(msg)
#define LOG_DEBUG(msg)              Logger::getInstance()->debug(msg)
#define LOG_INFO(msg)               Logger::getInstance()->info(msg)
#define LOG_WARN(msg)               Logger::getInstance()->warning(msg)
#define LOG_ERROR(msg, code)        Logger::getInstance()->error(msg, code)
#define LOG_FATAL(msg, code)        Logger::getInstance()->fatal(msg, code)
#define LOG_OPERATION(uid, op, det) Logger::getInstance()->logOperation(uid, op, det)
#define LOG_DB_ERROR(sql, err, code) Logger::getInstance()->logDatabaseError(sql, err, code)
#define LOG_NET_ERROR(fd, err, code) Logger::getInstance()->logNetworkError(fd, err, code)
