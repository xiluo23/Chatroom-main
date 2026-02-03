#include"epoll_client.h"


ClientState cur_state=state_connect;
int clint_fd,epoll_fd,str_len,send_len,total_len,recv_len,pipe_fd[2];//pipe_fd[0]:读,pipe_fd[1]:写
char username[512];
char password[512];
ClientBuffer client_recv_buffer;  // 接收缓冲区

struct epoll_event event,events[EVENTS_NUM];
pthread_t t_id,hb_tid;
bool heartbeat_started=false;  // 标志心跳线程是否已启动

int set_unblocking(int fd){
    int flag=fcntl(fd,F_GETFL);
    if(flag==-1){
        perror("Error:");
        return 0;
    }
    if(fcntl(fd,F_SETFL,flag|O_NONBLOCK)==-1){
        perror("Error:");
        return 0;
    }
    return 1;
}
void sign_in(){
    cur_state=state_signin_username;
    printf("请输入用户名:\n");
}
void sign_in_resp(const char*code,const char*msg){
    if (strcmp(code, "1") == 0) {
        printf("登录成功\n");
        // 登录成功后启动心跳线程
        if(!heartbeat_started){
            pthread_create(&hb_tid,NULL,heartbeat_thread,&clint_fd);
            heartbeat_started=true;
        }
        puts("输入3查询在线用户");
        puts("输入4单播通信");
        puts("输入5多播通信");
        puts("输入6广播通信");
        puts("输入7查询历史记录");
        puts("输入q(Q)退出连接");
    } else {
        printf("登录失败：%s\n", msg);
        cur_state=state_menu;
        puts("输入1登录");
        puts("输入2注册");
        puts("输入q(Q)退出连接");
        username[0]=0;
        password[0]=0;
    }
    cur_state=state_menu;
}
void sign_up(){
    cur_state=state_signup_username;
    printf("请输入用户名:\n");
}
void sign_up_resp(const char*code,const char*msg){
    if (strcmp(code, "1") == 0) {
        printf("注册成功：%s\n", msg);
        sign_in();
    } else {
        printf("注册失败：%s\n", msg);
        cur_state=state_menu;
        puts("输入1登录");
        puts("输入2注册");
        puts("输入q(Q)退出连接");
        username[0]=0;
        password[0]=0;
    }
}
void show_online_user(){
    char msg[]="show_online_user";
    send_message(msg,strlen(msg));
}
void show_online_user_resp(const char*code,const char*msg){
    if(strcmp(code,"1")==0){
        printf("当前在线用户:\n");
        puts(msg);
    }
    else{
        printf("请重试\n");
    }
    cur_state=state_menu;
}
void single_chat(){
    if(cur_state==state_single_chat_user){
        puts("请输入要与之进行通信的用户名：");
    }
    else if(cur_state==state_single_chat_text){
        puts("请输入文本");
    }
}
void show_history(char* code,char*msg){
    if(strcmp(code,"1")==0){
        puts("发送者  接收者  时间     类型    内容");
        puts(msg);
    }
    else{
        puts("请重试");
    }
    cur_state=state_menu;
}
void*handle_stdin(void*argv){//副线程处理用户输入数据，并通过管道发送给主线程
    char buf[BUF_SIZE];
    while(fgets(buf,sizeof(buf),stdin)){
        int ret=write(pipe_fd[1],buf,(size_t)strlen(buf));
        if(ret==-1){
            perror("handle_stdin");
            break;
        }
    }
    return NULL;
}
bool recv_message(){
    // 接收新数据并填充缓冲区
    int bytes_read;
    char temp_buf[BUF_SIZE];
    
    while(1){
        bytes_read = recv(clint_fd, temp_buf, BUF_SIZE, 0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break;  // 无数据可读
            }
            else{
                perror("Error:");
                return false;
            }
        }
        else if(bytes_read == 0){
            return false;  // 连接关闭
        }
        else{
            // 将新数据追加到缓冲区
            if(client_recv_buffer.pos + bytes_read <= PROTOCOL_MAX_TOTAL_SIZE){
                memcpy(client_recv_buffer.buffer + client_recv_buffer.pos, temp_buf, bytes_read);
                client_recv_buffer.pos += bytes_read;
            }
            else{
                perror("Buffer overflow");
                return false;
            }
        }
    }
    
    // 从缓冲区中提取完整的消息
    string message;
    while(true){
        int consumed = extractMessage(client_recv_buffer.buffer, client_recv_buffer.pos, message);
        if(consumed == -1){
            // 消息不完整，等待更多数据
            break;
        }
        else if(consumed == -2){
            // 消息长度无效，清理缓冲区的前4个字节并重新尝试
            printf("Warning: Invalid message detected, cleaning buffer\n");
            if(client_recv_buffer.pos > 1){
                // 移除第一个字节，尝试重新同步
                memmove(client_recv_buffer.buffer, client_recv_buffer.buffer + 1, client_recv_buffer.pos - 1);
                client_recv_buffer.pos -= 1;
            }
            else{
                // 缓冲区太小，清空它
                client_recv_buffer.pos = 0;
                break;
            }
        }
        else if(consumed == 0){
            // 缓冲区为空
            break;
        }
        else{
            // 成功提取一个完整消息
            handle_server_message(message.c_str());
            
            // 从缓冲区中移除已处理的数据
            memmove(client_recv_buffer.buffer, client_recv_buffer.buffer + consumed, 
                   client_recv_buffer.pos - consumed);
            client_recv_buffer.pos -= consumed;
        }
    }
    
    return true;
}
bool send_message(const char buf[],int len){
    // 使用协议编码消息
    // cout<<"编码后消息："<<encodeMessage(string(buf,len))<<endl;
    return sendMessage(clint_fd, string(buf, len));
}

