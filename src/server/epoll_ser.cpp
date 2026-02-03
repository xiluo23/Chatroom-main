#include"epoll_ser.h"
#include"MyDb.h"
#include"Logger.h"
#include"ErrorCode.h"
#include<functional>
#include <unordered_set>
#include<signal.h>
#include <sys/resource.h>
#include"redis.h"
using namespace std;
unordered_map<string,int>clint_nametofd;
unordered_map<int,string>clint_fdtoname;
pthread_mutex_t resp_mutex;
pthread_mutex_t client_map_mutex; // 在头文件中声明为 extern
pthread_mutex_t crypt_mutex; // 保护 crypt 函数的互斥锁
ThreadPool pool(32);  // 增加到32个连接以应对高并发
Redis redis_;
int ser_fd,epoll_fd;
int event_fd=eventfd(0,EFD_NONBLOCK);
queue<Response>resp_queue;
map<int, ClientBuffer> client_buffers;  // 为每个客户端维护接收缓冲区
pthread_mutex_t buffer_map_mutex;  // 保护 client_buffers 的互斥锁

// 专用于 close_clint 的全局数据库连接及互斥锁
MyDb g_close_conn;
pthread_mutex_t g_close_conn_mutex;

// Parser 线程管理（负责粘包/拆包，reactor 只收数据）
pthread_mutex_t parser_mutex;                         // 保护 parser_pending_set
pthread_cond_t parser_cond;
unordered_set<int> parser_pending_set;                // 待解析的客户端集合（去重）

// Redis 订阅管理
pthread_mutex_t redis_mutex;                         // 保护 redis 操作（publish/subscribe/unsubscribe）
unordered_set<int> redis_subscribed_channels;        // 当前已订阅的用户频道集合

// SIGINT 标志位
volatile sig_atomic_t stop_server = 0;

