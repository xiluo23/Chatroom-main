import socket
import threading
import time
import struct

# ===== 配置 =====
HOST = "127.0.0.1"      # 如 "121.41.85.94" 或 "172.26.179.107"
PORT = 9000               # Nginx stream 端口 (9000) 或 C++ 端口 (6000)
CLIENTS = 100             # 并发连接数
DURATION = 10             # 压测持续时间（秒）

# 按你聊天协议构造一条业务消息
PAYLOAD_STR = "heartbeat|"   # C++ 会按 '|' 拆分的文本命令


def build_message(payload: str) -> bytes:
    data = payload.encode("utf-8")
    # 前 4 字节为大端长度前缀（不包含自身长度）
    length = len(data)
    return struct.pack(">I", length) + data


MESSAGE = build_message(PAYLOAD_STR)

total_sent = 0
total_connect_fail = 0
total_send_fail = 0
lock = threading.Lock()


def worker(idx: int):
    global total_sent, total_connect_fail, total_send_fail

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)

    try:
        s.connect((HOST, PORT))
    except Exception as e:
        with lock:
            total_connect_fail += 1
        print(f"[{idx}] connect failed: {e}")
        return

    end_time = time.time() + DURATION

    while time.time() < end_time:
        try:
            s.sendall(MESSAGE)
            with lock:
                total_sent += 1
        except Exception as e:
            with lock:
                total_send_fail += 1
            # 连接断了就结束这个线程
            # print(f"[{idx}] send failed: {e}")
            break

    try:
        s.close()
    except:
        pass


def main():
    global total_sent, total_connect_fail, total_send_fail

    print(f"Start QPS test: {CLIENTS} clients, {DURATION}s, target {HOST}:{PORT}")
    start = time.time()

    threads = []
    for i in range(CLIENTS):
        t = threading.Thread(target=worker, args=(i,), daemon=True)
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    end = time.time()
    duration = end - start if end > start else 1e-6
    qps = total_sent / duration

    print("========== Result ==========")
    print(f"Total messages sent:      {total_sent}")
    print(f"Total connect failures:   {total_connect_fail}")
    print(f"Total send failures:      {total_send_fail}")
    print(f"Total time:               {duration:.2f}s")
    print(f"Approx QPS:               {qps:.2f} msg/s")


if __name__ == "__main__":
    main()