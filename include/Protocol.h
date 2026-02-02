#pragma once 
/**
 * @file Protocol.h
 * @brief 网络通信协议定义 - 处理粘包和拆包
 * 
 * 协议格式:
 * [长度(4字节)][数据(N字节)]
 * 
 * 长度字段: 4个字节的大端序整数，表示后续数据的字节数
 * 数据字段: 实际的消息内容（可以是任何格式）
 * 
 * 例如:
 * 消息 "sign_up|user|pass" (16个字节)
 * 协议格式: 0x00 0x00 0x00 0x10 sign_up|user|pass
 */

#include <cstring>
#include <cstdint>
#include <string>
#include <iostream>

using namespace std;

// ==================== 协议常量 ====================
#define PROTOCOL_HEADER_SIZE 4      // 头部大小（4字节长度字段）
#define PROTOCOL_MAX_MESSAGE_SIZE 4096  // 单条消息最大大小 (4KB)
#define PROTOCOL_MAX_TOTAL_SIZE (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_MESSAGE_SIZE)  // 总大小

// ==================== 协议函数 ====================

/**
 * @brief 将消息按照协议格式编码
 * @param message 要发送的消息（不包括长度前缀）
 * @return 编码后的消息（包括4字节长度前缀）
 * 
 * 例如:
 * input:  "sign_up|user|pass"
 * output: "\0\0\0\x10sign_up|user|pass"
 */
inline string encodeMessage(const string& message) {
    // 消息长度（4字节，大端序）
    uint32_t msg_len = htonl(message.length());
    
    // 创建编码后的消息
    string encoded;
    encoded.resize(PROTOCOL_HEADER_SIZE + message.length());
    
    // 复制长度字段（大端序）
    memcpy(encoded.data(), &msg_len, PROTOCOL_HEADER_SIZE);
    
    // 复制数据字段
    memcpy(encoded.data() + PROTOCOL_HEADER_SIZE, message.data(), message.length());
    // cout<<"编码信息："<<encoded<<endl;
    return encoded;
}

/**
 * @brief 从接收缓冲区中提取一条完整的消息
 * @param buffer 接收缓冲区
 * @param buffer_len 缓冲区中已有数据的长度
 * @param message 存储提取的消息（不包括长度前缀）
 * @return -1: 数据不完整，需要继续接收; >=0: 消费的字节数
 * 
 * 使用例:
 * while (true) {
 *     int consumed = extractMessage(buffer, len, msg);
 *     if (consumed < 0) {
 *         // 需要继续接收
 *         break;
 *     }
 *     // 处理msg...
 *     
 *     // 移动缓冲区
 *     len -= consumed;
 *     memmove(buffer, buffer + consumed, len);
 * }
 */
inline int extractMessage(const char* buffer, size_t buffer_len, string& message) {
    // 检查是否有完整的头部
    if (buffer_len < PROTOCOL_HEADER_SIZE) {
        return -1;  // 头部不完整
    }
    
    // 读取消息长度（大端序）
    uint32_t msg_len;
    memcpy(&msg_len, buffer, PROTOCOL_HEADER_SIZE);
    msg_len = ntohl(msg_len);
    
    // 验证消息长度的有效性
    if (msg_len == 0 || msg_len > PROTOCOL_MAX_MESSAGE_SIZE) {
        cerr << "Error: Invalid message length: " << msg_len << endl;
        return -2;  // 消息长度无效
    }
    
    // 检查是否有完整的消息
    size_t total_needed = PROTOCOL_HEADER_SIZE + msg_len;
    if (buffer_len < total_needed) {
        return -1;  // 消息不完整，需要继续接收
    }
    
    // 提取消息内容
    message.assign(buffer + PROTOCOL_HEADER_SIZE, msg_len);
    
    // 返回消费的字节数
    return total_needed;
}

/**
 * @brief 发送编码后的消息
 * @param fd 套接字文件描述符
 * @param message 消息内容（不包括长度前缀）
 * @return 成功返回true，失败返回false
 * 
 * 这个函数会自动编码消息并发送
 */
inline bool sendMessage(int fd, const string& message) {
    string encoded = encodeMessage(message);
    
    size_t total = 0;
    while (total < encoded.length()) {
        int n = send(fd, encoded.c_str() + total, encoded.length() - total, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else {
                cerr << "Error sending message: " << strerror(errno) << endl;
                return false;
            }
        }
        total += n;
    }
    
    return true;
}

/**
 * @brief 接收一条完整的消息
 * @param fd 套接字文件描述符
 * @param message 存储接收的消息
 * @param buffer 缓冲区（用于保存不完整的数据）
 * @param buffer_pos 缓冲区当前位置
 * @return 1: 接收到完整消息; 0: 连接关闭; -1: 需要继续接收; -2: 错误
 * 
 * 使用例:
 * char buffer[4096];
 * int pos = 0;
 * string msg;
 * 
 * while (true) {
 *     int ret = receiveMessage(fd, msg, buffer, pos);
 *     if (ret == 1) {
 *         // 处理消息
 *     } else if (ret == -1) {
 *         // 继续接收
 *     } else {
 *         // 错误或连接关闭
 *     }
 * }
 */
inline int receiveMessage(int fd, string& message, char* buffer, int& buffer_pos) {
    // 尝试从缓冲区提取消息
    int consumed = extractMessage(buffer, buffer_pos, message);
    
    if (consumed > 0) {
        // 提取成功，移动缓冲区
        memmove(buffer, buffer + consumed, buffer_pos - consumed);
        buffer_pos -= consumed;
        return 1;  // 接收到完整消息
    }
    
    if (consumed == -2) {
        return -2;  // 消息长度无效
    }
    
    // 需要接收更多数据
    int n = recv(fd, buffer + buffer_pos, PROTOCOL_MAX_TOTAL_SIZE - buffer_pos, 0);
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;  // 继续接收
        } else {
            cerr << "Error receiving message: " << strerror(errno) << endl;
            return -2;  // 错误
        }
    }
    
    if (n == 0) {
        return 0;  // 连接关闭
    }
    
    buffer_pos += n;
    
    // 再次尝试提取消息
    consumed = extractMessage(buffer, buffer_pos, message);
    
    if (consumed > 0) {
        // 提取成功
        memmove(buffer, buffer + consumed, buffer_pos - consumed);
        buffer_pos -= consumed;
        return 1;  // 接收到完整消息
    }
    
    return -1;  // 消息不完整，需要继续接收
}
