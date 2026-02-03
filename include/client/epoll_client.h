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
#include<pthread.h>
#include<stdlib.h>
#include"protocol.h"

#define IP "127.0.0.1"
#define PORT 8080
#define BUF_SIZE 4096
#define EVENTS_NUM 10

// 客户端接收缓冲区（用于处理粘包/拆包）
struct ClientBuffer {
    char buffer[PROTOCOL_MAX_TOTAL_SIZE];
    int pos;  // 当前缓冲区中的数据长度
    
    ClientBuffer() : pos(0) {
        memset(buffer, 0, sizeof(buffer));
    }
};

typedef enum{
    state_connect,
    state_menu,
    state_signup_username,
    state_signup_password,
    state_signin_username,
    state_signin_password,
    state_wait_resp,
    state_single_chat_user,
    state_single_chat_text,
    state_multi_chat_user,
    state_multi_chat_msg,
    state_broadcast_chat_msg,
    state_show_history
}ClientState;

int set_unblocking(int fd);
void sign_in();
void sign_in_resp(const char*code,const char*msg);
void sign_up();
void sign_in_resp(const char *code, const char *msg);
void sign_up_resp(const char*code,const char*msg);
void show_online_user();
void single_chat();
void multi_chat();
bool recv_message();
bool send_message(const char buf[],int len);
bool handle_pipe_input();
void handle_server_message(const char*msg);
void*heartbeat_thread(void*arg);