bool handle_pipe_input(){
    char buf[BUF_SIZE];
    total_len=0;
    while(1){
        str_len=read(pipe_fd[0],buf+total_len,BUF_SIZE-total_len-1);
        if(str_len>0){
            total_len+=str_len;
        }
        else if(str_len==-1){
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                break;
            }
            else{
                return false;
            }
        }
        else{
            return false;
        }
    }
    if(total_len==0)return true;
    buf[total_len]=0;
    //状态机处理输入
    char msg[BUF_SIZE];
    memset(msg,0,sizeof(msg));
    if(strcmp(buf,"q\n")==0||strcmp(buf,"Q\n")==0){
        buf[strcspn(buf,"\n")]=0;
        send_message(buf,strlen(buf));
        return false;
    }
    switch(cur_state){
        case state_menu:
            if(strcmp(buf,"1\n")==0){
                sign_in();
            }
            else if(strcmp(buf,"2\n")==0){
                sign_up();
            }
            else if(strcmp(buf,"3\n")==0){
                show_online_user();
                cur_state=state_wait_resp;
            }
            else if(strcmp(buf,"4\n")==0){
                cur_state=state_single_chat_user;
                single_chat();
            }
            else if(strcmp(buf,"5\n")==0){
                cur_state=state_multi_chat_user;
                puts("请输入进行多播通信的用户名");
                memset(username,0,sizeof(username));
            }
            else if(strcmp(buf,"6\n")==0){
                cur_state=state_broadcast_chat_msg;
                puts("请输入要发送的信息");
            }
            else if(strcmp(buf,"7\n")==0){
                const char*msg="show_history";
                send_message(msg,strlen(msg));
                cur_state=state_wait_resp;
            }
            break;
        case state_broadcast_chat_msg:
            buf[strchr(buf,'\n')-buf]=0;
            snprintf(msg,BUF_SIZE-1,"broadcast_chat|%s",buf);
            send_message(msg,strlen(msg));
            cur_state=state_wait_resp;
            break;
        case state_multi_chat_user:
            strncpy(username,buf,strlen(buf)-1);//去掉最后的 \n
            username[strlen(username)]=0;
            cur_state=state_multi_chat_msg;
            puts("请输入要发送的信息");
            break;
        case state_multi_chat_msg:
            buf[strchr(buf,'\n')-buf]=0;
            snprintf(msg,BUF_SIZE-1,"multi_chat|%s|%s",username,buf);
            send_message(msg,strlen(msg));
            cur_state=state_wait_resp;
            break;
        case state_single_chat_user:
            cur_state=state_single_chat_text;
            single_chat();
            strncpy(username,buf,strlen(buf)-1);//去掉最后的 \n
            username[strlen(username)]=0;
            break;
        case state_single_chat_text:
            buf[strlen(buf)-1]=0;
            // printf("user_name:%s,text:%s\n",username,buf);
            snprintf(msg,BUF_SIZE,"single_chat|%s|%s",username,buf);
            send_message(msg,strlen(msg));
            // puts(msg);
            memset(username,0,sizeof(username));
            memset(password,0,sizeof(password));
            cur_state=state_wait_resp;
            break;
        case state_signup_username:
            strcpy(username,buf);
            username[strcspn(username,"\n")]=0;
            cur_state=state_signup_password;
            printf("请输入密码:\n");
            break;
        case state_signup_password:
            strcpy(password, buf);
            password[strcspn(password, "\n")] = 0;
            snprintf(msg,BUF_SIZE,"sign_up|%s|%s",username,password);
            send_message(msg,strlen(msg));
            memset(username,0,sizeof(username));
            memset(password,0,sizeof(password));
            cur_state=state_wait_resp;
            puts("正在注册中，请稍等...");
            break;
        case state_wait_resp:
            puts("请等待服务器响应...");
            break;
        case state_signin_username:
            strcpy(username,buf);
            username[strcspn(username,"\n")]=0;
            cur_state=state_signin_password;
            printf("请输入密码:\n");
            break;
        case state_signin_password:
            strcpy(password, buf);
            password[strcspn(password, "\n")] = 0;
            snprintf(msg,BUF_SIZE,"sign_in|%s|%s",username,password);
            send_message(msg,strlen(msg));
            memset(username,0,sizeof(username));
            memset(password,0,sizeof(password));
            cur_state=state_wait_resp;
            puts("正在登录中，请稍等...");
            break;
        default:
            puts("无效输入，请重试");
            break;
    }
    
    return true;
}
void single_chat_resp(const char* code,char*msg){//singe_chat|1|Alice;abccc
    if(strcmp(code,"1")==0){
        char*from=strtok(msg,";");
        if(!from)return ;
        char*text=strtok(NULL,";");
        if(!text)return ;
        printf("收到%s的一条消息:%s\n",from,text);
    }
    else{
        puts(msg);
        cur_state=state_menu;
    }
}
void multi_chat_resp(char*code,char*msg){
    if(strcmp(code,"1")==0){
        puts("发送成功");
        cur_state=state_menu;
    }
    else if(strcmp(code,"2")==0){
        char*from=strtok(msg,";");
        char*text=strtok(NULL,";");
        printf("收到%s发送的信息:%s\n",from,text);
    }
    else{
        puts("请重试");
        cur_state=state_menu;
    }
}
void broadcast_chat_resp(char*code,char*msg){
    if(strcmp(code,"1")==0){
        char*from=strtok(msg,";");
        char*text=strtok(NULL,";");
        printf("收到%s发送的信息:%s\n",from,text);
    }
    else if(strcmp(code,"2")==0){
        puts("发送成功");
        cur_state=state_menu;
    }
    else{
        puts("请重试");
        cur_state=state_menu;
    }
}
void handle_server_message(const char*msg){//eg:sign_up|0|注册成功
    // 使用安全拷贝防止堆栈溢出：历史记录等消息可能远大于 BUF_SIZE
    char tmp[BUF_SIZE];
    // 只拷贝最多 BUF_SIZE-1 个字节，并手动补 '\0'
    strncpy(tmp, msg, BUF_SIZE - 1);
    tmp[BUF_SIZE - 1] = '\0';

    char*type=strtok(tmp,"|");
    char*code=strtok(NULL,"|");
    char*text=strtok(NULL,"|");
    if(!type||!code)return ;
    if(!text)text=(char*)"";
    // puts(type);
    if(strcmp(type,"sign_up")==0){
        sign_up_resp(code,text);
    }
    else if(strcmp(type,"sign_in")==0){
        sign_in_resp(code,text);
    }
    else if(strcmp(type,"single_chat")==0){
        single_chat_resp(code,text);
    }
    else if(strcmp(type,"show_online_user")==0){
        show_online_user_resp(code,text);
    }
    else if(strcmp(type,"chat_unread")==0){   //接收未读消息
        puts("收到未读消息\n来自   发送时间   内容");
        puts(text);
    }
    else if(strcmp(type,"show_history")==0){
        show_history(code,text);
    }
    else if(strcmp(type,"multi_chat")==0){
        multi_chat_resp(code,text);
    }
    else if(strcmp(type,"broadcast_chat")==0){
        broadcast_chat_resp(code,text);
    }
    else if(strcmp(type,"heartbeat")==0){
        if(strcmp(code,"1")==0){
            // printf("[DEBUG] 心跳成功\n");  // 可选：不输出以减少刷屏
        }
        else{
            printf("心跳检测失败: %s\n",text);
        }
    }
    else if(strcmp(type,"bye")==0){
        printf("服务器主动断开连接: %s\n",text);
        cur_state=state_connect;
    }
}
bool clint_init(){
    if((clint_fd=socket(PF_INET,SOCK_STREAM,0))==-1){
        perror("clint_init:");
        return false;
    }
    set_unblocking(clint_fd);
    return true;
}
bool connect_ser(int argc,char*argv[]){
    if(argc!=3){
        cerr<<"Usage: "<<argv[0]<<" <IP> <PORT>\n";
        exit(0);
    }
    struct sockaddr_in serv_addr;
    memset(&serv_addr,0,sizeof(serv_addr));
    socklen_t serv_size;
    serv_size=sizeof(serv_addr);
    serv_addr.sin_family=AF_INET;
    serv_addr.sin_port=htons(atoi(argv[2]));
    serv_addr.sin_addr.s_addr=inet_addr(argv[1]);
    int ret=connect(clint_fd,(struct sockaddr*)&serv_addr,serv_size);
    if(ret==-1){
        if(errno!=EINPROGRESS){
            perror("connect:");
            return false;
        }
    }
    return true;
}
bool epoll_init(){
    epoll_fd=epoll_create(1);
    if(epoll_fd==-1){
        perror("epoll_init:");
        return false;
    } 
    event.data.fd=pipe_fd[0];
    event.events=EPOLLIN|EPOLLET;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,pipe_fd[0],&event)==-1){
        perror("epoll_init:");
        return false;
    }
    event.data.fd=clint_fd;
    event.events=EPOLLIN|EPOLLET|EPOLLOUT;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,clint_fd,&event)==-1){
        perror("epoll_init:");
        return false;
    }
    return true;
}
bool pipe_init(){
    if(pipe(pipe_fd)==-1){
        perror("pipe");
        return false;
    }
    set_unblocking(pipe_fd[0]);
    return true;
}
void finish(){
    puts("已断开连接");
    pthread_cancel(t_id);
    pthread_join(t_id,NULL);
    pthread_cancel(hb_tid);
    pthread_join(hb_tid,NULL);
    close(clint_fd);
    close(epoll_fd);
    close(pipe_fd[0]);
    close(pipe_fd[1]);
}
//用于心跳检测
void*heartbeat_thread(void*arg){
    int sockfd=*(int*)arg;
    const char*msg="heartbeat";
    while(1){
        sleep(15);  // 每15秒发送一次心跳，保活连接（小于30秒超时）
        if(send_message(msg,strlen(msg))==false){
            printf("心跳发送失败，连接已断开\n");
            break;
        }
    }
    return NULL;
}


