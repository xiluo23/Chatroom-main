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
#include<errno.h>
#include<unordered_set>
#include<unordered_map>
#include<pthread.h>
#include"MyDb.h"
#include"Protocol.h"
#include<queue>
#include<vector>
#include<crypt.h>
#include<sys/eventfd.h>
#include<map>

#define BUF_SIZE 4096      // 接收缓冲区大小（4KB，对齐协议最大消息）
#define CLINT_SIZE 1000
#define MAX_EVENTS 1024
#define PORT 3306
#define HOST "192.168.147.130"
#define USER "ftpuser"
#define DB_NAME "Chatroom"
#define PWD "926472"

struct Task{
    int fd;//clinent_fd
    string message;
};
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

class ThreadPool{
private:
    queue<Task>tasks;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    vector<pthread_t>workers;
    queue<MyDb*>db_pool;
    bool stop;
    pthread_mutex_t db_mutex;
    static void* worker(void*arg);
public:
    ThreadPool(int thread_num);
    ~ThreadPool();
    void addTask(Task task);
    MyDb* get_conn();
    void en_conn(MyDb* conn);
};

MyDb* ThreadPool::get_conn(){
    pthread_mutex_lock(&db_mutex);
    
    if(db_pool.empty()){
        pthread_mutex_unlock(&db_mutex);
        cerr << "Error: No database connection available in pool!" << endl;
        return nullptr;
    }
    
    MyDb* conn = db_pool.front();
    db_pool.pop();
    pthread_mutex_unlock(&db_mutex);
    
    if(conn == nullptr){
        cerr << "Error: Retrieved null connection from pool!" << endl;
        return nullptr;
    }
    return conn;
}
void ThreadPool::en_conn(MyDb*conn){
    pthread_mutex_lock(&db_mutex);
    db_pool.push(conn);
    pthread_mutex_unlock(&db_mutex);
}

ThreadPool::ThreadPool(int thread_num){
    stop=false;
    pthread_mutex_init(&mutex,NULL);
    pthread_cond_init(&cond,NULL);
    pthread_mutex_init(&db_mutex,NULL);
    
    // 第一步：创建并初始化所有数据库连接
    // 确保所有连接都准备就绪后再创建工作线程
    for(int i=0; i<thread_num; i++){
        MyDb* conn = new MyDb();
        bool init_success = conn->initDB(HOST, USER, PWD, DB_NAME, 3306);
        if(!init_success){
            cerr << "Failed to init database connection " << i << endl;
            delete conn;
            exit(1);
        }
        db_pool.push(conn);
    }
    
    // 第二步：所有数据库连接初始化完成后，创建工作线程
    pthread_t t_id;
    for(int i=0; i<thread_num; i++){
        pthread_create(&t_id, NULL, worker, this);
        workers.push_back(t_id);
    }
}
void* ThreadPool::worker(void*arg){
    ThreadPool*pool=(ThreadPool*)arg;
    while(1){
        Task task;
        pthread_mutex_lock(&pool->mutex);
        while(!pool->stop&&pool->tasks.empty()){
            pthread_cond_wait(&pool->cond,&pool->mutex);
        }
        if(pool->stop&&pool->tasks.empty()){
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        task=pool->tasks.front();
        pool->tasks.pop();
        pthread_mutex_unlock(&pool->mutex);
        process_clint_data(task);
    }
    return NULL;
}
void ThreadPool::addTask(Task task){
    pthread_mutex_lock(&mutex);
    tasks.push(task);
    pthread_mutex_unlock(&mutex);
    pthread_cond_signal(&cond);
}

ThreadPool::~ThreadPool(){
    pthread_mutex_lock(&mutex);//保证线程可读到最新的stop
    stop=true;
    pthread_mutex_unlock(&mutex);
    pthread_cond_broadcast(&cond);//唤醒所有线程，结束了
    for(auto&t_id:workers){
        pthread_join(t_id,NULL);
    }
    while(!db_pool.empty()){
        MyDb*conn=db_pool.front();
        db_pool.pop();
        delete conn;
    }
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
}



// 连接守卫类 - 自动管理连接的获取和释放（RAII模式）
class DbConnectionGuard {
private:
    MyDb* conn;
    ThreadPool* pool;
    bool owned;
public:
    DbConnectionGuard(ThreadPool* p) : pool(p), owned(true) {
        conn = pool->get_conn();
        if(!conn){
            cerr << "Failed to acquire database connection" << endl;
            owned = false;
        }
    }
    
    ~DbConnectionGuard() {
        if(owned && conn != nullptr){
            pool->en_conn(conn);
        }
    }
    
    MyDb* get() const {
        return conn;
    }
    
    bool is_valid() const {
        return conn != nullptr;
    }
};