// SIGINT 信号处理函数：仅设置标志并唤醒 epoll
void handle_sigint(int signo){
    (void)signo;
    stop_server = 1;
    // 通过 event_fd 唤醒 epoll_wait，避免长时间阻塞
    uint64_t one = 1;
    write(event_fd,&one,sizeof(one));
}
// SQL 字符串转义，防止单引号导致的语句错误
static string escape_sql(const string &s) {
    string res;
    res.reserve(s.size());
    for (char c : s) {
        if (c == '\'') res += "''"; else res += c;
    }
    return res;
} 
bool send_message(int clint_fd,const char buf[],int len){
    return sendMessage(clint_fd, string(buf, len));
}
string generate_str(){//生成salt，使用MD5
    string str="";
    int i,flag;
    for(i=0;i<8;i++){
        flag=rand()%3;
        switch (flag){
            case 0:
                str+=rand()%26+'a';
                break;
            case 1:
                str+=rand()%26+'A';
                break;
            case 2:
                str+=rand()%10+'0';
                break;
        }
    }
    return str;
}
int server_init(int argc,char*argv[]){
    if(argc!=3){
        cerr<<"Usage: "<<argv[0]<<" <port> <thread_num>\n";
        exit(0);
    }
    struct sockaddr_in ser_addr;
    if((ser_fd=socket(PF_INET,SOCK_STREAM,0))==-1){
        LOG_ERROR("Socket creation failed",ERR_SOCKET_CREATE_FAIL);
        exit(0);
    }
    memset(&ser_addr,0,sizeof(ser_addr));
    ser_addr.sin_family=AF_INET;
    //127.0.0.1 6000
    ser_addr.sin_port=htons(atoi(argv[2]));
    ser_addr.sin_addr.s_addr=htonl(INADDR_ANY);
    int opt=1;
    if(setsockopt(ser_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt))==-1){
        LOG_ERROR("Socket setopt failed",ERR_SOCKET_SETOPT_FAIL);
        close(ser_fd);
        exit(0);
    }
    if(bind(ser_fd,(struct sockaddr*)&ser_addr,sizeof(ser_addr))==-1){
        LOG_ERROR("Socket bind failed",ERR_SOCKET_BIND_FAIL);
        close(ser_fd);
        exit(0);
    }
    if(listen(ser_fd,2048)==-1){
        LOG_ERROR("Socket listen failed",ERR_SOCKET_LISTEN_FAIL);
        close(ser_fd);
        exit(0);
    }
    // 优化套接字缓冲区大小，支持高并发
    int sndbuf = 256 * 1024;  // 发送缓冲区256KB
    int rcvbuf = 256 * 1024;  // 接收缓冲区256KB
    if(setsockopt(ser_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == -1){
        LOG_WARN("Failed to set SO_SNDBUF");
    }
    if(setsockopt(ser_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == -1){
        LOG_WARN("Failed to set SO_RCVBUF");
    }
    LOG_INFO("Server socket buffers configured: SNDbuf="+to_string(sndbuf)+", RCVbuf="+to_string(rcvbuf));
    // puts("server is running");
    LOG_INFO("Server starting");
    return ser_fd;
}
int set_unblocking(int fd){
    int flag=fcntl(fd,F_GETFL);
    if(flag==-1){
        LOG_ERROR("Get F_GETFL failed",ERR_CONFIG_INVALID);
        return 0;
    }
    if(fcntl(fd,F_SETFL,flag|O_NONBLOCK)==-1){
        LOG_ERROR("Set unblocking failed",ERR_CONFIG_INVALID);
        return 0;
    }
    return 1;
}
void handle_new_connect(){
    socklen_t clint_size;
    int clint_fd;
    struct sockaddr_in clint_addr;
    struct epoll_event event;
    clint_size=sizeof(clint_addr);
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    while(1){
        clint_size=sizeof(clint_addr);
        clint_fd=accept(ser_fd,(struct sockaddr*)&clint_addr,&clint_size);
        if(clint_fd==-1){
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                break;
            }
            else{
                LOG_ERROR("Accept failed",ERR_SOCKET_ACCEPT_FAIL);
                break;
            }
        }
        if(set_unblocking(clint_fd)==0){
            close(clint_fd);
            continue;
        }
        event.data.fd=clint_fd;
        event.events=EPOLLIN|EPOLLRDHUP;
        if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,clint_fd,&event)==-1){
            LOG_ERROR("Epoll_ctl failed",ERR_EPOLL_CTL_FAIL);
            close(clint_fd);
            break;
        }
        // // 启用TCP_NODELAY，禁用Nagle算法以降低延迟
        // int nodelay = 1;
        // if(setsockopt(clint_fd, IPPROTO_TCP, O_NDELAY, &nodelay, sizeof(nodelay)) == -1){
        //     LOG_WARN("Failed to set TCP_NODELAY for FD="+to_string(clint_fd));
        // }
        LOG_INFO("New client connected: FD="+to_string(clint_fd));
    }
}
void close_clint(int epoll_fd,int clint_fd){
    epoll_ctl(epoll_fd,EPOLL_CTL_DEL,clint_fd,NULL);
    close(clint_fd);
    LOG_INFO("Client disconnected: FD="+to_string(clint_fd));
    
    // 访问全局 map 前加锁，避免多线程竞争
    string username="";
    pthread_mutex_lock(&client_map_mutex);
    auto it_name = clint_fdtoname.find(clint_fd);
    if (it_name != clint_fdtoname.end()) {
        username = it_name->second;
        clint_nametofd.erase(it_name->second);
        clint_fdtoname.erase(it_name);
    }
    pthread_mutex_unlock(&client_map_mutex);

    // 清除该客户端的接收缓冲区
    pthread_mutex_lock(&buffer_map_mutex);
    client_buffers.erase(clint_fd);
    pthread_mutex_unlock(&buffer_map_mutex);

    if (!username.empty()) {
        // 使用全局数据库连接更新状态，需加锁
        pthread_mutex_lock(&g_close_conn_mutex);
        // 检查连接有效性
        if (!g_close_conn.ping()) {
            LOG_WARN("Global close connection lost, reconnecting...");
            // initDB 内部会先 mysql_close
            g_close_conn.initDB(HOST, USER, PWD, DB_NAME, 3306);
        }
        
        int uid = g_close_conn.get_id(username.c_str());
        if (uid != -1) {
            string sql = "update user_status set is_online = 0 where user_id ="+to_string(uid);
            g_close_conn.exeSQL(sql);
        }
        pthread_mutex_unlock(&g_close_conn_mutex);

        // Redis 退订（不需要 DB 锁，但需要 redis 锁）
        pthread_mutex_lock(&redis_mutex);
        if (redis_subscribed_channels.count(uid)) {
            if (redis_.unsubscribe(uid)) {
                redis_subscribed_channels.erase(uid);
                LOG_INFO("Unsubscribed Redis channel for user_id=" + to_string(uid));
            } else {
                LOG_WARN("Failed to unsubscribe Redis channel for user_id=" + to_string(uid));
            }
        }
        pthread_mutex_unlock(&redis_mutex);
    }
}
void handle_clint_data(int epoll_fd,int clint_fd){
    // 获取或创建客户端缓冲区
    pthread_mutex_lock(&buffer_map_mutex);
    if(client_buffers.find(clint_fd) == client_buffers.end()){
        client_buffers[clint_fd] = ClientBuffer();
    }
    ClientBuffer& client_buf = client_buffers[clint_fd];
    pthread_mutex_unlock(&buffer_map_mutex);
    
    // 接收新数据并填充缓冲区
    int bytes_read;
    char temp_buf[BUF_SIZE];
    while(1){
        bytes_read = recv(clint_fd, temp_buf, BUF_SIZE, 0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                // 无数据可读，跳出接收循环
                break;
            }
            else{
                LOG_NET_ERROR(clint_fd,"Failed to receive message",ERR_SOCKET_RECV_FAIL);
                close_clint(epoll_fd,clint_fd);
                return;
            }
        }
        else if(bytes_read == 0){
            // 客户端关闭连接
            LOG_INFO("Client closed connection: FD="+to_string(clint_fd));
            close_clint(epoll_fd,clint_fd);
            return;
        }
        else{
            // 将新数据追加到缓冲区
            if(client_buf.pos + bytes_read <= PROTOCOL_MAX_TOTAL_SIZE){
                memcpy(client_buf.buffer + client_buf.pos, temp_buf, bytes_read);
                client_buf.pos += bytes_read;
            }
            else{
                LOG_NET_ERROR(clint_fd,"Receive buffer overflow",ERR_SOCKET_RECV_FAIL);
                close_clint(epoll_fd,clint_fd);
                return;
            }
        }
    }
    
    // Reactor 只负责收数据并通知 parser 线程解析（parser 做粘包/拆包）
    pthread_mutex_lock(&parser_mutex);
    parser_pending_set.insert(clint_fd);
    pthread_cond_signal(&parser_cond);
    pthread_mutex_unlock(&parser_mutex);
    // 注意：parser 线程将会处理 client_buffers[clint_fd] 中的内容并把完整消息入队到线程池
}
void signal_event_fd(){
    uint64_t one=1;
    write(event_fd,&one,sizeof(one));
}

