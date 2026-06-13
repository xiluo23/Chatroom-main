#include "redis.h"
#include"Logger.h"
#include <iostream>
#include<string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
using namespace std;

Redis::Redis()
    : _publish_context(nullptr), _subcribe_context(nullptr)
{
}

Redis::~Redis()
{
    _running = false;
    _keepalive_running = false;

    _cmd_cv.notify_all();

    if (_sub_thread.joinable())
    {
        _sub_thread.join();
    }
    
    if (_keepalive_thread.joinable())
    {
        _keepalive_thread.join();
    }

    if (_subcribe_context)
    {
        redisFree(_subcribe_context);
        _subcribe_context = nullptr;
    }

    if (_publish_context)
    {
        redisFree(_publish_context);
        _publish_context = nullptr;
    }

    LOG_INFO("Redis service stopped successfully.");
}

bool Redis::connect()
{
    Config* conf = Config::getInstance();
    _redis_host = conf->getString("redis_host", "127.0.0.1");
    _redis_port = conf->getInt("redis_port", 6379);
    _reconnect_interval_ms = conf->getInt("redis_reconnect_interval", 5000);

    if (!connectInternal()) {
        return false;
    }

    // 启动保活线程
    _keepalive_running = true;
    _keepalive_thread = thread([&] {
        keepAliveLoop();
    });

    LOG_INFO("connect redis-server success!");
    return true;
}

bool Redis::connectInternal()
{
    // 负责publish发布消息和缓存操作的上下文连接
    _publish_context = redisConnect(_redis_host.c_str(), _redis_port);
    if (nullptr == _publish_context || _publish_context->err != 0)
    {
        if (_publish_context) {
            LOG_ERROR("Failed to connect Redis (publish): " + string(_publish_context->errstr));
            redisFree(_publish_context);
            _publish_context = nullptr;
        }
        return false;
    }

    // 负责subscribe订阅消息的上下文连接
    _subcribe_context = redisConnect(_redis_host.c_str(), _redis_port);
    if (nullptr == _subcribe_context || _subcribe_context->err != 0)
    {
        if (_subcribe_context) {
            LOG_ERROR("Failed to connect Redis (subscribe): " + string(_subcribe_context->errstr));
            redisFree(_subcribe_context);
            _subcribe_context = nullptr;
        }
        if (_publish_context) {
            redisFree(_publish_context);
            _publish_context = nullptr;
        }
        return false;
    }
    
    // 设置订阅上下文读取超时，避免长期阻塞无法处理命令队列
    timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
    redisSetTimeout(_subcribe_context, tv);

    // 在单独的线程中，监听通道上的事件，有消息给业务层进行上报
    if (!_running) {
        _running = true;
        _sub_thread = thread([&] {
            observer_channel_message();
        });
    }
    
    return true;
}

bool Redis::isConnected()
{
    if (!_publish_context || _publish_context->err != 0) return false;
    if (!_subcribe_context || _subcribe_context->err != 0) return false;
    
    // 测试连接
    lock_guard<mutex> lock(_cache_mutex);
    redisReply* r = (redisReply*)redisCommand(_publish_context, "PING");
    if (!r) return false;
    bool ok = (r->type == REDIS_REPLY_STATUS && strcasecmp(r->str, "PONG") == 0);
    freeReplyObject(r);
    return ok;
}

bool Redis::reconnect()
{
    LOG_WARN("Attempting to reconnect to Redis...");
    
    // 停止旧的连接
    _running = false;
    if (_sub_thread.joinable()) _sub_thread.join();
    
    if (_publish_context) {
        redisFree(_publish_context);
        _publish_context = nullptr;
    }
    if (_subcribe_context) {
        redisFree(_subcribe_context);
        _subcribe_context = nullptr;
    }
    
    // 重新连接
    return connectInternal();
}

void Redis::keepAliveLoop()
{
    while (_keepalive_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(_reconnect_interval_ms));
        
        if (!isConnected()) {
            LOG_WARN("Redis connection lost, attempting reconnect...");
            if (reconnect()) {
                LOG_INFO("Redis reconnected successfully!");
                // 注意：重连后需要重新订阅之前的频道
                // 这里简化处理，实际项目中应该保存订阅列表并重新订阅
            } else {
                LOG_ERROR("Redis reconnect failed");
            }
        }
    }
}

// 向redis指定的通道channel发布消息
bool Redis::publish(int channel, string message)
{
    if (!_running) return false;
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "PUBLISH %d %s", channel, message.c_str());
    if (nullptr == reply)
    {
        cerr << "publish command failed!" << endl;
        return false;
    }
    freeReplyObject(reply);
    return true;
}

// 向redis指定的通道subscribe订阅消息
bool Redis::subscribe(int channel)
{
    if (!_running) return false;
    // 将命令入队，由订阅线程串行执行，避免跨线程访问 _subcribe_context
    {
        lock_guard<mutex> lk(_cmd_mutex);
        _cmd_queue.push({CmdType::SUB, channel});
    }
    _cmd_cv.notify_one();
    return true; // 入队成功即返回
}

