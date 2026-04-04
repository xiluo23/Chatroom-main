#include"epoll_ser.h"
#include"DbConnectionGuard.h"
#include<sys/timerfd.h>
#include"MyDb.h"
#include"Logger.h"
#include"ErrorCode.h"
#include<functional>
#include <unordered_set>
#include<signal.h>
#include <sys/resource.h>
#include <memory>
#include"redis.h"
#include"Config.h"
#include"Dbhandle.h"
#include"ParserPool.h"
#include"CpuGuard.h"
#include<mutex>
using namespace std;
unordered_map<string,int>clint_nametofd;
unordered_map<int,string>clint_fdtoname;
pthread_mutex_t resp_mutex;
pthread_mutex_t client_map_mutex; // 在头文件中声明为 extern
pthread_mutex_t crypt_mutex; // 保护 crypt 函数的互斥锁
ThreadPool pool(16);  
Redis redis_;
int ser_fd,epoll_fd,tfd;
int event_fd;
queue<Response>resp_queue;
#include<memory>
unordered_map<int, unique_ptr<ClientBuffer>> client_buffers;  // 为每个客户端维护接收缓冲区，使用 unique_ptr 减少拷贝
unordered_map<int,time_t> latest_expire;
priority_queue<Timer, vector<Timer>, greater<Timer>> hp;
static const int HEARTBEAT_TIMEOUT_SEC = 60; // 心跳超时时间
pthread_mutex_t buffer_map_mutex;  // 保护 client_buffers 的互斥锁
pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER; // 保护 hp 和 latest_expire

