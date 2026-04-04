#pragma once 
/**
 * @file Protocol.h
 * @brief 网络通信协议定义 - 处理粘包和拆包
 * 
 * 协议格式:
 * [长度(4字节)][数据(N字节)]
 */

#include <cstring>
#include <cstdint>
#include <string>
#include <iostream>
#include <pthread.h>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>

using namespace std;

// ==================== 协议常量 ====================
#define PROTOCOL_HEADER_SIZE 4      // 头部大小（4字节长度字段）
#define PROTOCOL_MAX_MESSAGE_SIZE 65536  // 单条消息最大大小 (64KB)
#define PROTOCOL_MAX_TOTAL_SIZE (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_MESSAGE_SIZE)  // 单条完整消息最大总大小
#define PROTOCOL_MAX_RECV_BUFFER_SIZE 262144 // 接收缓冲区总大小 (256KB)，应对粘包与突发流量

// ==================== 协议函数 ====================

inline string encodeMessage(const string& message) {
    uint32_t msg_len = htonl(message.length());
    string encoded;
    encoded.resize(PROTOCOL_HEADER_SIZE + message.length());
    memcpy(encoded.data(), &msg_len, PROTOCOL_HEADER_SIZE);
    memcpy(encoded.data() + PROTOCOL_HEADER_SIZE, message.data(), message.length());
    return encoded;
}

inline int extractMessage(const char* buffer, size_t buffer_len, string& message) {
    if (buffer_len < PROTOCOL_HEADER_SIZE) {
        return -1;
    }
    uint32_t msg_len;
    memcpy(&msg_len, buffer, PROTOCOL_HEADER_SIZE);
    msg_len = ntohl(msg_len);
    if (msg_len == 0 || msg_len > PROTOCOL_MAX_MESSAGE_SIZE) {
        return -2;
    }
    size_t total_needed = PROTOCOL_HEADER_SIZE + msg_len;
    if (buffer_len < total_needed) {
        return -1;
    }
    message.assign(buffer + PROTOCOL_HEADER_SIZE, msg_len);
    return (int)total_needed;
}

inline bool sendMessage(int fd, const string& message) {
    string encoded = encodeMessage(message);
    size_t total = 0;
    int retry_count = 0;
    const int max_retries = 5;

    while (total < encoded.length()) {
        ssize_t n = send(fd, encoded.c_str() + total, encoded.length() - total, 0);
        if (n > 0) {
            total += n;
            retry_count = 0; // 成功写入一些数据，重置重试计数
        } else if (n == -1) {
            if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 内核发送缓冲区满，在高并发 QPS 测试时很常见
                if (++retry_count > max_retries) {
                    // 超过重试次数，可能对端接收太慢或连接异常
                    return false;
                }
                // 短暂让出 CPU，等待内核缓冲区可用
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            } else {
                // 其他错误（如 EPIPE, ECONNRESET），连接已断开
                return false;
            }
        } else {
            // n == 0，通常不应该发生，除非连接被关闭
            return false;
        }
    }
    return true;
}

inline int receiveMessage(int fd, string& message, char* buffer, int& buffer_pos) {
    int n = recv(fd, buffer + buffer_pos, PROTOCOL_MAX_RECV_BUFFER_SIZE - buffer_pos, 0);
    if (n <= 0) return n;
    buffer_pos += n;
    int consumed = extractMessage(buffer, buffer_pos, message);
    if (consumed > 0) {
        memmove(buffer, buffer + consumed, buffer_pos - consumed);
        buffer_pos -= consumed;
        return 1;
    }
    return -1;
}

// 环形缓冲区版本的 extractMessage
inline int extractMessageCircular(const char* buffer, size_t buffer_size, size_t& read_pos, string& message) {
    size_t current_size = (buffer_size - read_pos + buffer_size) % buffer_size; // 假设 tail 是 buffer_size，但需要传递 tail
    // 实际上，需要传递 tail
    // 重新定义函数签名
    // 为了简单，假设 buffer_size 是 PROTOCOL_MAX_RECV_BUFFER_SIZE，read_pos 是 head，tail 是全局或传递
    // 简化：传递 head 和 tail
    return current_size < PROTOCOL_HEADER_SIZE ? -1 : 0; // 先检查是否有足够数据读取长度字段
}

inline int extractMessageCircular(const char* buffer, size_t buffer_size, size_t& head, size_t tail, string& message) {
    size_t current_size = (tail - head + buffer_size) % buffer_size;
    if (current_size < PROTOCOL_HEADER_SIZE) {
        return -1;  // 数据不完整
    }
    
    // 读取消息长度（大端序）
    uint32_t msg_len;
    size_t header_pos = head;
    if (header_pos + PROTOCOL_HEADER_SIZE <= buffer_size) {
        memcpy(&msg_len, buffer + header_pos, PROTOCOL_HEADER_SIZE);
    } else {
        // 跨越边界
        size_t first = buffer_size - header_pos;
        char temp[4];
        memcpy(temp, buffer + header_pos, first);
        memcpy(temp + first, buffer, PROTOCOL_HEADER_SIZE - first);
        memcpy(&msg_len, temp, 4);
    }
    msg_len = ntohl(msg_len);
    
    // 验证消息长度的有效性
    if (msg_len == 0 || msg_len > PROTOCOL_MAX_MESSAGE_SIZE) {
        return -2;  // 消息长度无效
    }
    
    // 检查是否有完整的消息
    size_t total_needed = PROTOCOL_HEADER_SIZE + msg_len;
    if (current_size < total_needed) {
        return -1;  // 消息不完整
    }
    
    // 提取消息内容
    message.resize(msg_len);
    size_t data_pos = (head + PROTOCOL_HEADER_SIZE) % buffer_size;
    size_t to_copy = msg_len;
    size_t first = min(to_copy, buffer_size - data_pos);
    memcpy(&message[0], buffer + data_pos, first);
    if (to_copy > first) {
        memcpy(&message[first], buffer, to_copy - first);
    }
    
    // 更新 head
    head = (head + total_needed) % buffer_size;
    
    // 返回消费的字节数
    return total_needed;
}