// 向redis指定的通道unsubscribe取消订阅消息
bool Redis::unsubscribe(int channel)
{
    if (!_running) return false;
    {
        lock_guard<mutex> lk(_cmd_mutex);
        _cmd_queue.push({CmdType::UNSUB, channel});
    }
    _cmd_cv.notify_one();
    return true;
}

void Redis::process_pending_commands()
{
    // 执行入队的订阅/退订命令（使用同一个上下文，保证线程安全）
    queue<Cmd> local;
    {
        lock_guard<mutex> lk(_cmd_mutex);
        swap(local, _cmd_queue);
    }
    while (!local.empty())
    {
        Cmd cmd = local.front();
        local.pop();
        if (cmd.type == CmdType::SUB)
        {
            if (REDIS_ERR == redisAppendCommand(this->_subcribe_context, "SUBSCRIBE %d", cmd.channel))
            {
                cerr << "subscribe command failed!" << endl;
                continue;
            }
        }
        else
        {
            if (REDIS_ERR == redisAppendCommand(this->_subcribe_context, "UNSUBSCRIBE %d", cmd.channel))
            {
                cerr << "unsubscribe command failed!" << endl;
                continue;
            }
        }
        int done = 0;
        while (!done)
        {
            if (REDIS_ERR == redisBufferWrite(this->_subcribe_context, &done))
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // 资源暂时不可用（超时或缓冲区满），稍后重试
                    // 必须重置错误标志，否则 hiredis 下次调用会直接返回错误
                    this->_subcribe_context->err = 0;
                    usleep(1000); // 1ms
                    continue;
                }
                cerr << "redis buffer write failed: " << this->_subcribe_context->errstr << endl;
                return; // 连接断开，停止处理后续命令
            }
        }
    }
}

// 在独立线程中接收订阅通道中的消息
void Redis::observer_channel_message()
{
    redisReply *reply = nullptr;
    while (_running)
    {
        // 先处理待执行命令（SUB/UNSUB）
        process_pending_commands();

        // 尝试获取一条消息（设置了超时，不会永久阻塞）
        int rc = redisGetReply(this->_subcribe_context, (void **)&reply);
        if (rc == REDIS_OK)
        {
            if (reply == nullptr) continue;

            if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3)
            {
                if (reply->element[0] && reply->element[0]->type == REDIS_REPLY_STRING && reply->element[0]->str)
                {
                    const char* type_str = reply->element[0]->str;
                    
                    if (strcasecmp(type_str, "message") == 0)
                    {
                        if (reply->element[1] && reply->element[1]->type == REDIS_REPLY_STRING && reply->element[1]->str &&
                            reply->element[2] && reply->element[2]->type == REDIS_REPLY_STRING && reply->element[2]->str)
                        {
                            _notify_message_handler(atoi(reply->element[1]->str), reply->element[2]->str);
                        }
                    }
                    else if (strcasecmp(type_str, "subscribe") == 0 || strcasecmp(type_str, "unsubscribe") == 0)
                    {
                        LOG_DEBUG("Redis command confirmed: " + string(type_str));
                    }
                }
            }
            
            freeReplyObject(reply);
            reply = nullptr;
        }
        else
        {
            if (this->_subcribe_context->err != REDIS_ERR_IO && this->_subcribe_context->err != REDIS_ERR_EOF)
            {
                cerr << "redisGetReply error: " << this->_subcribe_context->errstr << endl;
                break;
            }
            if (this->_subcribe_context->err == REDIS_ERR_IO)
            {
                this->_subcribe_context->err = 0;
            }
            unique_lock<mutex> lk(_cmd_mutex);
            _cmd_cv.wait_for(lk, chrono::milliseconds(50));
        }
    }

    cerr << ">>>>>>>>>>>>> observer_channel_message quit <<<<<<<<<<<<<" << endl;
}

void Redis::init_notify_handler(function<void(int,string)> fn)
{
    this->_notify_message_handler = fn;
}

// ==================== 缓存操作实现 ====================

bool Redis::set(const string &key, const string &value, int expire_sec)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply;
    if (expire_sec > 0)
    {
        reply = (redisReply *)redisCommand(_publish_context, "SET %s %s EX %d", key.c_str(), value.c_str(), expire_sec);
    }
    else
    {
        reply = (redisReply *)redisCommand(_publish_context, "SET %s %s", key.c_str(), value.c_str());
    }
    if (reply == nullptr)
    {
        LOG_ERROR("Redis SET command failed for key: " + key,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_STATUS && strcasecmp(reply->str, "OK") == 0);
    freeReplyObject(reply);
    return success;
}

string Redis::get(const string &key)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "GET %s", key.c_str());
    if (reply == nullptr)
    {
        LOG_ERROR("Redis GET command failed for key: " + key, ERR_REDIS);
        return "";
    }
    string result;
    if (reply->type == REDIS_REPLY_STRING)
    {
        result = reply->str;
    }
    freeReplyObject(reply);
    return result;
}

