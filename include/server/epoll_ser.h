#pragma once 
#include<stdio.h>
#include<iostream>
#include<vector>
#include<string.h>
#include<sys/socket.h>
#include<sys/epoll.h>
#include<netinet/in.h>
#include<errno.h>
#include<arpa/inet.h>
#include<fcntl.h>
#include<unistd.h>
#include<unordered_set>
#include<unordered_map>
#include<pthread.h>
#include"MyDb.h"
#include"Protocol.h"
#include<queue>
#include<crypt.h>
#include<sys/eventfd.h>
#include<map>
#define BUF_SIZE 4096      // 接收缓冲区大小（4KB，对齐协议最大消息）
#define CLINT_SIZE 10000
#define MAX_EVENTS 4096
#define PORT 3306
#define HOST "192.168.147.130"
#define USER "ftpuser"
#define DB_NAME "Chatroom"
#define PWD "926472"

struct Task;

struct Response{
    int fd;
    string out;
    bool close_after;
};

// 客户端接收缓冲区（用于处理粘包/拆包）
struct ClientBuffer {
    char buffer[PROTOCOL_MAX_TOTAL_SIZE];
    int pos;  // 当前缓冲区中的数据长度
    
    ClientBuffer() : pos(0) {
        memset(buffer, 0, sizeof(buffer));
    }
};

// 保护 clint_nametofd / clint_fdtoname 的互斥锁（多线程访问）
extern pthread_mutex_t client_map_mutex;

int server_init();//服务器初始化
int set_unblocking(int fd);//为ET触发，设置非阻塞式i/o
void handle_new_connect();//与客户端建立连接
void handle_clint_data(int epoll_fd,int clint_fd);//接受并处理客户端数据
void close_clint(int epoll_fd,int clint_fd);
void process_clint_data(Task&task);
bool send_message(int clint_fd,const char buf[],int len);
void handle_response();
void signal_event_fd();
void en_resp(char*msg,int clint_fd);
void process_clint_data(Task &task);