// 专用于 close_clint 的全局数据库连接及互斥锁
MyDb g_close_conn;
pthread_mutex_t g_close_conn_mutex;

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
void update_expire(int clint_fd){
    time_t now = time(NULL);
    time_t expire = now + HEARTBEAT_TIMEOUT_SEC;
    latest_expire[clint_fd] = expire;
    Timer timer;
    timer.fd = clint_fd;
    timer.expire = expire;
    hp.push(timer);
}
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
int server_init(){
    Config* conf = Config::getInstance();
    int port = conf->getInt("server_port", 6000);
    int backlog = conf->getInt("listen_backlog", 1024);

    struct sockaddr_in ser_addr;
    if((ser_fd=socket(PF_INET,SOCK_STREAM,0))==-1){
        LOG_ERROR("Socket creation failed",ERR_SOCKET_CREATE_FAIL);
        exit(0);
    }
    memset(&ser_addr,0,sizeof(ser_addr));
    ser_addr.sin_family=AF_INET;
    ser_addr.sin_port=htons(port);
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
    if(listen(ser_fd,backlog)==-1){
        LOG_ERROR("Socket listen failed",ERR_SOCKET_LISTEN_FAIL);
        close(ser_fd);
        exit(0);
    }
    // 优化套接字缓冲区大小，支持高并发
    int sndbuf = 128 * 1024;  // 发送缓冲区32KB
    int rcvbuf = 128 * 1024;  // 接收缓冲区32KB
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

        // 预先创建接收缓冲区，作为连接活跃的标识
        pthread_mutex_lock(&buffer_map_mutex);
        client_buffers[clint_fd] = make_unique<ClientBuffer>();
        pthread_mutex_unlock(&buffer_map_mutex);

        // 新连接也需要初始化超时时间，避免无心跳连接无限保留
        update_expire(clint_fd);

        LOG_INFO("New client connected: FD="+to_string(clint_fd));
    }
}
void close_clint(int epoll_fd,int clint_fd){
    // 使用 buffer_map_mutex 作为全局关闭开关，防止重复调用
    pthread_mutex_lock(&buffer_map_mutex);
    if (client_buffers.find(clint_fd) == client_buffers.end()) {
        // 如果缓冲区已经不存在，说明该 FD 已被关闭并清理过，直接退出
        pthread_mutex_unlock(&buffer_map_mutex);
        return;
    }
    client_buffers.erase(clint_fd); // 移除缓冲区，标记该 FD 已被清理
    pthread_mutex_unlock(&buffer_map_mutex);

    // 取消超时记录，避免后续 timer heap 的懒删除误判
    latest_expire.erase(clint_fd);

    // 从 epoll 中删除并关闭
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, clint_fd, NULL);
    close(clint_fd);
    LOG_INFO("Client disconnected: FD="+to_string(clint_fd));
    
    // 从 ParserPool 的待处理队列中移除
    ParserPool::getInstance()->clearFd(clint_fd);

    // 访问全局用户映射前加锁
    string username="";
    pthread_mutex_lock(&client_map_mutex);
    auto it_name = clint_fdtoname.find(clint_fd);
    if (it_name != clint_fdtoname.end()) {
        username = it_name->second;
        clint_nametofd.erase(it_name->second);
        clint_fdtoname.erase(it_name);
    }
    pthread_mutex_unlock(&client_map_mutex);

    if (!username.empty()) {
        // 使用全局数据库连接更新状态
        pthread_mutex_lock(&g_close_conn_mutex);
        if (!g_close_conn.ping()) {
            Config* conf = Config::getInstance();
            g_close_conn.initDB(conf->getString("mysql_host", "127.0.0.1"), 
                               conf->getString("mysql_user", "ftpuser"), 
                               conf->getString("mysql_password", "926472"), 
                               conf->getString("mysql_dbname", "Chatroom"), 
                               conf->getInt("mysql_port", 3306));
        }
        
        int uid = g_close_conn.get_id(username.c_str());
        if (uid != -1) {
            string sql = "update user_status set is_online = 0, last_active = NOW() where user_id ="+to_string(uid);
            DbHandle::getInstance()->add_task(sql);
            
            // Redis 退订
            pthread_mutex_lock(&redis_mutex);
            if (redis_subscribed_channels.count(uid)) {
                if (redis_.unsubscribe(uid)) {
                    redis_subscribed_channels.erase(uid);
                }
            }
            pthread_mutex_unlock(&redis_mutex);
        }
        pthread_mutex_unlock(&g_close_conn_mutex);
    }
}
void handle_clint_data(int epoll_fd,int clint_fd){
    // 获取或创建客户端缓冲区
    pthread_mutex_lock(&buffer_map_mutex);
    if(client_buffers.find(clint_fd) == client_buffers.end()){
        client_buffers[clint_fd] = make_unique<ClientBuffer>();
    }
    ClientBuffer& client_buf = *(client_buffers[clint_fd]);
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
            if(client_buf.pos + bytes_read <= PROTOCOL_MAX_RECV_BUFFER_SIZE){
                memcpy(client_buf.buffer + client_buf.pos, temp_buf, bytes_read);
                client_buf.pos += bytes_read;
            }
            else{
                LOG_NET_ERROR(clint_fd,"Error: Receive buffer overflow (pos=" + to_string(client_buf.pos) + ", read=" + to_string(bytes_read) + ")", ERR_BUFFER_OVERFLOW);
                close_clint(epoll_fd,clint_fd);
                return;
            }
        }
    }
    
    // Reactor 只负责收数据并通知 ParserPool 进行解析（多线程粘包/拆包）
    ParserPool::getInstance()->addFd(clint_fd);
}
void signal_event_fd(){
    uint64_t one=1;
    write(event_fd,&one,sizeof(one));
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
        const string& m = task.message;
        size_t p1 = m.find('|');
        size_t p2 = (p1 == string::npos) ? string::npos : m.find('|', p1 + 1);
        size_t p3 = (p2 == string::npos) ? string::npos : m.find('|', p2 + 1);
        if (p1 == string::npos || p2 == string::npos || p3 == string::npos) {
            LOG_WARN("Invalid SUB_MSG payload: " + task.message);
            return;
        }
        string cmd = m.substr(0, p1);
        string code = m.substr(p1 + 1, p2 - p1 - 1);
        string msgid_str = m.substr(p2 + 1, p3 - p2 - 1);
        string payload = m.substr(p3 + 1);
        if (cmd.empty() || msgid_str.empty() || payload.empty()) {
            LOG_WARN("Invalid SUB_MSG payload: " + task.message);
            return;
        }
        long long msgid = atoll(msgid_str.c_str());
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
                snprintf(out, BUF_SIZE-1, "%s|%s|%s", cmd.c_str(), code.c_str(), payload.c_str());
                out[strlen(out)] = 0;
                en_resp(out, to_fd);
                // 异步更新 DB：通过 msgid 标记已投递
                string update = "update chat_log set is_delivered=1 where id=" + to_string(msgid);
                DbHandle::getInstance()->add_task(update);
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
        
        // 优化注册流程：直接插入并处理重复键错误，减少一次 SELECT 查询
        string p = generate_str();
        string salt="$1$"+p+"$";
        
        // 修复：将 crypt_data 移至堆分配，防止 128KB 的结构体在多线程栈上导致溢出
        unique_ptr<struct crypt_data> data(new struct crypt_data);
        memset(data.get(), 0, sizeof(struct crypt_data));
        
        // 使用 CpuScopedGuard 限制高并发下的哈希计算，防止 2 核环境下 Reactor 被饿死
        char* hashed;
        {
            CpuScopedGuard cpu_guard;
            hashed = crypt_r(password, salt.c_str(), data.get());
        }
        string new_password = (hashed != nullptr) ? hashed : "";
        
        if (new_password.empty()) {
            char msg[] = "sign_up|0|系统错误";
            en_resp(msg, clint_fd);
            return;
        }

        // 使用 escape_sql 防止特殊字符导致的 SQL 语法错误
        string safe_username = escape_sql(string(username));
        string insert_sql = "insert into user (user_name, password, salt) values ('" + safe_username + "', '" + new_password + "', '" + p + "')";
        
        if (conn->exeSQL(insert_sql)) {
            // 插入成功，获取新用户ID并异步插入状态表
            long long user_id = conn->get_last_insert_id();
            if (user_id > 0) {
                LOG_OPERATION(user_id, "sign_up", "username: " + string(username));
                string status_sql = "insert into user_status (user_id) values (" + to_string(user_id) + ")";
                DbHandle::getInstance()->add_task(status_sql);
                char msg[] = "sign_up|1|请登录";
                en_resp(msg, clint_fd);
            } else {
                char msg[] = "sign_up|0|注册失败，请重试";
                en_resp(msg, clint_fd);
            }
        } else {
            // 插入失败，大概率是用户名重复
            char msg[] = "sign_up|0|用户名已存在";
            en_resp(msg, clint_fd);
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
        
        string safe_username = escape_sql(string(username));
        string sql="select user_name,password,salt from user where user_name='"+safe_username+"'";
        string ret="";
        bool res=conn->select_one_SQL(sql,ret);
        if(!res){
            char msg[]="sign_in|0|无此用户";
            en_resp(msg,clint_fd);
        }
        else{
            // 对查询结果进行解析
            unique_ptr<char[]>str(new char[ret.size()+1]);
            strcpy(str.get(),ret.c_str());
            char* saveptr_db = NULL;
            char*db_name=strtok_r(str.get(),"|", &saveptr_db);
            char*db_password=strtok_r(NULL,"|", &saveptr_db);
            char*db_salt=strtok_r(NULL,"|", &saveptr_db);
            // 防御性检查：避免 std::string(nullptr) 导致 basic_string: construction from null
            if(!db_name || !db_password || !db_salt){
                LOG_ERROR("Login failed: invalid DB row (null field) for user "+string(username),ERR_DB_QUERY_FAIL);
                char msg[]="sign_in|0|请重试";
                en_resp(msg,clint_fd);
                return;
            }
            string salt="$1$"+string(db_salt)+"$";
            
            // 使用线程安全的 crypt_r 替代带全局锁的 crypt
            // 修复：将 crypt_data 移至堆分配，防止 128KB 的结构体在多线程栈上导致溢出
            unique_ptr<struct crypt_data> data(new struct crypt_data);
            memset(data.get(), 0, sizeof(struct crypt_data));
            
            // 使用 CpuScopedGuard 限制高并发下的哈希计算，防止 2 核环境下 Reactor 被饿死
            char* hashed;
            {
                CpuScopedGuard cpu_guard;
                hashed = crypt_r(password, salt.c_str(), data.get());
            }
            string computed_hash = (hashed != nullptr) ? hashed : "";
            
            if (computed_hash.empty()) {
                char msg[] = "sign_in|0|系统错误";
                en_resp(msg, clint_fd);
                return;
            }

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
                    // 查询是否有未读信息（离线消息）
                    string ret="";
                    // 返回字段：sender_name send_time group_type conversation_id content
                    // conversation_id 用于群聊离线消息定位具体群
                    string sql="select su.user_name,c.send_time,c.group_type,ifnull(c.conversation_id,0),c.content "
                               "from chat_log c "
                               "join user ru on c.receiver_id=ru.user_id "
                               "join user su on c.sender_id=su.user_id "
                               "where ru.user_name='"+string(username)+"' and c.is_delivered=0 "
                               "order by c.send_time";
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
    else if(strcmp(cmd,"show_groups")==0){
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        // 查询用户加入的所有群组
        string sql = "select c.conversation_id, c.name from conversation c join conversation_member m on c.conversation_id = m.conversation_id where m.user_id = " + to_string(my_id);
        string ret;
        if(conn->select_many_SQL(sql, ret)){
            char msg[BUF_SIZE];
            snprintf(msg, BUF_SIZE-1, "show_groups|1|%s", ret.c_str());
            en_resp(msg, clint_fd);
        } else {
            char msg[]="show_groups|1|暂无群组";
            en_resp(msg, clint_fd);
        }
    }
    else if(strcmp(cmd,"single_chat")==0){
        // 访问映射加锁
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string from_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(from_name.empty()){
            char msg[]="single_chat|0|未登录";
            en_resp(msg, clint_fd);
            return;
        }
        const char*from=from_name.c_str();
        const char*to=strtok_r(NULL,"|",&saveptr);
        const char*text=strtok_r(NULL,"|",&saveptr);
        if(!to || !text){
            char msg[]="single_chat|0|参数错误";
            en_resp(msg, clint_fd);
            return;
        }
        string receiver_id=to_string(conn->get_id(to));
        if(receiver_id=="-1"){//发送给的用户不存在
            char msg[BUF_SIZE];
            snprintf(msg,BUF_SIZE-1,"single_chat|0|%s","用户不存在");
            msg[strlen(msg)]=0;
            en_resp(msg,clint_fd);
            return ;
        }
        string sender_id=to_string(conn->get_id(from));

        // 只允许好友之间进行单播
        {
            string friend_ok;
            string check_sql =
                "select 1 from friend_relation "
                "where user_id=" + sender_id + " and friend_id=" + receiver_id + " and status='accepted' limit 1";
            if(!conn->select_one_SQL(check_sql, friend_ok)){
                char msg[]="single_chat|0|请先添加好友";
                en_resp(msg, clint_fd);
                return;
            }
        }

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
            // 异步更新消息状态为已投递
            string update_sql = "update chat_log set is_delivered=1 where id=" + to_string(msgid);
            DbHandle::getInstance()->add_task(update_sql);
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
        // 异步更新发送者状态
        string sql_status ="update user_status set last_active = NOW() where user_id = "+sender_id;
        DbHandle::getInstance()->add_task(sql_status);
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
            snprintf(msg,BUF_SIZE-1,"multi_chat|0|error");
            en_resp(msg, clint_fd);
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
                // 异步更新消息状态为已投递
                string update_sql = "update chat_log set is_delivered=1 where id=" + to_string(msgid);
                DbHandle::getInstance()->add_task(update_sql);
            }
            to=strtok_r(NULL," ",&names_saveptr);
        }
        //异步更新status
        string sql_status ="update user_status set last_active = NOW() where user_id = "+sender_id;
        DbHandle::getInstance()->add_task(sql_status);
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
                
                // 异步更新消息状态为已投递
                string update_sql = "update chat_log set is_delivered=1 where id=" + to_string(msgid);
                DbHandle::getInstance()->add_task(update_sql);
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
        
        // 异步更新发送者状态
        string sql_update="update user_status set last_active = NOW() where user_id = "+sender_id;
        DbHandle::getInstance()->add_task(sql_update);
        
        // 发送成功响应
        char msg_resp[BUF_SIZE];
        snprintf(msg_resp,BUF_SIZE-1,"broadcast_chat|2|发送成功");
        msg_resp[strlen(msg_resp)]=0;
        en_resp(msg_resp,clint_fd);
    }
    else if(strcmp(cmd,"add_friend")==0){
        char*friend_name=strtok_r(NULL,"|",&saveptr);
        if(!friend_name){
            char msg[]="add_friend|0|参数错误";
            en_resp(msg,clint_fd);
            return;
        }
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        int friend_id = conn->get_id(friend_name);
        if(friend_id == -1){
            char msg[]="add_friend|0|用户不存在";
            en_resp(msg,clint_fd);
            return;
        }
        if(my_id == friend_id){
            char msg[]="add_friend|0|不能添加自己";
            en_resp(msg,clint_fd);
            return;
        }

        string sql = "insert into friend_relation (user_id, friend_id, status) values (" + to_string(my_id) + "," + to_string(friend_id) + ",'pending')";
        if(conn->exeSQL(sql)){
            char msg[]="add_friend|1|已发送申请";
            en_resp(msg,clint_fd);
            // 如果对方在线，可以发送实时通知
            pthread_mutex_lock(&client_map_mutex);
            if(clint_nametofd.count(friend_name)){
                int to_fd = clint_nametofd[friend_name];
                char notice[BUF_SIZE];
                snprintf(notice, BUF_SIZE-1, "friend_request|1|%s", my_name.c_str());
                en_resp(notice, to_fd);
            } else {
                // Redis 转发
                char pub_msg[BUF_SIZE];
                snprintf(pub_msg, BUF_SIZE-1, "friend_request|1|0|%s", my_name.c_str());
                pthread_mutex_lock(&redis_mutex);
                redis_.publish(friend_id, string(pub_msg));
                pthread_mutex_unlock(&redis_mutex);
            }
            pthread_mutex_unlock(&client_map_mutex);
        } else {
            char msg[]="add_friend|0|已发送过申请或已是好友";
            en_resp(msg,clint_fd);
        }
    }
    else if(strcmp(cmd,"handle_friend")==0){
        char*friend_name=strtok_r(NULL,"|",&saveptr);
        char*status=strtok_r(NULL,"|",&saveptr); // accepted or rejected
        if(!friend_name || !status){
            char msg[]="handle_friend|0|参数错误";
            en_resp(msg,clint_fd);
            return;
        }
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        int friend_id = conn->get_id(friend_name);
        
        string sql = "update friend_relation set status='" + string(status) + "' where user_id=" + to_string(friend_id) + " and friend_id=" + to_string(my_id);
        if(conn->exeSQL(sql)){
            if(strcmp(status, "accepted") == 0){
                // 互相成为好友
                string sql2 = "insert ignore into friend_relation (user_id, friend_id, status) values (" + to_string(my_id) + "," + to_string(friend_id) + ",'accepted')";
                conn->exeSQL(sql2);
                char msg[]="handle_friend|1|已添加好友";
                en_resp(msg,clint_fd);
                
                // 通知对方（发起方）已通过，对方前端收到 handle_friend 会自动刷新好友列表
                pthread_mutex_lock(&client_map_mutex);
                if(clint_nametofd.count(friend_name)){
                    int to_fd = clint_nametofd[friend_name];
                    char notice[BUF_SIZE];
                    snprintf(notice, BUF_SIZE-1, "handle_friend|1|%s 已接受了你的好友申请", my_name.c_str());
                    en_resp(notice, to_fd);
                } else {
                    // Redis 转发，确保跨服务器也能收到通知
                    char pub_msg[BUF_SIZE];
                    snprintf(pub_msg, BUF_SIZE-1, "handle_friend|1|0|%s 已接受了你的好友申请", my_name.c_str());
                    pthread_mutex_lock(&redis_mutex);
                    redis_.publish(friend_id, string(pub_msg));
                    pthread_mutex_unlock(&redis_mutex);
                }
                pthread_mutex_unlock(&client_map_mutex);
            } else {
                char msg[]="handle_friend|1|已拒绝申请";
                en_resp(msg,clint_fd);
            }
        } else {
            char msg[]="handle_friend|0|处理失败";
            en_resp(msg,clint_fd);
        }
    }
    else if(strcmp(cmd,"show_friends")==0){
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        string sql = "select u.user_name from friend_relation f join user u on f.friend_id = u.user_id where f.user_id=" + to_string(my_id) + " and f.status='accepted'";
        string ret;
        if(conn->select_many_SQL(sql, ret)){
            char msg[BUF_SIZE];
            snprintf(msg, BUF_SIZE-1, "show_friends|1|%s", ret.c_str());
            en_resp(msg, clint_fd);
        } else {
            char msg[]="show_friends|1|暂无好友";
            en_resp(msg, clint_fd);
        }
    }
    else if(strcmp(cmd,"create_group")==0){
        char*group_name=strtok_r(NULL,"|",&saveptr);
        if(!group_name){
            char msg[]="create_group|0|参数错误";
            en_resp(msg,clint_fd);
            return;
        }
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        string sql = "insert into conversation (type, name, owner_id) values ('group', '" + string(group_name) + "', " + to_string(my_id) + ")";
        if(conn->exeSQL(sql)){
            long long conv_id = conn->get_last_insert_id();
            string sql2 = "insert into conversation_member (conversation_id, user_id, role) values (" + to_string(conv_id) + ", " + to_string(my_id) + ", 'admin')";
            conn->exeSQL(sql2);
            char msg[BUF_SIZE];
            snprintf(msg, BUF_SIZE-1, "create_group|1|群组创建成功，ID:%lld", conv_id);
            en_resp(msg, clint_fd);
        } else {
            char msg[]="create_group|0|创建失败";
            en_resp(msg, clint_fd);
        }
    }
    else if(strcmp(cmd,"delete_friend")==0){
        char*friend_name=strtok_r(NULL,"|",&saveptr);
        if(!friend_name){
            char msg[]="delete_friend|0|参数错误";
            en_resp(msg,clint_fd);
            return;
        }
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        int friend_id = conn->get_id(friend_name);
        
        string sql = "delete from friend_relation where (user_id=" + to_string(my_id) + " and friend_id=" + to_string(friend_id) + ") or (user_id=" + to_string(friend_id) + " and friend_id=" + to_string(my_id) + ")";
        if(conn->exeSQL(sql)){
            char msg[]="delete_friend|1|已删除好友";
            en_resp(msg,clint_fd);
        } else {
            char msg[]="delete_friend|0|删除失败";
            en_resp(msg,clint_fd);
        }
    }
    else if(strcmp(cmd,"join_group")==0){
        char*group_id_str=strtok_r(NULL,"|",&saveptr);
        if(!group_id_str){
            char msg[]="join_group|0|参数错误";
            en_resp(msg,clint_fd);
            return;
        }
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        string sql = "insert into conversation_member (conversation_id, user_id) values (" + string(group_id_str) + ", " + to_string(my_id) + ")";
        if(conn->exeSQL(sql)){
            char msg[]="join_group|1|成功加入群组";
            en_resp(msg, clint_fd);
        } else {
            char msg[]="join_group|0|群组不存在或已在群中";
            en_resp(msg, clint_fd);
        }
    }
    else if(strcmp(cmd,"quit_group")==0){
        char*group_id_str=strtok_r(NULL,"|",&saveptr);
        if(!group_id_str){
            char msg[]="quit_group|0|参数错误";
            en_resp(msg,clint_fd);
            return;
        }
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        string sql = "delete from conversation_member where conversation_id=" + string(group_id_str) + " and user_id=" + to_string(my_id);
        if(conn->exeSQL(sql)){
            char msg[]="quit_group|1|成功退出群组";
            en_resp(msg, clint_fd);
        } else {
            char msg[]="quit_group|0|退出失败";
            en_resp(msg, clint_fd);
        }
    }
    else if(strcmp(cmd,"show_group_members")==0){
        char*group_id_str=strtok_r(NULL,"|",&saveptr);
        if(!group_id_str){
            char msg[]="show_group_members|0|参数错误";
            en_resp(msg,clint_fd);
            return;
        }
        string sql = "select u.user_name from conversation_member m join user u on m.user_id = u.user_id where m.conversation_id = " + string(group_id_str);
        string ret;
        if(conn->select_many_SQL(sql, ret)){
            char msg[BUF_SIZE];
            snprintf(msg, BUF_SIZE-1, "show_group_members|2|%s", ret.c_str());
            en_resp(msg, clint_fd);
        } else {
            char msg[]="show_group_members|0|获取成员失败";
            en_resp(msg, clint_fd);
        }
    }
    else if(strcmp(cmd,"invite_group")==0){
        char*conv_id_str=strtok_r(NULL,"|",&saveptr);
        char*friend_name=strtok_r(NULL,"|",&saveptr);
        if(!conv_id_str || !friend_name){
            char msg[]="invite_group|0|参数错误";
            en_resp(msg,clint_fd);
            return;
        }

        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string my_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(my_name.empty()) return;

        int my_id = conn->get_id(my_name.c_str());
        int friend_id = conn->get_id(friend_name);
        if(friend_id == -1){
            char msg[]="invite_group|0|用户不存在";
            en_resp(msg,clint_fd);
            return;
        }
        if(friend_id == my_id){
            char msg[]="invite_group|0|不能邀请自己";
            en_resp(msg,clint_fd);
            return;
        }

        string group_name;
        if(!conn->select_one_SQL("select ifnull(name,'') from conversation where conversation_id=" + string(conv_id_str) + " and type='group' limit 1", group_name)){
            char msg[]="invite_group|0|群组不存在";
            en_resp(msg,clint_fd);
            return;
        }

        // 必须是群管理员才能邀请
        string role;
        if(!conn->select_one_SQL("select role from conversation_member where conversation_id=" + string(conv_id_str) + " and user_id=" + to_string(my_id) + " limit 1", role)){
            char msg[]="invite_group|0|你不在该群组中";
            en_resp(msg,clint_fd);
            return;
        }
        if(role != "admin"){
            char msg[]="invite_group|0|无权限邀请";
            en_resp(msg,clint_fd);
            return;
        }

        // 只能邀请已添加的好友
        string friend_ok;
        if(!conn->select_one_SQL("select 1 from friend_relation where user_id=" + to_string(my_id) + " and friend_id=" + to_string(friend_id) + " and status='accepted' limit 1", friend_ok)){
            char msg[]="invite_group|0|只能邀请好友";
            en_resp(msg,clint_fd);
            return;
        }

        string exists;
        if(conn->select_one_SQL("select 1 from conversation_member where conversation_id=" + string(conv_id_str) + " and user_id=" + to_string(friend_id) + " limit 1", exists)){
            char msg[]="invite_group|0|对方已在群中";
            en_resp(msg,clint_fd);
            return;
        }

        string sql = "insert into conversation_member (conversation_id, user_id) values (" + string(conv_id_str) + ", " + to_string(friend_id) + ")";
        if(conn->exeSQL(sql)){
            char msg[BUF_SIZE];
            snprintf(msg, BUF_SIZE-1, "invite_group|1|已邀请 %s 加入群组 %s", friend_name, group_name.c_str());
            en_resp(msg,clint_fd);

            bool online_local = false;
            int to_fd = -1;
            pthread_mutex_lock(&client_map_mutex);
            if(clint_nametofd.count(friend_name)){
                online_local = true;
                to_fd = clint_nametofd[friend_name];
            }
            pthread_mutex_unlock(&client_map_mutex);

            if(online_local){
                char notice[BUF_SIZE];
                snprintf(notice, BUF_SIZE-1, "group_invite|2|%s|%s|%s", conv_id_str, group_name.c_str(), my_name.c_str());
                en_resp(notice, to_fd);
            } else {
                char pub_msg[BUF_SIZE];
                snprintf(pub_msg, BUF_SIZE-1, "group_invite|2|%s|%s|%s", conv_id_str, group_name.c_str(), my_name.c_str());
                pthread_mutex_lock(&redis_mutex);
                redis_.publish(friend_id, string(pub_msg));
                pthread_mutex_unlock(&redis_mutex);
            }
        } else {
            char msg[]="invite_group|0|邀请失败";
            en_resp(msg,clint_fd);
        }
    }
    else if(strcmp(cmd,"group_chat")==0){
        char*conv_id_str=strtok_r(NULL,"|",&saveptr);
        const char*text=strtok_r(NULL,"|",&saveptr);
        if(!conv_id_str || !text){
            char msg[]="group_chat|0|参数错误";
            en_resp(msg, clint_fd);
            return;
        }
        pthread_mutex_lock(&client_map_mutex);
        auto it_name = clint_fdtoname.find(clint_fd);
        string from_name = (it_name != clint_fdtoname.end()) ? it_name->second : "";
        pthread_mutex_unlock(&client_map_mutex);
        if(from_name.empty()) return;

        int sender_id = conn->get_id(from_name.c_str());
        int conv_id = atoi(conv_id_str);

        // 获取群组内所有成员
        string sql_members = "select u.user_name from conversation_member m join user u on m.user_id = u.user_id where m.conversation_id = " + to_string(conv_id);
        string members_ret;
        if(conn->select_many_SQL(sql_members, members_ret)){
            // 构造消息
            char msg[BUF_SIZE];
            snprintf(msg, BUF_SIZE-1, "group_chat|1|%d|%s;%s", conv_id, from_name.c_str(), text);
            msg[strlen(msg)] = 0;

            // 遍历成员发送
            char* members_buf = strdup(members_ret.c_str());
            char* m_saveptr = NULL;
            char* member_name = strtok_r(members_buf, "\n", &m_saveptr);
            while(member_name){
                // Trim trailing space added by select_many_SQL
                char* p = member_name + strlen(member_name) - 1;
                while(p >= member_name && isspace(*p)) {
                    *p = '\0';
                    p--;
                }
                
                if(strlen(member_name) > 0 && strcmp(member_name, from_name.c_str()) != 0){
                    pthread_mutex_lock(&client_map_mutex);
                    bool online_local = clint_nametofd.count(member_name);
                    int to_fd = online_local ? clint_nametofd[member_name] : -1;
                    pthread_mutex_unlock(&client_map_mutex);

                    int receiver_id = conn->get_id(member_name);
                    string esc_text = escape_sql(string(text));
                    string is_delivered = online_local ? "1" : "0";
                    string insert_sql = "insert into chat_log (sender_id,receiver_id,is_delivered,group_type,content,conversation_id) values("+to_string(sender_id)+","+to_string(receiver_id)+","+is_delivered+",'multi','"+esc_text+"',"+to_string(conv_id)+")";
                    conn->exeSQL(insert_sql);
                    long long msgid = conn->get_last_insert_id();

                    if(online_local){
                        en_resp(msg, to_fd);
                    } else {
                        // Redis 转发
                        char pub_msg[BUF_SIZE];
                        snprintf(pub_msg, BUF_SIZE-1, "group_chat|1|%lld|%d|%s;%s", msgid, conv_id, from_name.c_str(), text);
                        pthread_mutex_lock(&redis_mutex);
                        redis_.publish(receiver_id, string(pub_msg));
                        pthread_mutex_unlock(&redis_mutex);
                    }
                }
                member_name = strtok_r(NULL, "\n", &m_saveptr);
            }
            free(members_buf);
            
            char ok_msg[] = "group_chat|2|发送成功";
            en_resp(ok_msg, clint_fd);
        } else {
            char msg[] = "group_chat|0|群组不存在或无成员";
            en_resp(msg, clint_fd);
        }
    }
    else if(strcmp(cmd,"update_profile")==0 || strcmp(cmd,"get_profile")==0){
        char msg[]="error|0|该版本已移除头像功能";
        en_resp(msg, clint_fd);
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

        char* scope = strtok_r(NULL, "|", &saveptr); // broadcast|private|group (optional)
        char* arg   = strtok_r(NULL, "|", &saveptr); // friend_name or conv_id (optional)
        string safe_username = escape_sql(username);
        string sql;
        string ret="";
        string uid_str = to_string(conn->get_id(username.c_str()));

        if(scope && (strcmp(scope,"broadcast")==0)){
            sql =
                "select ru.user_name as sender, u.user_name as receiver, "
                "send_time, group_type, ifnull(conversation_id, 0), content "
                "from chat_log c "
                "join user u on c.receiver_id = u.user_id "
                "join user ru on ru.user_id = c.sender_id "
                "where (u.user_name = '" + safe_username + "' or ru.user_name = '" + safe_username + "') "
                "and c.group_type='broadcast' "
                "order by send_time desc limit 50";
        } else if(scope && (strcmp(scope,"private")==0 || strcmp(scope,"single")==0)){
            if(arg && strlen(arg)>0){
                string friend_name = escape_sql(string(arg));
                string fid_str = to_string(conn->get_id(friend_name.c_str()));
                sql =
                    "select ru.user_name as sender, u.user_name as receiver, "
                    "send_time, group_type, ifnull(conversation_id, 0), content "
                    "from chat_log c "
                    "join user u on c.receiver_id = u.user_id "
                    "join user ru on ru.user_id = c.sender_id "
                    "where ((c.sender_id=" + uid_str + " and c.receiver_id=" + fid_str + ") "
                    "or (c.sender_id=" + fid_str + " and c.receiver_id=" + uid_str + ")) "
                    "and c.group_type='single' "
                    "order by send_time desc limit 50";
            } else {
                sql =
                    "select ru.user_name as sender, u.user_name as receiver, "
                    "send_time, group_type, ifnull(conversation_id, 0), content "
                    "from chat_log c "
                    "join user u on c.receiver_id = u.user_id "
                    "join user ru on ru.user_id = c.sender_id "
                    "where (u.user_name = '" + safe_username + "' or ru.user_name = '" + safe_username + "') "
                    "and c.group_type='single' "
                    "order by send_time desc limit 50";
            }
        } else if(scope && (strcmp(scope,"group")==0 || strcmp(scope,"multi")==0)){
            if(arg && strlen(arg)>0){
                string conv = escape_sql(string(arg));
                sql =
                    "select ru.user_name as sender, u.user_name as receiver, "
                    "send_time, group_type, ifnull(conversation_id, 0), content "
                    "from chat_log c "
                    "join user u on c.receiver_id = u.user_id "
                    "join user ru on ru.user_id = c.sender_id "
                    "where c.group_type='multi' and c.conversation_id=" + conv + " "
                    "order by send_time desc limit 50";
            } else {
                sql =
                    "select ru.user_name as sender, u.user_name as receiver, "
                    "send_time, group_type, ifnull(conversation_id, 0), content "
                    "from chat_log c "
                    "join user u on c.receiver_id = u.user_id "
                    "join user ru on ru.user_id = c.sender_id "
                    "where (u.user_name = '" + safe_username + "' or ru.user_name = '" + safe_username + "') "
                    "and c.group_type='multi' "
                    "order by send_time desc limit 50";
            }
        } else {
            sql=
                "select ru.user_name as sender, u.user_name as receiver, "
                "send_time, group_type, ifnull(conversation_id, 0), content "
                "from chat_log c "
                "join user u on c.receiver_id = u.user_id "
                "join user ru on ru.user_id = c.sender_id "
                "where (u.user_name = '" + safe_username + "' "
                "or ru.user_name = '" + safe_username + "') "
                "order by send_time desc limit 50";
        }

        conn->select_many_SQL(sql,ret);
        if (ret.length() > BUF_SIZE - 100) {
             LOG_WARN("History data too large, truncating...");
             ret = ret.substr(0, BUF_SIZE - 100);
        }
        char msg[BUF_SIZE];
        int written = snprintf(msg, BUF_SIZE-1, "show_history|1|%s", ret.c_str());
        if (written >= BUF_SIZE-1) { msg[BUF_SIZE-1] = 0; }
        en_resp(msg,clint_fd);
    }
    else if(strcmp(cmd,"q")==0||strcmp(cmd,"Q")==0){
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
        DbHandle::getInstance()->add_task(sql);
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
            // 对于未登录用户也应该刷新超时时间，因为可能正在注册/登录过程中
            update_expire(clint_fd);
            char msg[]="heartbeat|0|未登录";
            en_resp(msg,clint_fd);
            // pool.en_conn(conn);
            return;
        }
        char msg[]="heartbeat|1|ok";
        update_expire(clint_fd);
        en_resp(msg,clint_fd);
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
            int err = errno;
            // 对于 Broken pipe (EPIPE) 或 Connection reset (ECONNRESET)，
            // 说明客户端已主动断开，这是高并发下的正常现象，记录为 DEBUG 即可，避免污染 ERROR 日志
            if (err == EPIPE || err == ECONNRESET) {
                LOG_DEBUG("Client disconnected while sending response to fd=" + to_string(resp.fd));
            } else {
                LOG_ERROR("Failed to send response to fd=" + to_string(resp.fd) + 
                         " (errno=" + to_string(err) + ": " + strerror(err) + ")", ERR_MSG_SEND_FAIL);
            }
        }
        if(resp.close_after){
            close_clint(epoll_fd,resp.fd);
        }
    }
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
bool is_valid(int fd,time_t expire){
    pthread_mutex_lock(&timer_mutex);
    auto it = latest_expire.find(fd);
    bool valid = (it != latest_expire.end() && it->second == expire);
    pthread_mutex_unlock(&timer_mutex);
    return valid;
}

