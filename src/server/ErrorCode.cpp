#include "ErrorCode.h"
#include <iostream>

using namespace std;

// 静态成员初始化
ErrorCodeManager* ErrorCodeManager::instance = nullptr;
unordered_map<int, string> ErrorCodeManager::error_descriptions;
unordered_map<int, string> ErrorCodeManager::error_categories;

/**
 * @brief 初始化错误码映射表
 */
//错误码->描述
//错误码->类型
void ErrorCodeManager::initializeErrorCodes() {
    // ==================== 网络层错误 ====================
    error_descriptions[ERR_SUCCESS] = "操作成功";
    error_categories[ERR_SUCCESS] = "SUCCESS";

    error_descriptions[ERR_SOCKET_SETOPT_FAIL]="Socket中setopt失败";
    error_categories[ERR_SOCKET_SETOPT_FAIL]="NETWORK";

    error_descriptions[ERR_SOCKET_CREATE_FAIL] = "Socket创建失败";
    error_categories[ERR_SOCKET_CREATE_FAIL] = "NETWORK";

    error_descriptions[ERR_SOCKET_BIND_FAIL] = "Socket绑定失败,端口可能已被占用";
    error_categories[ERR_SOCKET_BIND_FAIL] = "NETWORK";

    error_descriptions[ERR_SOCKET_LISTEN_FAIL] = "Socket监听失败";
    error_categories[ERR_SOCKET_LISTEN_FAIL] = "NETWORK";

    error_descriptions[ERR_SOCKET_ACCEPT_FAIL] = "接受客户端连接失败";
    error_categories[ERR_SOCKET_ACCEPT_FAIL] = "NETWORK";

    error_descriptions[ERR_SOCKET_SEND_FAIL] = "发送数据失败";
    error_categories[ERR_SOCKET_SEND_FAIL] = "NETWORK";

    error_descriptions[ERR_SOCKET_RECV_FAIL] = "接收数据失败";
    error_categories[ERR_SOCKET_RECV_FAIL] = "NETWORK";

    error_descriptions[ERR_SOCKET_CLOSE_FAIL] = "关闭Socket失败";
    error_categories[ERR_SOCKET_CLOSE_FAIL] = "NETWORK";

    error_descriptions[ERR_EPOLL_CREATE_FAIL] = "创建epoll失败";
    error_categories[ERR_EPOLL_CREATE_FAIL] = "NETWORK";

    error_descriptions[ERR_EPOLL_CTL_FAIL] = "epoll控制操作失败";
    error_categories[ERR_EPOLL_CTL_FAIL] = "NETWORK";

    error_descriptions[ERR_EPOLL_WAIT_FAIL] = "epoll等待失败";
    error_categories[ERR_EPOLL_WAIT_FAIL] = "NETWORK";

    error_descriptions[ERR_CONNECTION_TIMEOUT] = "连接超时";
    error_categories[ERR_CONNECTION_TIMEOUT] = "NETWORK";

    error_descriptions[ERR_CONNECTION_CLOSED] = "连接已关闭";
    error_categories[ERR_CONNECTION_CLOSED] = "NETWORK";

    // ==================== 数据库错误 ====================
    error_descriptions[ERR_DB_CONNECTION_FAIL] = "无法连接到数据库";
    error_categories[ERR_DB_CONNECTION_FAIL] = "DATABASE";

    error_descriptions[ERR_DB_QUERY_FAIL] = "数据库查询失败";
    error_categories[ERR_DB_QUERY_FAIL] = "DATABASE";

    error_descriptions[ERR_DB_EXECUTE_FAIL] = "数据库执行失败";
    error_categories[ERR_DB_EXECUTE_FAIL] = "DATABASE";

    error_descriptions[ERR_DB_NO_RESULT] = "数据库查询无结果";
    error_categories[ERR_DB_NO_RESULT] = "DATABASE";

    error_descriptions[ERR_DB_TRANSACTION_FAIL] = "数据库事务失败";
    error_categories[ERR_DB_TRANSACTION_FAIL] = "DATABASE";

    error_descriptions[ERR_DB_DISCONNECT] = "数据库连接已断开";
    error_categories[ERR_DB_DISCONNECT] = "DATABASE";

    error_descriptions[ERR_DB_LOCK_FAIL] = "数据库锁定失败";
    error_categories[ERR_DB_LOCK_FAIL] = "DATABASE";

    // ==================== 用户认证错误 ====================
    error_descriptions[ERR_USER_NOT_FOUND] = "用户不存在";
    error_categories[ERR_USER_NOT_FOUND] = "AUTHENTICATION";

    error_descriptions[ERR_PASSWORD_INCORRECT] = "密码错误";
    error_categories[ERR_PASSWORD_INCORRECT] = "AUTHENTICATION";

    error_descriptions[ERR_USER_ALREADY_EXISTS] = "用户已存在";
    error_categories[ERR_USER_ALREADY_EXISTS] = "AUTHENTICATION";

    error_descriptions[ERR_USER_DISABLED] = "用户被禁用";
    error_categories[ERR_USER_DISABLED] = "AUTHENTICATION";

    error_descriptions[ERR_USER_NOT_LOGIN] = "用户未登录";
    error_categories[ERR_USER_NOT_LOGIN] = "AUTHENTICATION";

    error_descriptions[ERR_USER_ALREADY_LOGIN] = "用户已登录";
    error_categories[ERR_USER_ALREADY_LOGIN] = "AUTHENTICATION";

    error_descriptions[ERR_USER_SESSION_EXPIRED] = "用户会话已过期";
    error_categories[ERR_USER_SESSION_EXPIRED] = "AUTHENTICATION";

    error_descriptions[ERR_PASSWORD_WEAK] = "密码强度不够";
    error_categories[ERR_PASSWORD_WEAK] = "AUTHENTICATION";

    error_descriptions[ERR_INVALID_USERNAME] = "用户名格式无效";
    error_categories[ERR_INVALID_USERNAME] = "AUTHENTICATION";

    // ==================== 消息处理错误 ====================
    error_descriptions[ERR_MSG_INVALID_FORMAT] = "消息格式无效";
    error_categories[ERR_MSG_INVALID_FORMAT] = "MESSAGE";

    error_descriptions[ERR_MSG_PARSE_FAIL] = "消息解析失败";
    error_categories[ERR_MSG_PARSE_FAIL] = "MESSAGE";

    error_descriptions[ERR_MSG_SEND_FAIL] = "消息发送失败";
    error_categories[ERR_MSG_SEND_FAIL] = "MESSAGE";

    error_descriptions[ERR_MSG_RECEIVE_FAIL] = "消息接收失败";
    error_categories[ERR_MSG_RECEIVE_FAIL] = "MESSAGE";

    error_descriptions[ERR_MSG_TOO_LARGE] = "消息过大";
    error_categories[ERR_MSG_TOO_LARGE] = "MESSAGE";

    error_descriptions[ERR_MSG_EMPTY] = "消息为空";
    error_categories[ERR_MSG_EMPTY] = "MESSAGE";

    error_descriptions[ERR_MSG_INVALID_COMMAND] = "无效的命令";
    error_categories[ERR_MSG_INVALID_COMMAND] = "MESSAGE";

    // ==================== 业务逻辑错误 ====================
    error_descriptions[ERR_RECIPIENT_NOT_FOUND] = "接收者不存在";
    error_categories[ERR_RECIPIENT_NOT_FOUND] = "BUSINESS";

    error_descriptions[ERR_RECIPIENT_OFFLINE] = "接收者离线";
    error_categories[ERR_RECIPIENT_OFFLINE] = "BUSINESS";

    error_descriptions[ERR_GROUP_NOT_FOUND] = "群组不存在";
    error_categories[ERR_GROUP_NOT_FOUND] = "BUSINESS";

    error_descriptions[ERR_USER_NOT_IN_GROUP] = "用户不在群组中";
    error_categories[ERR_USER_NOT_IN_GROUP] = "BUSINESS";

    error_descriptions[ERR_PERMISSION_DENIED] = "权限不足";
    error_categories[ERR_PERMISSION_DENIED] = "BUSINESS";

    error_descriptions[ERR_OPERATION_NOT_ALLOWED] = "操作不被允许";
    error_categories[ERR_OPERATION_NOT_ALLOWED] = "BUSINESS";

    error_descriptions[ERR_DUPLICATE_OPERATION] = "重复操作";
    error_categories[ERR_DUPLICATE_OPERATION] = "BUSINESS";

    error_descriptions[ERR_RESOURCE_EXHAUSTED] = "资源耗尽";
    error_categories[ERR_RESOURCE_EXHAUSTED] = "BUSINESS";

    // ==================== 线程和并发错误 ====================
    error_descriptions[ERR_THREAD_POOL_FULL] = "线程池已满";
    error_categories[ERR_THREAD_POOL_FULL] = "CONCURRENCY";

    error_descriptions[ERR_THREAD_CREATE_FAIL] = "线程创建失败";
    error_categories[ERR_THREAD_CREATE_FAIL] = "CONCURRENCY";

    error_descriptions[ERR_THREAD_JOIN_FAIL] = "线程等待失败";
    error_categories[ERR_THREAD_JOIN_FAIL] = "CONCURRENCY";

    error_descriptions[ERR_MUTEX_LOCK_FAIL] = "互斥锁锁定失败";
    error_categories[ERR_MUTEX_LOCK_FAIL] = "CONCURRENCY";

    error_descriptions[ERR_MUTEX_UNLOCK_FAIL] = "互斥锁解锁失败";
    error_categories[ERR_MUTEX_UNLOCK_FAIL] = "CONCURRENCY";

    error_descriptions[ERR_DEADLOCK_DETECTED] = "检测到死锁";
    error_categories[ERR_DEADLOCK_DETECTED] = "CONCURRENCY";

    error_descriptions[ERR_RACE_CONDITION] = "检测到竞态条件";
    error_categories[ERR_RACE_CONDITION] = "CONCURRENCY";

    // ==================== 文件和系统错误 ====================
    error_descriptions[ERR_FILE_OPEN_FAIL] = "文件打开失败";
    error_categories[ERR_FILE_OPEN_FAIL] = "SYSTEM";

    error_descriptions[ERR_FILE_READ_FAIL] = "文件读取失败";
    error_categories[ERR_FILE_READ_FAIL] = "SYSTEM";

    error_descriptions[ERR_FILE_WRITE_FAIL] = "文件写入失败";
    error_categories[ERR_FILE_WRITE_FAIL] = "SYSTEM";

    error_descriptions[ERR_FILE_NOT_FOUND] = "文件不存在";
    error_categories[ERR_FILE_NOT_FOUND] = "SYSTEM";

    error_descriptions[ERR_FILE_PERMISSION_DENIED] = "文件权限拒绝";
    error_categories[ERR_FILE_PERMISSION_DENIED] = "SYSTEM";

    error_descriptions[ERR_DISK_SPACE_FULL] = "磁盘空间满";
    error_categories[ERR_DISK_SPACE_FULL] = "SYSTEM";

    error_descriptions[ERR_SYSTEM_CALL_FAIL] = "系统调用失败";
    error_categories[ERR_SYSTEM_CALL_FAIL] = "SYSTEM";

    // ==================== 内存管理错误 ====================
    error_descriptions[ERR_MEMORY_ALLOC_FAIL] = "内存分配失败";
    error_categories[ERR_MEMORY_ALLOC_FAIL] = "MEMORY";

    error_descriptions[ERR_MEMORY_LEAK] = "检测到内存泄漏";
    error_categories[ERR_MEMORY_LEAK] = "MEMORY";

    error_descriptions[ERR_NULL_POINTER] = "空指针错误";
    error_categories[ERR_NULL_POINTER] = "MEMORY";

    error_descriptions[ERR_BUFFER_OVERFLOW] = "缓冲区溢出";
    error_categories[ERR_BUFFER_OVERFLOW] = "MEMORY";

    // ==================== 配置和初始化错误 ====================
    error_descriptions[ERR_CONFIG_LOAD_FAIL] = "配置加载失败";
    error_categories[ERR_CONFIG_LOAD_FAIL] = "CONFIGURATION";

    error_descriptions[ERR_CONFIG_INVALID] = "配置无效";
    error_categories[ERR_CONFIG_INVALID] = "CONFIGURATION";

    error_descriptions[ERR_INIT_FAIL] = "初始化失败";
    error_categories[ERR_INIT_FAIL] = "CONFIGURATION";

    error_descriptions[ERR_PARAMETER_INVALID] = "参数无效";
    error_categories[ERR_PARAMETER_INVALID] = "CONFIGURATION";

    // ==================== 未分类错误 ====================
    error_descriptions[ERR_UNKNOWN] = "未知错误";
    error_categories[ERR_UNKNOWN] = "UNKNOWN";
}

/**
 * @brief 获取错误码的描述
 */
string ErrorCodeManager::getDescription(int error_code) {
    auto it = error_descriptions.find(error_code);
    if (it != error_descriptions.end()) {
        return it->second;
    }
    return "未定义的错误码: " + to_string(error_code);
}

/**
 * @brief 获取错误码的分类
 */
string ErrorCodeManager::getCategory(int error_code) {
    auto it = error_categories.find(error_code);
    if (it != error_categories.end()) {
        return it->second;
    }
    return "UNKNOWN";
}

/**
 * @brief 获取完整的错误信息
 */
string ErrorCodeManager::getFullMessage(int error_code) {
    string category = getCategory(error_code);
    string description = getDescription(error_code);
    return "[" + category + "] (" + to_string(error_code) + ") " + description;
}
