import asyncio
import argparse
import struct
import time
import random
import string
try:
    import resource
except ImportError:
    resource = None


def encode_message(payload: str) -> bytes:
    """
    按照 C++ 里的 Protocol.h 协议编码:
    [4 字节大端长度][payload]
    """
    data = payload.encode("utf-8")
    return struct.pack("!I", len(data)) + data


async def send_and_maybe_recv(reader: asyncio.StreamReader,
                              writer: asyncio.StreamWriter,
                              payload: str,
                              need_resp: bool = False,
                              timeout: float = 30.0,
                              expected_cmd: str | None = None) -> str | None:
    """
    发送一条协议消息，可选等待一条响应。
    如果指定了 expected_cmd，则会自动丢弃不匹配的消息（如广播、离线消息），
    直到收到匹配前缀的消息或超时。
    """
    try:
        writer.write(encode_message(payload))
        await writer.drain()
        if not need_resp:
            return None

        start_time = time.time()
        while True:
            # 计算剩余超时时间
            now = time.time()
            remain = timeout - (now - start_time)
            if remain <= 0:
                return None

            # 先读 4 字节长度
            header = await asyncio.wait_for(reader.readexactly(4), timeout=remain)
            (length,) = struct.unpack("!I", header)
            
            if length <= 0 or length > 4096:
                return None

            # 再次检查时间，读取 body
            now = time.time()
            remain = timeout - (now - start_time)
            if remain <= 0:
                return None
            
            body_bytes = await asyncio.wait_for(reader.readexactly(length), timeout=remain)
            body = body_bytes.decode("utf-8", errors="ignore")

            # 如果没有指定期待的指令，直接返回（兼容旧行为）
            if not expected_cmd:
                return body

            # 检查前缀是否匹配
            if body.startswith(expected_cmd):
                return body
            
            # 不匹配则忽略（可能是广播消息或离线消息），继续循环读取下一条
            # print(f"Ignored unexpected message: {body}")

    except Exception:
        return None


async def create_user_and_login(host: str,
                                port: int,
                                user_index: int,
                                results: dict,
                                need_signup: bool,
                                heartbeat_interval: int | None):
    """
    建立一个完整业务连接：
    - 可选：注册用户 sign_up
    - 登录 sign_in
    - 循环发送 heartbeat 维持在线
    """
    username = f"stress_new18_{user_index}"
    password = "123456"

    try:
        reader, writer = await asyncio.open_connection(host, port)
    except Exception as e:
        results["connect_fail"] += 1
        # print(f"[{user_index}] connect failed: {e}")
        return

    async def close():
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

    try:
        # 注册（只在第一次压测时需要，已有用户可以关闭 need_signup）
        if need_signup:
            resp = await send_and_maybe_recv(
                reader, writer,
                f"sign_up|{username}|{password}",
                need_resp=True,
                expected_cmd="sign_up"
            )
            # 允许"用户名重复"，因此这里只在严重错误时计为失败
            if resp is None or resp.startswith("sign_up|0|") and "用户名重复" not in resp:
                if results["signup_fail"] < 5: # 只打印前几个错误
                    print(f"[{user_index}] signup fail: resp={resp}")
                results["signup_fail"] += 1
                await close()
                return

        # 登录
        resp = await send_and_maybe_recv(
            reader, writer,
            f"sign_in|{username}|{password}",
            need_resp=True,
            expected_cmd="sign_in"
        )
        if resp is None or not resp.startswith("sign_in|1|"):
            if results["login_fail"] < 5:
                print(f"[{user_index}] login fail: resp={resp}")
            results["login_fail"] += 1
            await close()
            return

        results["online_success"] += 1

        # 给前一个用户发送一条消息
        if user_index > 0:
            target_user = f"stress_new6_{user_index - 1}"
            msg = f"hello from {username}"
            chat_payload = f"single_chat|{target_user}|{msg}"
            await send_and_maybe_recv(
                reader, writer,
                chat_payload,
                need_resp=True,
                expected_cmd="single_chat"
            )

        # 广播消息测试 (仅 user_index=0 发送，避免风暴)
        if user_index == 0:
            broadcast_msg = f"broadcast_from_{username}"
            broadcast_payload = f"broadcast_chat|{broadcast_msg}"
            # 期待收到确认 "broadcast_chat|1|..."
            await send_and_maybe_recv(
                reader, writer,
                broadcast_payload,
                need_resp=True,
                expected_cmd="broadcast_chat"
            )

        # 启动心跳循环，维持“在线通信”状态
        if heartbeat_interval is not None and heartbeat_interval > 0:
            while True:
                await asyncio.sleep(heartbeat_interval)
                ok = await send_and_maybe_recv(
                    reader, writer,
                    "heartbeat",
                    need_resp=True,
                    expected_cmd="heartbeat"
                )
                if ok is None or not ok.startswith("heartbeat|1|"):
                    # 心跳失败视为下线
                    break

    except Exception:
        # 任意异常都视为失败下线
        pass
    finally:
        await close()


