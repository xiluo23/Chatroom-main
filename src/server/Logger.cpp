#include "Logger.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <cstring>

using namespace std;

// 静态成员初始化
Logger* Logger::instance = nullptr;

/**
 * @brief Logger构造函数
 */
Logger::Logger() 
    : current_level(LogLevel::INFO), 
      max_file_size(10 * 1024 * 1024),  // 10MB
      current_file_size(0),
      console_output(true) {
    pthread_mutex_init(&log_mutex, nullptr);
}

/**
 * @brief Logger析构函数
 */
Logger::~Logger() {
    close();
    pthread_mutex_destroy(&log_mutex);
}

/**
 * @brief 获取当前时间戳
 */
string Logger::getCurrentTimestamp() {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    return string(buffer);
}

/**
 * @brief 获取日志级别的字符串表示
 */
string Logger::getLevelString(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE:   return "TRACE";
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO ";
        case LogLevel::WARNING: return "WARN ";
        case LogLevel::ERROR_:  return "ERROR";
        case LogLevel::FATAL:   return "FATAL";
        default:                return "UNKN ";
    }
}

/**
 * @brief 初始化日志系统
 */
bool Logger::initialize(const string& dir, 
                       const string& filename,
                       LogLevel level,
                       bool enable_console) {
    pthread_mutex_lock(&log_mutex);

    log_dir = dir;
    log_filename = filename;
    current_level = level;
    console_output = enable_console;

    // 创建日志目录
    DIR* directory = opendir(dir.c_str());
    if (!directory) {
        if (mkdir(dir.c_str(), 0755) != 0) {
            cerr << "Failed to create log directory: " << dir << endl;
            pthread_mutex_unlock(&log_mutex);
            return false;
        }
    } else {
        closedir(directory);
    }

    // 打开日志文件
    string full_path = dir + "/" + filename;
    log_file.open(full_path, ios::app);
    if (!log_file.is_open()) {
        cerr << "Failed to open log file: " << full_path << endl;
        pthread_mutex_unlock(&log_mutex);
        return false;
    }

    // 获取当前文件大小
    ifstream file_check(full_path);
    if (file_check) {
        file_check.seekg(0, ios::end);
        current_file_size = file_check.tellg();
        file_check.close();
    }

    pthread_mutex_unlock(&log_mutex);

    info("Logger initialized successfully");
    return true;
}

/**
 * @brief 轮转日志文件
 */
void Logger::rotateLogFile() {
    if (log_file.is_open()) {
        log_file.close();
    }

    // 生成带时间戳的备份文件名
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", timeinfo);

    string old_path = log_dir + "/" + log_filename;
    string new_path = log_dir + "/" + log_filename + "." + timestamp;

    // 重命名当前日志文件
    if (rename(old_path.c_str(), new_path.c_str()) != 0) {
        cerr << "Failed to rotate log file" << endl;
    }

    // 打开新的日志文件
    log_file.open(old_path, ios::app);
    current_file_size = 0;
}

/**
 * @brief 检查日志文件是否需要轮转
 */
void Logger::checkAndRotateLogFile() {
    if (current_file_size > max_file_size) {
        rotateLogFile();
    }
}

/**
 * @brief 内部日志写入函数
 */
void Logger::writeLog(LogLevel level, const string& message, int error_code) {
    pthread_mutex_lock(&log_mutex);

    // 检查是否需要轮转日志文件
    checkAndRotateLogFile();

    string timestamp = getCurrentTimestamp();
    string level_str = getLevelString(level);
    pthread_t thread_id = pthread_self();

    // 构建日志消息
    stringstream ss;
    ss << "[" << timestamp << "] "
       << "[" << level_str << "] "
       << "[TID:" << thread_id << "] ";

    // 如果有错误码，添加错误信息
    if (error_code != 0) {
        string error_info = ErrorCodeManager::getFullMessage(error_code);
        ss << "[ERR:" << error_info << "] ";
    }

    ss << message;

    string log_message = ss.str();

    // 写入文件
    if (log_file.is_open()) {
        log_file << log_message << "\n";
        log_file.flush();
        current_file_size += log_message.length() + 1;  // +1 for newline
    }

    // 输出到控制台
    if (console_output) {
        cout << log_message << "\n";
    }

    pthread_mutex_unlock(&log_mutex);
}

/**
 * @brief 记录操作日志
 */
void Logger::logOperation(int user_id, const string& operation, const string& details) {
    stringstream ss;
    ss << "Operation - UserID: " << user_id 
       << " | Op: " << operation;
    if (!details.empty()) {
        ss << " | Details: " << details;
    }
    info(ss.str());
}

/**
 * @brief 记录数据库错误
 */
void Logger::logDatabaseError(const string& sql, const string& error_msg, int error_code) {
    stringstream ss;
    ss << "Database Error - SQL: " << sql << " | Error: " << error_msg;
    error(ss.str(), error_code);
}

/**
 * @brief 记录网络错误
 */
void Logger::logNetworkError(int client_fd, const string& error_msg, int error_code) {
    stringstream ss;
    ss << "Network Error - ClientFD: " << client_fd << " | Error: " << error_msg;
    error(ss.str(), error_code);
}