// Parser thread: 从 client_buffers 中解析出完整消息并把 Task 投入线程池
void* parser_thread_func(void* arg){
    while(1){
        pthread_mutex_lock(&parser_mutex);
        while(parser_pending_set.empty()){
            pthread_cond_wait(&parser_cond, &parser_mutex);
        }
        // 取一个 fd 出来处理（去重）
        int fd = *parser_pending_set.begin();
        parser_pending_set.erase(parser_pending_set.begin());
        pthread_mutex_unlock(&parser_mutex);

        // 处理该 fd 的缓冲区
        pthread_mutex_lock(&buffer_map_mutex);
        auto it = client_buffers.find(fd);
        if(it == client_buffers.end()){
            pthread_mutex_unlock(&buffer_map_mutex);
            continue;
        }
        ClientBuffer &buf = it->second;
        pthread_mutex_unlock(&buffer_map_mutex);

        string message;
        while(true){
            int consumed = extractMessage(buf.buffer, buf.pos, message);
            if(consumed == -1){
                // 不完整，等待更多数据
                break;
            } else if(consumed == -2){
                // 无效长度，移位
                pthread_mutex_lock(&buffer_map_mutex);
                if(buf.pos > 1){
                    memmove(buf.buffer, buf.buffer + 1, buf.pos - 1);
                    buf.pos -= 1;
                } else {
                    buf.pos = 0;
                }
                pthread_mutex_unlock(&buffer_map_mutex);
                continue;
            } else if(consumed == 0){
                break;
            } else {
                // 完整消息 -> 入队线程池
                Task task;
                task.fd = fd;
                task.message = message;
                task.type = CLIENT_MSG;
                pool.addTask(task);

                // 移除已消费数据
                pthread_mutex_lock(&buffer_map_mutex);
                memmove(buf.buffer, buf.buffer + consumed, buf.pos - consumed);
                buf.pos -= consumed;
                pthread_mutex_unlock(&buffer_map_mutex);
            }
        }
    }
    return NULL;
}
void en_resp(char msg[],int clint_fd){
    Response resp;
    resp.fd=clint_fd;
    resp.out=string(msg);
    resp.close_after=false;
    if(strcmp(msg,"bye\n")==0)resp.close_after=true;
    pthread_mutex_lock(&resp_mutex);
    resp_queue.push(resp);
    pthread_mutex_unlock(&resp_mutex);
    signal_event_fd();
}
void process_clint_data(Task&task){
    // 使用连接守卫确保连接一定被正确归还
    DbConnectionGuard guard(&pool);
    
    if(!guard.is_valid()){
        cerr << "Error: Failed to get database connection for task" << endl;
        if(task.type == CLIENT_MSG){
            char msg[] = "Error: Database connection failed";
            en_resp(msg, task.fd);
        }
        return;  // 守卫析构时自动清理（虽然conn是nullptr）
    }
    
    MyDb* conn = guard.get();

    // Check if connection is alive, reconnect if needed
    if (!conn->ping()) {
        LOG_WARN("Database connection lost in pool, attempting to reconnect...");
    }

    // 处理 Redis 订阅入队的消息
    if(task.type == SUB_MSG){
        // 格式: cmd|code|msgid|payload
        char buf[BUF_SIZE];
        size_t len = min(task.message.size(), (size_t)BUF_SIZE - 1);
        memcpy(buf, task.message.data(), len);
        buf[len] = '\0';
        char* saveptr = NULL;
        char* cmd = strtok_r(buf, "|", &saveptr);
        char* code = strtok_r(NULL, "|", &saveptr);
        char* msgid_str = strtok_r(NULL, "|", &saveptr);
        char* payload = strtok_r(NULL, "|", &saveptr); // payload expected like "from;text"
        if(!cmd || !msgid_str || !payload){
            LOG_WARN("Invalid SUB_MSG payload: " + task.message);
            return;
        }
        long long msgid = atoll(msgid_str);
        // payload 例如: "from;text"
        // 查找 channel（receiver）是否在本机在线
        int receiver_id = task.channel;
        string username = conn->get_name(receiver_id);
        if(username.empty()) return;
        pthread_mutex_lock(&client_map_mutex);
        auto it = clint_nametofd.find(username);
        int to_fd = (it != clint_nametofd.end()) ? it->second : -1;
        pthread_mutex_unlock(&client_map_mutex);
        if(to_fd != -1){
            // 转发给客户端：保持消息内容不含 msgid（和本机消息一致）
            // payload 形如 "from;text"，构造 client 消息: cmd|code|from;text
            char out[BUF_SIZE];
            snprintf(out, BUF_SIZE-1, "%s|%s|%s", cmd, code, payload);
            out[strlen(out)] = 0;
            en_resp(out, to_fd);
            // 更新 DB：通过 msgid 标记已投递
            string update = "update chat_log set is_delivered=1 where id=" + to_string(msgid);
            conn->exeSQL(update);
        }
        return;
    }
    
    // 以下处理客户端发来的消息（parser 解析后入队）
    char buf[BUF_SIZE];
    size_t len = min(task.message.size(), (size_t)BUF_SIZE - 1);
    memcpy(buf, task.message.data(), len);
    buf[len] = '\0';
    int clint_fd=task.fd;
    char*saveptr=NULL;
    char*cmd=strtok_r(buf,"|",&saveptr);
    if(!cmd){
        return;  // ✓ 守卫析构时自动调用 en_conn()
    }
    if(strcmp(cmd,"sign_up")==0){
        char*username=strtok_r(NULL,"|",&saveptr);
        char*password=strtok_r(NULL,"|",&saveptr);
        if (!username || !password) {
            char msg[] = "sign_up|0|请重试";
            en_resp(msg,clint_fd);
            return;
        }           
        string sql="select user_id from user where user_name='"+string(username)+"'";
        string ret="";
        bool res=conn->select_one_SQL(sql,ret);
        // puts("1");
        if(!res){//无相同的name
            string p = generate_str();
            string salt="$1$"+p+"$";
            pthread_mutex_lock(&crypt_mutex);
            string new_password = crypt(password, salt.c_str());
            pthread_mutex_unlock(&crypt_mutex);
            string sql = "insert into user (user_name, password, salt) values ('" + string(username) + "', '" + new_password + "', '" + p + "')";
            res=conn->exeSQL(sql);
            if(res){
                //查询该用户的user_id
                int user_id=conn->get_id(username);
                // printf("user_id:%d\n",user_id);
                LOG_OPERATION(user_id,"sign_up","username: "+string(username));
                if(user_id==-1){
                    char msg[]="sign_up|0|请重试";
                    en_resp(msg,clint_fd);
                    return;
                }
                sql="insert into user_status (user_id) values ("+to_string(user_id)+")";//新用户信息插入user_status
                if(!conn->exeSQL(sql)){
                    char msg[]="sign_up|0|请重试";
                    en_resp(msg,clint_fd);
                    return;
                }
                char msg[]="sign_up|1|请登录";
                en_resp(msg,clint_fd);
            }
            else{
                char msg[]="sign_up|0|请重试";
                en_resp(msg,clint_fd);
            }
        }
        else{//name重复
            char msg[]="sign_up|0|用户名重复";
            en_resp(msg,clint_fd);
        }
    }
    else if(strcmp(cmd,"sign_in")==0){
        char*username=strtok_r(NULL,"|",&saveptr);
        char*password=strtok_r(NULL,"|",&saveptr);
        if (!username || !password) {
            char msg[] = "sign_in|0|请重试";//eg:sign_in|1|ok
            en_resp(msg,clint_fd);
            return;
        }           
        string sql="select user_name,password,salt from user where user_name='"+string(username)+"'";
        string ret="";
        bool res=conn->select_one_SQL(sql,ret);
        if(!res){
            char msg[]="sign_in|0|无此用户";
            en_resp(msg,clint_fd);
        }
        else{
            // 对查询结果进行解析
            char *str=new char[ret.size()+1];
            strcpy(str,ret.c_str());
            char* saveptr_db = NULL;
            char*db_name=strtok_r(str,"|", &saveptr_db);
            char*db_password=strtok_r(NULL,"|", &saveptr_db);
            char*db_salt=strtok_r(NULL,"|", &saveptr_db);
            // 防御性检查：避免 std::string(nullptr) 导致 basic_string: construction from null
            if(!db_name || !db_password || !db_salt){
                LOG_ERROR("Login failed: invalid DB row (null field) for user "+string(username),ERR_DB_QUERY_FAIL);
                char msg[]="sign_in|0|请重试";
                en_resp(msg,clint_fd);
                delete[] str;
                return;
            }
            string salt="$1$"+string(db_salt)+"$";
            pthread_mutex_lock(&crypt_mutex);
            string computed_hash = crypt(password, salt.c_str());
            pthread_mutex_unlock(&crypt_mutex);
            if(strcmp(db_password, computed_hash.c_str())==0){
                //更新status表
                int id=conn->get_id(db_name);
                // printf("userid:%d\n",id);
                sql="update user_status set is_online=1 , last_active = NOW() where user_id = "+to_string(id)+" and is_online=0";
                if(!conn->exeSQL(sql)){
                    char msg[] = "sign_in|0|请重试";
                    en_resp(msg,clint_fd);
                }
                else{
                    pthread_mutex_lock(&client_map_mutex);
                    clint_fdtoname[clint_fd]=string(username);
                    clint_nametofd[string(username)]=clint_fd;
                    pthread_mutex_unlock(&client_map_mutex);
                    int uid=conn->get_id(username);
                    LOG_OPERATION(uid,"login","username: "+string(username));
                    char msg[]="sign_in|1|ok";
                    en_resp(msg,clint_fd);
                    // 订阅该用户在 Redis 的频道（user_id）以接收跨服务器转发消息
                    pthread_mutex_lock(&redis_mutex);
                    if (redis_subscribed_channels.find(uid) == redis_subscribed_channels.end()) {
                        if (redis_.subscribe(uid)) {
                            redis_subscribed_channels.insert(uid);
                            LOG_INFO("Subscribed Redis channel for user_id=" + to_string(uid));
                        } else {
                            LOG_WARN("Failed to subscribe Redis channel for user_id=" + to_string(uid));
                        }
                    }
                    pthread_mutex_unlock(&redis_mutex);
                    //查询是否有未读信息
                    string ret="";
                    string sql="select su.user_name,c.send_time,c.content from chat_log c join user ru on c.receiver_id=ru.user_id join user su on c.sender_id=su.user_id where ru.user_name='"+string(username)+"' and c.is_delivered=0 order by c.send_time";
                    conn->select_many_SQL(sql,ret);
                    if(ret.empty()){
                        return ;
                    }
                    char resp[BUF_SIZE];
                    snprintf(resp,BUF_SIZE-1,"chat_unread|1|%s",ret.c_str());
                    resp[strlen(resp)]=0;
                    en_resp(resp,clint_fd);
                    int receiver_id=conn->get_id(username);
                    sql="update chat_log set is_delivered=1 where is_delivered=0 and receiver_id="+to_string(receiver_id);
                    // puts(sql.c_str());
                    conn->exeSQL(sql);
                    // printf("查询未读信息:%s\n",resp);
                    // puts(resp);
                }
            }
            else{//密码错误
                LOG_ERROR("Login failed: incorrect password for user "+string(db_name),ERR_PASSWORD_INCORRECT);
                char msg[]="sign_in|0|密码错误";
                en_resp(msg,clint_fd);
            }
            delete[]str;
        }
    }
    else if(strcmp(cmd,"show_online_user")==0){
        string sql="select user_name from user join user_status on user.user_id = user_status.user_id where is_online = 1";
        string ret="";
        if(conn->select_many_SQL(sql,ret)){
            char msg[BUF_SIZE];
            snprintf(msg,BUF_SIZE-1,"show_online_user|1|%s",ret.c_str());
            msg[strlen(msg)]=0;
            en_resp((char*)msg,clint_fd);
        }
        else{
            char msg[]="show_online_user|0|请重试";
            en_resp(msg,clint_fd);
        }
    }
    else if(strcmp(cmd,"single_chat")==0){
        // 访问映射加锁
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string from_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        const char*from=from_name.c_str();
        const char*to=strtok_r(NULL,"|",&saveptr);
        const char*text=strtok_r(NULL,"|",&saveptr);
        string receiver_id=to_string(conn->get_id(to));
        if(receiver_id=="-1"){//发送给的用户不存在
            char msg[BUF_SIZE];
            snprintf(msg,BUF_SIZE-1,"single_chat|0|%s","用户不存在");
            msg[strlen(msg)]=0;
            en_resp(msg,clint_fd);
            return ;
        }
        string sender_id=to_string(conn->get_id(from));
        string group_type="single";
        pthread_mutex_lock(&client_map_mutex);
        bool online_local = clint_nametofd.count(to);
        int to_fd = online_local ? clint_nametofd[to] : -1;
        pthread_mutex_unlock(&client_map_mutex);

        // 先将消息插入 chat_log，拿到 message id
        string esc_text = escape_sql(string(text));
        string is_delivered = online_local ? "1" : "0";
        string insert_sql = "insert into chat_log (sender_id,receiver_id,is_delivered,group_type,content) values("+sender_id+","+receiver_id+","+is_delivered+",'"+group_type+"','"+esc_text+"')";
        if(!conn->exeSQL(insert_sql)){
            char msg_err[] = "single_chat|0|请重试";
            en_resp(msg_err, clint_fd);
            return;
        }
        long long msgid = conn->get_last_insert_id();

        if(online_local){
            // 本机在线，直接下发并标记已投递
            char msg[BUF_SIZE];
            snprintf(msg,BUF_SIZE-1,"single_chat|1|%s;%s",from,text);
            msg[strlen(msg)]=0;
            en_resp(msg,to_fd);
            string update_sql = "update chat_log set is_delivered=1 where id=" + to_string(msgid);
            conn->exeSQL(update_sql);
        }
        else{
            // 不在本机，发布到 Redis 的接收者频道，消息格式增加 msgid
            int rid = atoi(receiver_id.c_str());
            if (rid != -1) {
                char pub_msg[BUF_SIZE];
                // pub payload: cmd|code|msgid|from;text
                snprintf(pub_msg, BUF_SIZE-1, "single_chat|1|%lld|%s;%s", msgid, from, text);
                pthread_mutex_lock(&redis_mutex);
                redis_.publish(rid, string(pub_msg));
                pthread_mutex_unlock(&redis_mutex);
            }
        }

        char msg_resp[BUF_SIZE];
        snprintf(msg_resp,BUF_SIZE-1,"single_chat|2|发送成功");
        msg_resp[strlen(msg_resp)]=0;
        en_resp(msg_resp, clint_fd);
        //更新status
        string sql="update user_status set last_active = NOW() where user_id = "+sender_id;
        conn->exeSQL(sql);        
    }
    else if(strcmp(cmd,"multi_chat")==0){
        char*usernames=strtok_r(NULL,"|",&saveptr);
        const char*text=strtok_r(NULL,"|",&saveptr);
        // 访问映射加锁
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string from_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        const char*from=from_name.c_str();
        // printf("usernames:%s,text:%s\n",usernames,text);
        if(!text||!usernames){
            char msg[BUF_SIZE];
            snprintf(msg,BUF_SIZE-1,"mulit_chat|0|error");
            return ;
        }
        string sender_id=to_string(conn->get_id(from));
        // 这里不能复用 saveptr，否则会破坏上面 cmd 的分割状态
        char* names_saveptr = NULL;
        char* to=strtok_r(usernames," ",&names_saveptr);
        while(to){
            // Filter out self-sending
            if(strcmp(to, from) == 0){
                to=strtok_r(NULL," ",&names_saveptr);
                continue;
            }

            string receiver_id=to_string(conn->get_id(to));
            if(receiver_id=="-1"){//发送给的用户不存在
                to=strtok_r(NULL," ",&names_saveptr);
                continue;
            }
            string is_delivered="1";
            string group_type="multi";
            pthread_mutex_lock(&client_map_mutex);
            bool online = clint_nametofd.count(to);
            int to_fd = online ? clint_nametofd[to] : -1;
            pthread_mutex_unlock(&client_map_mutex);
            
            if(!online){//接收用户不在线
                is_delivered="0";
            }
            
            // 插入该接收者的 chat_log 并获取 msgid
            string esc_text = escape_sql(string(text));
            string insert_sql = "insert into chat_log (sender_id,receiver_id,is_delivered,group_type,content) values("+sender_id+","+receiver_id+","+is_delivered+",'"+group_type+"','"+esc_text+"')";
            if(!conn->exeSQL(insert_sql)){
                to=strtok_r(NULL," ",&names_saveptr);
                continue;
            }
            long long msgid = conn->get_last_insert_id();

            if(!online){
                // 发布到远端用户频道（包含msgid）
                int rid = conn->get_id(to);
                if (rid != -1) {
                    char pub_msg[BUF_SIZE];
                    snprintf(pub_msg, BUF_SIZE-1, "multi_chat|2|%lld|%s;%s", msgid, from, text);
                    pthread_mutex_lock(&redis_mutex);
                    redis_.publish(rid, string(pub_msg));
                    pthread_mutex_unlock(&redis_mutex);
                }
            }
            else{
                char msg[BUF_SIZE];
                snprintf(msg,BUF_SIZE-1,"multi_chat|2|%s;%s",from,text);
                msg[strlen(msg)]=0;
                en_resp(msg,to_fd);
                string update_sql = "update chat_log set is_delivered=1 where id=" + to_string(msgid);
                conn->exeSQL(update_sql);
            }
            to=strtok_r(NULL," ",&names_saveptr);
        }
        //更新status
        string sql="update user_status set last_active = NOW() where user_id = "+sender_id;
        conn->exeSQL(sql); 
        char msg_resp[BUF_SIZE];
        snprintf(msg_resp,BUF_SIZE-1,"multi_chat|1|发送成功");
        msg_resp[strlen(msg_resp)]=0;
        en_resp(msg_resp,clint_fd);
    }
    else if(strcmp(cmd,"broadcast_chat")==0){
        const char*text=strtok_r(NULL,"|",&saveptr);
        if(!text){
            char msg[BUF_SIZE];
            snprintf(msg,BUF_SIZE-1,"broadcast_chat|0|error");
            en_resp(msg,clint_fd);
            return ;
        }
        
        // 获取发送者信息
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string from_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        
        if(from_name.empty()){
            char msg[BUF_SIZE];
            snprintf(msg,BUF_SIZE-1,"broadcast_chat|0|未登录");
            en_resp(msg,clint_fd);
            return ;
        }
        
        string sender_id=to_string(conn->get_id(from_name.c_str()));
        string group_type="broadcast";
        
        // 获取所有在线用户
        string online_users="";
        // 优化查询：一次性获取用户名和ID，避免在循环中反复查询数据库
        string sql="select user.user_name, user.user_id from user join user_status on user.user_id = user_status.user_id where is_online = 1";
        if(!conn->select_many_SQL(sql, online_users)){
            char msg[BUF_SIZE];
            snprintf(msg,BUF_SIZE-1,"broadcast_chat|0|获取在线用户失败");
            en_resp(msg,clint_fd);
            return ;
        }
        
        // 处理在线用户列表
        char* buf = strdup(online_users.c_str());
        char* saveptr_online = NULL;
        char* user_name_token = strtok_r(buf, " \n", &saveptr_online);
        
        while(user_name_token){
            // 获取对应的 user_id
            char* user_id_token = strtok_r(NULL, " \n", &saveptr_online);
            if(!user_id_token) break;

            string current_user_name = user_name_token;
            string receiver_id = user_id_token;

            // 过滤掉发送者自己
            if(current_user_name == from_name){
                user_name_token = strtok_r(NULL, " \n", &saveptr_online);
                continue;
            }
            
            // 检查用户是否在本地在线
            pthread_mutex_lock(&client_map_mutex);
            bool online_local = clint_nametofd.count(current_user_name);
            int to_fd = online_local ? clint_nametofd[current_user_name] : -1;
            pthread_mutex_unlock(&client_map_mutex);
            
            // 插入聊天记录
            string esc_text = escape_sql(string(text));
            string is_delivered = online_local ? "1" : "0";
            string insert_sql = "insert into chat_log (sender_id,receiver_id,is_delivered,group_type,content) values("+sender_id+","+receiver_id+","+is_delivered+",'"+group_type+"','"+esc_text+"')";
            
            if(!conn->exeSQL(insert_sql)){
                LOG_ERROR("Failed to insert chat log for user: " + current_user_name, ERR_DB_EXECUTE_FAIL);
                user_name_token = strtok_r(NULL, " \n", &saveptr_online);
                continue;
            }
            
            long long msgid = conn->get_last_insert_id();
            
            if(online_local){
                // 本地在线，直接发送
                char msg[BUF_SIZE];
                snprintf(msg,BUF_SIZE-1,"broadcast_chat|1|%s;%s",from_name.c_str(),text);
                msg[strlen(msg)]=0;
                en_resp(msg,to_fd);
                
                // 更新消息状态为已投递
                string update_sql = "update chat_log set is_delivered=1 where id=" + to_string(msgid);
                conn->exeSQL(update_sql);
            }
            else{
                // 远程用户，通过Redis发布消息
                int rid = atoi(receiver_id.c_str());
                if (rid != -1) {
                    char pub_msg[BUF_SIZE];
                    // pub payload: cmd|code|msgid|from;text
                    snprintf(pub_msg, BUF_SIZE-1, "broadcast_chat|1|%lld|%s;%s", msgid, from_name.c_str(), text);
                    pthread_mutex_lock(&redis_mutex);
                    redis_.publish(rid, string(pub_msg));
                    pthread_mutex_unlock(&redis_mutex);
                }
            }
            
            user_name_token = strtok_r(NULL, " \n", &saveptr_online);
        }
        
        free(buf);
        
        // 更新发送者状态
        string sql_update="update user_status set last_active = NOW() where user_id = "+sender_id;
        conn->exeSQL(sql_update);
        
        // 发送成功响应
        char msg_resp[BUF_SIZE];
        snprintf(msg_resp,BUF_SIZE-1,"broadcast_chat|2|发送成功");
        msg_resp[strlen(msg_resp)]=0;
        en_resp(msg_resp,clint_fd);
    }
    else if(strcmp(cmd,"show_history")==0){
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string username = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(username.empty()){
            // pool.en_conn(conn);
            return;
        }
        // 查询与当前用户相关的所有聊天记录（自己是发送方或接收方都要查出来）
        string sql=
            "select ru.user_name as sender, u.user_name as receiver, "
            "send_time, group_type, content "
            "from chat_log c "
            "join user u on c.receiver_id = u.user_id "
            "join user ru on ru.user_id = c.sender_id "
            "where u.user_name = '" + username + "' "
            "or ru.user_name = '" + username + "'";
        string ret="";
        conn->select_many_SQL(sql,ret);
        char msg[BUF_SIZE];
        snprintf(msg,BUF_SIZE-1,"show_history|1|%s",ret.c_str());
        en_resp(msg,clint_fd);
    }
    else if(strcmp(cmd,"q\n")==0||strcmp(cmd,"Q\n")==0){
        //更新status
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string username = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(username.empty()){
            // pool.en_conn(conn);
            return;
        }
        int id=conn->get_id(username.c_str());
        // printf("user_id:%d\n",id);
        string sql="update user_status set is_online = 0 where user_id ="+to_string(id);
        conn->exeSQL(sql);
        // 退订该用户的 Redis 频道
        pthread_mutex_lock(&redis_mutex);
        if (redis_subscribed_channels.count(id)) {
            if (redis_.unsubscribe(id)) {
                redis_subscribed_channels.erase(id);
                LOG_INFO("Unsubscribed Redis channel for user_id=" + to_string(id));
            } else {
                LOG_WARN("Failed to unsubscribe Redis channel for user_id=" + to_string(id));
            }
        }
        pthread_mutex_unlock(&redis_mutex);
        char msg[]="bye\n";
        en_resp(msg,clint_fd);
    }
    else if(strcmp(cmd,"heartbeat")==0){
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string username = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(username.empty()){
            char msg[]="heartbeat|0|未登录";
            en_resp(msg,clint_fd);
            // pool.en_conn(conn);
            return;
        }
        int user_id=conn->get_id(username.c_str());
        string sql="update user_status set last_active = NOW() where user_id ="+to_string(user_id);
        if(conn->exeSQL(sql)){
            char msg[]="heartbeat|1|ok";
            en_resp(msg,clint_fd);
        }
        else{
            char msg[]="heartbeat|0|更新失败";
            en_resp(msg,clint_fd);
        }
    }
    // ✓ 不需要手动调用 en_conn()，守卫析构时自动调用
}
void handle_response(){
    uint64_t tmp;
    read(event_fd,&tmp,sizeof(tmp));
    while(1){
        pthread_mutex_lock(&resp_mutex);
        if(resp_queue.empty()){
            pthread_mutex_unlock(&resp_mutex);
            break;
        }
        Response resp=resp_queue.front();
        resp_queue.pop();
        pthread_mutex_unlock(&resp_mutex);
        if(!sendMessage(resp.fd, resp.out)){
            LOG_ERROR("Failed to send response to fd=" + to_string(resp.fd), ERR_MSG_SEND_FAIL);
        }
        if(resp.close_after){
            close_clint(epoll_fd,resp.fd);
        }
    }
}
void* check_timeout_thread(void* arg) {
    MyDb con;
    con.initDB(HOST, USER, PWD, DB_NAME, 3306);
    while (1) {
        sleep(10);
        string ret;
        // 优化超时检查: 40秒无心跳则判定超时（留有缓冲时间）
        // C++客户端心跳间隔: 15秒
        // Python客户端心跳间隔: 18秒
        // 40秒足够检测到真正掉线的客户端，并给正常客户端足够的缓冲时间
        con.select_many_SQL(
            "select user_id from user_status "
            "where is_online=1 and last_active < NOW() - INTERVAL 40 SECOND",
            ret
        );
        if (ret.empty()) continue;
        char* buf = strdup(ret.c_str());
        char* saveptr_to = NULL;
        char* user_id = strtok_r(buf, "\n", &saveptr_to);
        while (user_id) {
            int uid = atoi(user_id);
            string update ="update user_status set is_online=0 where user_id="+to_string(uid);
            con.exeSQL(update);
            string name = con.get_name(uid);
            pthread_mutex_lock(&client_map_mutex);
            auto it_fd = clint_nametofd.find(name);
            int to_fd = (it_fd != clint_nametofd.end()) ? it_fd->second : -1;
            pthread_mutex_unlock(&client_map_mutex);
            if (to_fd != -1) {
                char msg[]="bye\n";
                en_resp(msg,to_fd);
            }
            user_id = strtok_r(NULL, "\n", &saveptr_to);
        }
        free(buf);
    }
    return nullptr;
}
void handleRedisSubscribeMessage(int channel, const string& message)
{
    // 收到订阅消息后只入队到线程池，由工作线程处理 DB 更新和下发，以避免阻塞订阅线程
    LOG_DEBUG("Received Redis message on channel " + to_string(channel) + ": " + message);
    Task task;
    task.type = SUB_MSG;
    task.channel = channel; // receiver user_id
    task.message = message; // 包含 message_id 的完整 payload
    pool.addTask(task);
}

