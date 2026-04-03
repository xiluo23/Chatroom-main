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
    // 这里不再 join 线程，因为它可能在 redisGetReply 中阻塞
    // 但通过设置 _running = false 和关闭上下文，可以强制其退出
    
    if (_publish_context != nullptr)
    {
        redisFree(_publish_context);
    }

    if (_subcribe_context != nullptr)
    {
        // 关闭上下文会使阻塞的 redisGetReply 立即返回错误
        redisFree(_subcribe_context);
    }
}

bool Redis::connect()
{
    Config* conf = Config::getInstance();
    string host = conf->getString("redis_host", "127.0.0.1");
    int port = conf->getInt("redis_port", 6379);

    // 负责publish发布消息的上下文连接
    _publish_context = redisConnect(host.c_str(), port);
    if (nullptr == _publish_context)
    {
        cerr << "connect redis failed!" << endl;
        return false;
    }

    // 负责subscribe订阅消息的上下文连接
    _subcribe_context = redisConnect(host.c_str(), port);
    if (nullptr == _subcribe_context)
    {
        cerr << "connect redis failed!" << endl;
        return false;
    }
    // 设置订阅上下文读取超时，避免长期阻塞无法处理命令队列
    timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
    redisSetTimeout(_subcribe_context, tv);

    // 在单独的线程中，监听通道上的事件，有消息给业务层进行上报
    _running = true;
    thread t([&]() {
        observer_channel_message();
    });
    t.detach();

    LOG_INFO("connect redis-server success!");

    return true;
}

// 向redis指定的通道channel发布消息
bool Redis::publish(int channel, string message)
{
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
        
        // 移除这里的 redisGetReply。
        // 我们在 observer_channel_message 的主循环中统一处理所有回复。
        // 这样可以避免在消息和确认同时到达时发生回复消耗错位（同步错误）。
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

            // 订阅收到的消息是一个带三个元素的数组
            // 无论是 "message" (业务数据), "subscribe" (确认), 还是 "unsubscribe" (确认)
            if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3)
            {
                // 必须严格检查每个 element 的类型，防止非字符串类型导致内存损坏
                if (reply->element[0] && reply->element[0]->type == REDIS_REPLY_STRING && reply->element[0]->str)
                {
                    const char* type_str = reply->element[0]->str;
                    
                    if (strcasecmp(type_str, "message") == 0)
                    {
                        // 业务消息：[ "message", channel, payload ]
                        if (reply->element[1] && reply->element[1]->type == REDIS_REPLY_STRING && reply->element[1]->str &&
                            reply->element[2] && reply->element[2]->type == REDIS_REPLY_STRING && reply->element[2]->str)
                        {
                            _notify_message_handler(atoi(reply->element[1]->str), reply->element[2]->str);
                        }
                    }
                    else if (strcasecmp(type_str, "subscribe") == 0 || strcasecmp(type_str, "unsubscribe") == 0)
                    {
                        // 确认消息：[ "subscribe"/"unsubscribe", channel, total_count ]
                        // element[1] 是字符串，但 element[2] 是整数。
                        // 我们只需要消费掉它，不需要额外处理。
                        LOG_DEBUG("Redis command confirmed: " + string(type_str));
                    }
                }
            }
            
            freeReplyObject(reply);
            reply = nullptr;
        }
        else
        {
            // 读取失败（可能是超时或暂时性错误），继续下一轮
            if (this->_subcribe_context->err != REDIS_ERR_IO && this->_subcribe_context->err != REDIS_ERR_EOF)
            {
                // 严重错误（如协议错误），退出循环
                cerr << "redisGetReply error: " << this->_subcribe_context->errstr << endl;
                break;
            }
            //如果是超时，重置错误标志
            if (this->_subcribe_context->err == REDIS_ERR_IO)
            {
                this->_subcribe_context->err = 0;
            }
            // 留出时间让其他线程入队命令
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