async def stress_test(host: str,
                      port: int,
                      clients: int,
                      heartbeat_interval: int,
                      batch_size: int,
                      need_signup: bool):
    """
    总体压测逻辑：
    - 分批创建 N 个“业务完整”的客户端（登录 + 心跳）
    - 可通过观察 online_success 数判断最大在线能力
    """
    results = {
        "connect_fail": 0,
        "signup_fail": 0,
        "login_fail": 0,
        "online_success": 0,
    }

    start = time.time()
    print(f"开始压测: host={host}, port={port}, 目标并发客户端={clients}")
    print(f"心跳间隔={heartbeat_interval}s, 批大小={batch_size}, 是否注册新用户={need_signup}")

    tasks: list[asyncio.Task] = []
    for base in range(0, clients, batch_size):
        cur = min(batch_size, clients - base)
        for i in range(cur):
            user_index = base + i
            t = asyncio.create_task(
                create_user_and_login(
                    host,
                    port,
                    user_index,
                    results,
                    need_signup=need_signup,
                    heartbeat_interval=heartbeat_interval,
                )
            )
            tasks.append(t)

        # 每批之间稍微停顿，避免压垮本机/服务器瞬时
        await asyncio.sleep(0.2)
        print(
            f"已发起客户端数: {base + cur}, "
            f"当前在线成功={results['online_success']}, "
            f"连接失败={results['connect_fail']}, "
            f"注册失败={results['signup_fail']}, "
            f"登录失败={results['login_fail']}"
        )

    # 所有客户端任务开始运行，等待它们自然结束（例如因为心跳失败/服务器关闭）
    print("所有客户端已发起，正在运行中，按 Ctrl+C 可停止测试...")
    try:
        await asyncio.gather(*tasks)
    except asyncio.CancelledError:
        pass

    cost = time.time() - start
    print("\n==== 压测结束 ====")
    print(f"总耗时: {cost:.2f}s")
    print(
        f"连接失败={results['connect_fail']}, "
        f"注册失败={results['signup_fail']}, "
        f"登录失败={results['login_fail']}, "
        f"成功在线客户端数={results['online_success']}"
    )


def main():
    parser = argparse.ArgumentParser(
        description="Chatroom 最大在线并发客户端压测脚本（带登录+心跳）"
    )
    parser.add_argument("host", help="服务器 IP")
    parser.add_argument("port", type=int, help="服务器端口")
    parser.add_argument(
        "--clients", type=int, default=5000, help="目标并发在线客户端数量"
    )
    parser.add_argument(
        "--heartbeat-interval",
        type=int,
        default=15,
        help="心跳间隔秒数（需小于服务器超时阈值 40s）",
    )
    parser.add_argument(
        "--batch-size", type=int, default=10, help="每批次创建的客户端数量"
    )
    parser.add_argument(
        "--no-signup",
        action="store_true",
        help="不执行 sign_up（假设这些 stress_user_x 已存在）",
    )

    args = parser.parse_args()
    need_signup = not args.no_signup

    # Check ulimit
    if resource:
        try:
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            print(f"Current RLIMIT_NOFILE: soft={soft}, hard={hard}")
            if soft < 65535:
                target = hard
                if target == resource.RLIM_INFINITY:
                    target = 65535
                try:
                    resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
                    print(f"Increased RLIMIT_NOFILE to {target}")
                except Exception as e:
                    print(f"Failed to increase RLIMIT_NOFILE: {e}")
        except Exception as e:
            print(f"Error checking rlimit: {e}")
    else:
        print("resource module not available (Windows?), skipping ulimit check.")

    asyncio.run(
        stress_test(
            args.host,
            args.port,
            args.clients,
            args.heartbeat_interval,
            args.batch_size,
            need_signup,
        )
    )


if __name__ == "__main__":
    main()


