import socket
import threading
import time

# ===== 配置区域 =====
NGINX_HOST = "A公网IP或内网IP"  # 比如 "1.2.3.4" 或 "172.26.179.107"
NGINX_PORT = 9000               # Nginx stream 监听端口
CLIENT_COUNT = 50               # 模拟连接数，可以根据需要调大
HOLD_SECONDS = 10               # 每个连接保持的时间
SLEEP_BETWEEN = 0.05            # 每个连接之间的间隔，防止一下子打爆


def client_worker(idx):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect((NGINX_HOST, NGINX_PORT))
        print(f"[{idx}] Connected to {NGINX_HOST}:{NGINX_PORT}")
        # 不必发业务数据，只要建立 TCP 连接，Nginx 就会分配到某个后端
        # 如果你想让服务器日志更明显，可以发一条无害消息（长度头+内容）
        time.sleep(HOLD_SECONDS)
    except Exception as e:
        print(f"[{idx}] Connection failed: {e}")
    finally:
        try:
            s.close()
        except:
            pass


def main():
    threads = []
    for i in range(CLIENT_COUNT):
        t = threading.Thread(target=client_worker, args=(i,))
        t.start()
        threads.append(t)
        time.sleep(SLEEP_BETWEEN)

    for t in threads:
        t.join()

    print("All clients finished.")


if __name__ == "__main__":
    main()