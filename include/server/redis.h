#ifndef REDIS_H
#define REDIS_H

#include <hiredis/hiredis.h>
#include <thread>
#include <functional>
#include<string>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <unordered_map>
#include "Config.h"
#include"Logger.h"
using namespace std;

/*
redis作为集群服务器通信的基于发布-订阅消息队列和缓存层
*/
class Redis
{
public:
    Redis();
    ~Redis();

    // 连接redis服务器 
    bool connect();

    // 向redis指定的通道channel发布消息
    bool publish(int channel, string message);

    // 向redis指定的通道subscribe订阅消息
    bool subscribe(int channel);

    // 向redis指定的通道unsubscribe取消订阅消息
    bool unsubscribe(int channel);

    // 在独立线程中接收订阅通道中的消息
    void observer_channel_message();

    // 初始化向业务层上报通道消息的回调对象
    void init_notify_handler(function<void(int, string)> fn);

    // 检查连接健康状态
    bool isConnected();
    
    // 手动触发重连
    bool reconnect();

    // ==================== 缓存操作接口 ====================
    
    string get(const string &key);
    bool set(const string &key, const string &value, int expire_sec = -1);
    bool del(const string &key);
    bool expire(const string &key, int expire_sec);
    bool exists(const string &key);
    
    // Hash 操作
    bool hset(const string &key, const string &field, const string &value);
    string hget(const string &key, const string &field);
    unordered_map<string, string> hgetall(const string &key);
    bool hdel(const string &key, const string &field);
    bool hexists(const string &key, const string &field);
    
    // Set 操作
    bool sadd(const string &key, const string &value);
    bool srem(const string &key, const string &value);
    bool sismember(const string &key, const string &value);
    vector<string> smembers(const string &key);
    
    // List 操作
    bool lpush(const string &key, const string &value);
    bool rpush(const string &key, const string &value);
    vector<string> lrange(const string &key, int start, int stop);
    int llen(const string &key);

private:
    enum class CmdType { SUB, UNSUB };
    struct Cmd { CmdType type; int channel; };

    // hiredis同步上下文对象，负责publish消息和缓存操作
    redisContext *_publish_context;

    // hiredis同步上下文对象，负责subscribe消息
    redisContext *_subcribe_context;

    // 回调操作，收到订阅的消息，给service层上报
    function<void(int, string)> _notify_message_handler;
    
    // 订阅/退订命令队列，仅在订阅线程中串行执行，保证 _subcribe_context 线程安全
    queue<Cmd> _cmd_queue;
    mutex _cmd_mutex;
    condition_variable _cmd_cv;
    atomic<bool> _running{false};
    thread _sub_thread; // 订阅线程对象
    
    // 缓存操作的互斥锁
    mutex _cache_mutex;
    
    // 重连相关
    std::string _redis_host;
    int _redis_port;
    int _reconnect_interval_ms = 5000;  // 5秒重连间隔
    std::thread _keepalive_thread;
    std::atomic<bool> _keepalive_running{false};
    
    void process_pending_commands();
    void keepAliveLoop();
};

#endif
