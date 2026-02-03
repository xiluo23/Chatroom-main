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
    if (_publish_context != nullptr)
    {
        redisFree(_publish_context);
    }

    if (_subcribe_context != nullptr)
    {
        redisFree(_subcribe_context);
    }
}

bool Redis::connect()
{
    // 负责publish发布消息的上下文连接
    _publish_context = redisConnect("127.0.0.1", 6379);
    if (nullptr == _publish_context)
    {
        cerr << "connect redis failed!" << endl;
        return false;
    }

    // 负责subscribe订阅消息的上下文连接
    _subcribe_context = redisConnect("127.0.0.1", 6379);
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
            // 订阅收到的消息是一个带三元素的数组
            if (reply != nullptr && reply->type == REDIS_REPLY_ARRAY && reply->elements == 3)
            {
                // 只有当消息类型为 "message" 时才处理业务逻辑
                // "subscribe" 和 "unsubscribe" 消息的 element[2] 是整数，访问 str 会导致 crash
                if (reply->element[0] && reply->element[0]->str && 
                    strcasecmp(reply->element[0]->str, "message") == 0 &&
                    reply->element[1] && reply->element[1]->str &&
                    reply->element[2] && reply->element[2]->str)
                {
                    _notify_message_handler(atoi(reply->element[1]->str) , reply->element[2]->str);
                }
            }
            if (reply) freeReplyObject(reply);
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
