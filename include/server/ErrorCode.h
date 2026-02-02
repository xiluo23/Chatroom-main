#pragma once

/**
 * @file ErrorCode.h
 * @brief 错误码定义和错误处理
 * 
 * 统一管理系统中所有的错误码，便于日志记录和调试
 */

#include <string>
#include <unordered_map>

using namespace std;

// ==================== 错误码定义 ====================

// 基础错误码范围划分
#define ERR_SUCCESS                 0    // 成功

// 网络层错误 (1000-1999)
#define ERR_SOCKET_CREATE_FAIL      1001 // Socket创建失败
#define ERR_SOCKET_BIND_FAIL        1002 // Socket绑定失败
#define ERR_SOCKET_LISTEN_FAIL      1003 // Socket监听失败
#define ERR_SOCKET_ACCEPT_FAIL      1004 // Socket接受连接失败
#define ERR_SOCKET_SEND_FAIL        1005 // Socket发送失败
#define ERR_SOCKET_RECV_FAIL        1006 // Socket接收失败
#define ERR_SOCKET_CLOSE_FAIL       1007 // Socket关闭失败
#define ERR_EPOLL_CREATE_FAIL       1008 // epoll创建失败
#define ERR_EPOLL_CTL_FAIL          1009 // epoll控制失败
#define ERR_EPOLL_WAIT_FAIL         1010 // epoll等待失败
#define ERR_CONNECTION_TIMEOUT      1011 // 连接超时
#define ERR_CONNECTION_CLOSED       1012 // 连接已关闭
#define ERR_SOCKET_SETOPT_FAIL      1013//Socket设置opt失败

// 数据库错误 (2000-2999)
#define ERR_DB_CONNECTION_FAIL      2001 // 数据库连接失败
#define ERR_DB_QUERY_FAIL           2002 // 数据库查询失败
#define ERR_DB_EXECUTE_FAIL         2003 // 数据库执行失败
#define ERR_DB_NO_RESULT            2004 // 数据库无结果
#define ERR_DB_TRANSACTION_FAIL     2005 // 事务失败
#define ERR_DB_DISCONNECT           2006 // 数据库已断开连接
#define ERR_DB_LOCK_FAIL            2007 // 数据库锁定失败

// 用户认证错误 (3000-3999)
#define ERR_USER_NOT_FOUND          3001 // 用户不存在
#define ERR_PASSWORD_INCORRECT      3002 // 密码错误
#define ERR_USER_ALREADY_EXISTS     3003 // 用户已存在
#define ERR_USER_DISABLED           3004 // 用户被禁用
#define ERR_USER_NOT_LOGIN          3005 // 用户未登录
#define ERR_USER_ALREADY_LOGIN      3006 // 用户已登录
#define ERR_USER_SESSION_EXPIRED    3007 // 用户会话已过期
#define ERR_PASSWORD_WEAK           3008 // 密码强度不够
#define ERR_INVALID_USERNAME        3009 // 用户名格式无效

// 消息处理错误 (4000-4999)
#define ERR_MSG_INVALID_FORMAT      4001 // 消息格式无效
#define ERR_MSG_PARSE_FAIL          4002 // 消息解析失败
#define ERR_MSG_SEND_FAIL           4003 // 消息发送失败
#define ERR_MSG_RECEIVE_FAIL        4004 // 消息接收失败
#define ERR_MSG_TOO_LARGE           4005 // 消息过大
#define ERR_MSG_EMPTY               4006 // 消息为空
#define ERR_MSG_INVALID_COMMAND     4007 // 无效的命令

// 业务逻辑错误 (5000-5999)
#define ERR_RECIPIENT_NOT_FOUND     5001 // 接收者不存在
#define ERR_RECIPIENT_OFFLINE       5002 // 接收者离线
#define ERR_GROUP_NOT_FOUND         5003 // 群组不存在
#define ERR_USER_NOT_IN_GROUP       5004 // 用户不在群组中
#define ERR_PERMISSION_DENIED       5005 // 权限不足
#define ERR_OPERATION_NOT_ALLOWED   5006 // 操作不被允许
#define ERR_DUPLICATE_OPERATION     5007 // 重复操作
#define ERR_RESOURCE_EXHAUSTED      5008 // 资源耗尽

// 线程和并发错误 (6000-6999)
#define ERR_THREAD_POOL_FULL        6001 // 线程池已满
#define ERR_THREAD_CREATE_FAIL      6002 // 线程创建失败
#define ERR_THREAD_JOIN_FAIL        6003 // 线程等待失败
#define ERR_MUTEX_LOCK_FAIL         6004 // 互斥锁锁定失败
#define ERR_MUTEX_UNLOCK_FAIL       6005 // 互斥锁解锁失败
#define ERR_DEADLOCK_DETECTED       6006 // 检测到死锁
#define ERR_RACE_CONDITION          6007 // 竞态条件

// 文件和系统错误 (7000-7999)
#define ERR_FILE_OPEN_FAIL          7001 // 文件打开失败
#define ERR_FILE_READ_FAIL          7002 // 文件读取失败
#define ERR_FILE_WRITE_FAIL         7003 // 文件写入失败
#define ERR_FILE_NOT_FOUND          7004 // 文件不存在
#define ERR_FILE_PERMISSION_DENIED  7005 // 文件权限拒绝
#define ERR_DISK_SPACE_FULL         7006 // 磁盘空间满
#define ERR_SYSTEM_CALL_FAIL        7007 // 系统调用失败

// 内存管理错误 (8000-8999)
#define ERR_MEMORY_ALLOC_FAIL       8001 // 内存分配失败
#define ERR_MEMORY_LEAK             8002 // 内存泄漏
#define ERR_NULL_POINTER            8003 // 空指针错误
#define ERR_BUFFER_OVERFLOW         8004 // 缓冲区溢出

// 配置和初始化错误 (9000-9999)
#define ERR_CONFIG_LOAD_FAIL        9001 // 配置加载失败
#define ERR_CONFIG_INVALID          9002 // 配置无效
#define ERR_INIT_FAIL               9003 // 初始化失败
#define ERR_PARAMETER_INVALID       9004 // 参数无效

// 未分类错误 (10000+)
#define ERR_UNKNOWN                 10000 // 未知错误


/**
 * @class ErrorCodeManager
 * @brief 错误码管理器，提供错误码到字符串的映射
 */
class ErrorCodeManager {
private:
    // 错误码到描述的映射表
    static unordered_map<int, string> error_descriptions;
    static unordered_map<int, string> error_categories;

    // 单例实例
    static ErrorCodeManager* instance;

    // 构造函数
    ErrorCodeManager() {
        initializeErrorCodes();
    }

    // 初始化错误码映射
    void initializeErrorCodes();

public:
    // 获取单例实例
    static ErrorCodeManager* getInstance() {
        if (instance == nullptr) {
            instance = new ErrorCodeManager();
        }
        return instance;
    }

    // 获取错误码的描述
    static string getDescription(int error_code);

    // 获取错误码的分类
    static string getCategory(int error_code);

    // 获取完整的错误信息（包括分类和描述）
    static string getFullMessage(int error_code);

    // 销毁单例
    static void destroy() {
        if (instance != nullptr) {
            delete instance;
            instance = nullptr;
        }
    }
};