int main(int argc,char*argv[]){
    if(pipe_init()==false||clint_init()==false||epoll_init()==false||connect_ser(argc,argv)==false){
        finish();
        exit(1);
    }
    pthread_create(&t_id,NULL,handle_stdin,NULL);
    // 不再在这里创建心跳线程，而是在登录成功后创建
    bool running=true;
    while(running){
        int event_num=epoll_wait(epoll_fd,events,EVENTS_NUM,-1);
        if(event_num==-1){
            if(errno==EINTR)continue;
            perror("Event_num:");
            break;
        }
        for(int i=0;i<event_num&&running;i++){
            int ev_fd=events[i].data.fd;
            if(events[i].events&(EPOLLHUP|EPOLLERR)){//连接出现问题
                perror("Error: ");
                running=false;
                break;
            }
            else if(events[i].events&EPOLLOUT){//建立连接
                int err;
                socklen_t len = sizeof(err);
                if (getsockopt(clint_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                    perror("connect failed");
                    running = false;
                } else {
                    event.data.fd=clint_fd;
                    event.events=EPOLLIN|EPOLLET;
                    epoll_ctl(epoll_fd,EPOLL_CTL_MOD,clint_fd,&event);
                    puts("已连接到epoll服务器");
                    cur_state=state_menu;//修改状态
                    puts("输入1登录");
                    puts("输入2注册");
                    puts("输入q(Q)退出连接");
                }
            }
            else if(ev_fd==clint_fd&&(events[i].events&EPOLLIN)){//服务器有数据发来
                running=recv_message();
            }
            else if(ev_fd==pipe_fd[0]){//用户有数据输入,从管道中取数据,并对数据进行处理
                running=handle_pipe_input();
        
            }
        }
    }
    finish();
    return 0;
}