bool Redis::del(const string &key)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "DEL %s", key.c_str());
    if (reply == nullptr)
    {
        LOG_ERROR("Redis DEL command failed for key: " + key,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_INTEGER && reply->integer >= 0);
    freeReplyObject(reply);
    return success;
}

bool Redis::expire(const string &key, int expire_sec)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "EXPIRE %s %d", key.c_str(), expire_sec);
    if (reply == nullptr)
    {
        LOG_ERROR("Redis EXPIRE command failed for key: " + key,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);
    return success;
}

bool Redis::exists(const string &key)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "EXISTS %s", key.c_str());
    if (reply == nullptr)
    {
        return false;
    }
    bool exists = (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);
    return exists;
}

bool Redis::hset(const string &key, const string &field, const string &value)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "HSET %s %s %s", key.c_str(), field.c_str(), value.c_str());
    if (reply == nullptr)
    {
        LOG_ERROR("Redis HSET command failed for key: " + key + ", field: " + field,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return success;
}

string Redis::hget(const string &key, const string &field)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "HGET %s %s", key.c_str(), field.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_STRING)
    {
        if (reply) freeReplyObject(reply);
        return "";
    }
    string result = reply->str;
    freeReplyObject(reply);
    return result;
}

unordered_map<string, string> Redis::hgetall(const string &key)
{
    lock_guard<mutex> lk(_cache_mutex);
    unordered_map<string, string> result;
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "HGETALL %s", key.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY)
    {
        if (reply) freeReplyObject(reply);
        return result;
    }
    for (size_t i = 0; i < reply->elements; i += 2)
    {
        if (i + 1 < reply->elements && 
            reply->element[i]->type == REDIS_REPLY_STRING && 
            reply->element[i + 1]->type == REDIS_REPLY_STRING)
        {
            result[reply->element[i]->str] = reply->element[i + 1]->str;
        }
    }
    freeReplyObject(reply);
    return result;
}

bool Redis::hdel(const string &key, const string &field)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "HDEL %s %s", key.c_str(), field.c_str());
    if (reply == nullptr)
    {
        LOG_ERROR("Redis HDEL command failed for key: " + key + ", field: " + field,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_INTEGER && reply->integer >= 0);
    freeReplyObject(reply);
    return success;
}

bool Redis::hexists(const string &key, const string &field)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "HEXISTS %s %s", key.c_str(), field.c_str());
    if (reply == nullptr)
    {
        return false;
    }
    bool exists = (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);
    return exists;
}

bool Redis::sadd(const string &key, const string &value)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "SADD %s %s", key.c_str(), value.c_str());
    if (reply == nullptr)
    {
        LOG_ERROR("Redis SADD command failed for key: " + key,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return success;
}

bool Redis::srem(const string &key, const string &value)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "SREM %s %s", key.c_str(), value.c_str());
    if (reply == nullptr)
    {
        LOG_ERROR("Redis SREM command failed for key: " + key,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return success;
}

bool Redis::sismember(const string &key, const string &value)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "SISMEMBER %s %s", key.c_str(), value.c_str());
    if (reply == nullptr)
    {
        return false;
    }
    bool is_member = (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);
    return is_member;
}

vector<string> Redis::smembers(const string &key)
{
    lock_guard<mutex> lk(_cache_mutex);
    vector<string> result;
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "SMEMBERS %s", key.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY)
    {
        if (reply) freeReplyObject(reply);
        return result;
    }
    for (size_t i = 0; i < reply->elements; i++)
    {
        if (reply->element[i]->type == REDIS_REPLY_STRING)
        {
            result.push_back(reply->element[i]->str);
        }
    }
    freeReplyObject(reply);
    return result;
}

bool Redis::lpush(const string &key, const string &value)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "LPUSH %s %s", key.c_str(), value.c_str());
    if (reply == nullptr)
    {
        LOG_ERROR("Redis LPUSH command failed for key: " + key,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return success;
}

bool Redis::rpush(const string &key, const string &value)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "RPUSH %s %s", key.c_str(), value.c_str());
    if (reply == nullptr)
    {
        LOG_ERROR("Redis RPUSH command failed for key: " + key,ERR_REDIS);
        return false;
    }
    bool success = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return success;
}

vector<string> Redis::lrange(const string &key, int start, int stop)
{
    lock_guard<mutex> lk(_cache_mutex);
    vector<string> result;
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "LRANGE %s %d %d", key.c_str(), start, stop);
    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY)
    {
        if (reply) freeReplyObject(reply);
        return result;
    }
    for (size_t i = 0; i < reply->elements; i++)
    {
        if (reply->element[i]->type == REDIS_REPLY_STRING)
        {
            result.push_back(reply->element[i]->str);
        }
    }
    freeReplyObject(reply);
    return result;
}

int Redis::llen(const string &key)
{
    lock_guard<mutex> lk(_cache_mutex);
    redisReply *reply = (redisReply *)redisCommand(_publish_context, "LLEN %s", key.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER)
    {
        if (reply) freeReplyObject(reply);
        return 0;
    }
    int len = reply->integer;
    freeReplyObject(reply);
    return len;
}