int main(int argc,char*argv[]){
    ErrorCodeManager* errorcodemanager=ErrorCodeManager::getInstance();
    Logger*logger=Logger::getInstance();
    if(!logger->initialize("../logs","chatroom.log",LogLevel::DEBUG,true)){
        cerr<<"Failed to initialize logger"<<endl;
        return 1;
    }
    LOG_INFO("========Chatroom Server Statring========");
    // 安装 SIGINT 处理函数，用于优雅关闭服务器
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
    }

    pthread_mutex_init(&resp_mutex,NULL);
    pthread_mutex_init(&client_map_mutex,NULL);
    pthread_mutex_init(&buffer_map_mutex,NULL);  // 初始化缓冲区互斥锁
    pthread_mutex_init(&redis_mutex,NULL);  // 初始化 redis 操作互斥锁
    pthread_mutex_init(&crypt_mutex,NULL);  // 初始化 crypt 互斥锁
    pthread_mutex_init(&g_close_conn_mutex, NULL); // 初始化 close_clint 专用连接锁
    
    // 初始化 close_clint 专用数据库连接
    if (!g_close_conn.initDB(HOST, USER, PWD, DB_NAME, 3306)) {
        LOG_FATAL("Failed to initialize global close connection", ERR_DB_CONNECTION_FAIL);
        return 1;
    }

    srand(time(NULL));
    if (redis_.connect())
    {
        // 设置上报消息的回调
        redis_.init_notify_handler(std::bind(&handleRedisSubscribeMessage, std::placeholders::_1, std::placeholders::_2));
    }
    // 启动 parser 线程（负责粘包/拆包）
    pthread_t parser_tid;
    pthread_mutex_init(&parser_mutex, NULL);
    pthread_cond_init(&parser_cond, NULL);
    if(pthread_create(&parser_tid, NULL, parser_thread_func, NULL) != 0){
        perror("Failed to create parser thread");
        exit(0);
    }
    // 启动心跳检测线程
    pthread_t timeout_tid;
    if(pthread_create(&timeout_tid, NULL, check_timeout_thread, NULL) != 0){
        perror("Failed to create timeout thread");
        exit(0);
    }
    ser_fd=server_init(argc,argv);
    if(set_unblocking(ser_fd)==0){
        close(ser_fd);
        exit(0);
    }
    struct epoll_event event,events[MAX_EVENTS];
    epoll_fd=epoll_create(1);
    if(epoll_fd==-1){
        LOG_FATAL("Epoll create failed",ERR_EPOLL_CREATE_FAIL);
        exit(0);
    }
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    event.data.fd=ser_fd;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,ser_fd,&event)==-1){
        LOG_FATAL("Epoll_ctl add server socket failed",ERR_EPOLL_CTL_FAIL);
        close(epoll_fd);
        close(ser_fd);
        close(event_fd);
        exit(0);
    }
    event.events=EPOLLIN;
    event.data.fd=event_fd;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,event_fd,&event)==-1){
        LOG_FATAL("Epoll_ctl add event_fd failed",ERR_EPOLL_CTL_FAIL);
        close(epoll_fd);
        close(ser_fd);
        close(event_fd);
        exit(0);
    }
    LOG_INFO("Epoll server started successfully, waiting for connections...");
    int i;
    while(1){
        int num_fd=epoll_wait(epoll_fd,events,MAX_EVENTS,-1);
        if(num_fd==-1){
            if(errno == EINTR){
                // 被信号中断，如果是 SIGINT 触发的，则准备优雅退出
                if(stop_server){
                    LOG_INFO("SIGINT received, preparing to shutdown server gracefully...");
                    break;
                }
                // 其他信号中断则继续等待
                continue;
            }
            LOG_ERROR("Epoll wait failed",ERR_EPOLL_WAIT_FAIL);
            break;
        }
        for(i=0;i<num_fd;i++){
            int fd=events[i].data.fd;
            uint32_t ev=events[i].events;
            if(fd==ser_fd){//有新客户端连接
                handle_new_connect();
            }
            else if(fd==event_fd){
                handle_response();
            }
            else{
                if(ev&EPOLLIN){//客户端有消息发送
                    handle_clint_data(epoll_fd,fd);
                }
                if(ev&(EPOLLERR|EPOLLHUP|EPOLLRDHUP)){//客户端断开连接
                    close_clint(epoll_fd,fd);
                }
            }
        }
    }
    // 优雅关闭：断开所有当前服务器连接的客户端（使用 clint_fdtoname 作为当前连接集合）
    pthread_mutex_lock(&client_map_mutex);
    vector<int> fds_to_close;
    fds_to_close.reserve(clint_fdtoname.size());
    for (const auto &p : clint_fdtoname) {
        fds_to_close.push_back(p.first);  // map<int, string> 的 key 就是 fd
    }
    pthread_mutex_unlock(&client_map_mutex);

    for(int fd : fds_to_close){
        close_clint(epoll_fd, fd);
    }

    close(ser_fd);
    close(epoll_fd);
    Logger::destroy();
    return 0;
}