void check_timeout(){
    time_t now=time(NULL);
    while(true){
        Timer t;
        {
            pthread_mutex_lock(&timer_mutex);
            if(hp.empty()){
                pthread_mutex_unlock(&timer_mutex);
                break;
            }
            t = hp.top();
            if(t.expire > now){
                pthread_mutex_unlock(&timer_mutex);
                break;
            }
            hp.pop();
            pthread_mutex_unlock(&timer_mutex);
        }
        
        //懒删除，检测是否有效
        if(is_valid(t.fd,t.expire)){
            close_clint(epoll_fd,t.fd);
        }
    }
}


int main(int argc,char*argv[]){
    ErrorCodeManager* errorcodemanager=ErrorCodeManager::getInstance();
    Logger*logger=Logger::getInstance();
    if(!logger->initialize("../logs","chatroom.log",LogLevel::DEBUG,true)){
        cerr<<"Failed to initialize logger"<<endl;
        return 1;
    }
    LOG_INFO("========Chatroom Server Statring========");

    // 2. 根据配置调整池大小
    // 注意：这里需要重新设置 pool，或者通过配置初始化
    // 目前 pool 是全局变量，已经在 main 之前初始化为 16
    // 如果需要动态修改，可以给 ThreadPool 增加一个 resize 方法
    // 这里暂时保持 16，或者在 main 中根据配置重新初始化（如果 Pool 支持）
    
    // 检查并尝试提升文件描述符限制
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        LOG_INFO("Current RLIMIT_NOFILE: soft=" + to_string(rl.rlim_cur) + ", hard=" + to_string(rl.rlim_max));
        if (rl.rlim_cur < 65535) {
            // 尝试提升软限制到硬限制的最大值（或 65535）
            rlim_t target = (rl.rlim_max == RLIM_INFINITY) ? 65535 : rl.rlim_max;
            if (target > 65535) target = 65535; // 避免设置过大
            if (target > rl.rlim_cur) {
                rl.rlim_cur = target;
                if (setrlimit(RLIMIT_NOFILE, &rl) == 0) {
                    LOG_INFO("Successfully increased RLIMIT_NOFILE to " + to_string(rl.rlim_cur));
                } else {
                    LOG_WARN("Failed to increase RLIMIT_NOFILE: " + string(strerror(errno)));
                }
            }
        }
    } else {
        LOG_ERROR("Failed to get RLIMIT_NOFILE", ERR_UNKNOWN);
    }

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
    Config* conf = Config::getInstance();
    if (!g_close_conn.initDB(conf->getString("mysql_host", "127.0.0.1"), 
                             conf->getString("mysql_user", "ftpuser"), 
                             conf->getString("mysql_password", "926472"), 
                             conf->getString("mysql_dbname", "Chatroom"), 
                             conf->getInt("mysql_port", 3306))) {
        LOG_FATAL("Failed to initialize global close connection", ERR_DB_CONNECTION_FAIL);
        return 1;
    }

    // Ensure chat_log has conversation_id for group chat history (idempotent)
    {
        string col_count;
        bool ok = g_close_conn.select_one_SQL(
            "SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS "
            "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME='chat_log' AND COLUMN_NAME='conversation_id'",
            col_count
        );
        if (ok) {
            int count = atoi(col_count.c_str());
            if (count == 0) {
                g_close_conn.exeSQL("ALTER TABLE chat_log ADD COLUMN conversation_id INT DEFAULT NULL;");
            }
        }
    }

    // 启动异步数据库写入线程
    DbHandle::getInstance()->start();

    srand(time(NULL));
    if (redis_.connect())
    {
        // 设置上报消息的回调
        redis_.init_notify_handler(std::bind(&handleRedisSubscribeMessage, std::placeholders::_1, std::placeholders::_2));
    }
    
    // 3. 启动 ParserPool（多线程粘包/拆包）
    int parser_threads = Config::getInstance()->getInt("parser_thread_count", 2);
    ParserPool::getInstance()->init(parser_threads, &client_buffers, &buffer_map_mutex, &pool);
    ParserPool::getInstance()->start();

    ser_fd=server_init();
    if(set_unblocking(ser_fd)==0){
        close(ser_fd);
        exit(EXIT_FAILURE);
    }
    struct epoll_event event,events[MAX_EVENTS];
    epoll_fd=epoll_create(1);
    if(epoll_fd==-1){
        LOG_FATAL("Epoll create failed",ERR_EPOLL_CREATE_FAIL);
        exit(EXIT_FAILURE);
    }
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    event.data.fd=ser_fd;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,ser_fd,&event)==-1){
        LOG_FATAL("Epoll_ctl add server socket failed",ERR_EPOLL_CTL_FAIL);
        close(epoll_fd);
        close(ser_fd);
        exit(EXIT_FAILURE);
    }
    event_fd=eventfd(0,EFD_NONBLOCK);
    event.events=EPOLLIN;
    event.data.fd=event_fd;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,event_fd,&event)==-1){
        LOG_FATAL("Epoll_ctl add event_fd failed",ERR_EPOLL_CTL_FAIL);
        close(epoll_fd);
        close(ser_fd);
        close(event_fd);
        exit(EXIT_FAILURE);
    }
    // 定时器
    tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd == -1) {
        LOG_FATAL("timerfd_create failed", ERR_EPOLL_CREATE_FAIL);
        close(epoll_fd);
        close(ser_fd);
        close(event_fd);
        exit(EXIT_FAILURE);
    }
    struct itimerspec value;
    value.it_value.tv_sec = 10;
    value.it_value.tv_nsec = 0;
    value.it_interval = value.it_value;
    if (timerfd_settime(tfd, 0, &value, NULL) == -1) {
        LOG_FATAL("timerfd_settime failed", ERR_EPOLL_CTL_FAIL);
        close(epoll_fd);
        close(ser_fd);
        close(event_fd);
        close(tfd);
        exit(EXIT_FAILURE);
    }
    event.data.fd = tfd;
    event.events = EPOLLIN;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,tfd,&event)==-1){
        LOG_FATAL("Epoll_ctl add timerfd failed",ERR_EPOLL_CTL_FAIL);
        close(epoll_fd);
        close(ser_fd);
        close(event_fd);
        close(tfd);
        exit(EXIT_FAILURE);
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
            else if(fd==tfd){
                //心跳检测
                uint64_t exp;
                read(tfd,&exp,sizeof(exp));
                check_timeout();
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
    
    // 在销毁日志系统之前停止所有后台服务，确保日志记录完整
    ParserPool::getInstance()->stop();
    DbHandle::getInstance()->stop();
    
    Logger::destroy();
    return 0;
}
