"""Windows 本地真实 TCP 集成测试；合成数据不能替代 RK3568 物理按键验收。"""
import pathlib
import re
import socket
import subprocess
import sys
import threading
import time


def main():
    exe, logfile = sys.argv[1:3]
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    process = subprocess.Popen(
        [exe, "127.0.0.1", str(port), "9"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        encoding="utf-8", errors="strict",
    )
    lines = []

    def collect():
        for line in process.stdout:
            lines.append(line)

    reader = threading.Thread(target=collect)
    reader.start()
    try:
        deadline = time.monotonic() + 4
        while True:
            try:
                client = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        heartbeat = bytes.fromhex("01 12 34 56 78 09")
        key = bytes.fromhex("02 00 01 12 34 56 78 0b")
        with client:
            both = heartbeat + key
            for part in (both[:2], both[2:9], both[9:]):
                client.sendall(part)
                time.sleep(0.02)
            # 即使持续收到按键，没有心跳也必须约 2s 报警。
            for _ in range(25):
                client.sendall(key)
                time.sleep(0.1)
            client.sendall(heartbeat)
            time.sleep(0.15)
            client.sendall(bytes.fromhex("01 00 00 00 00 00"))
            client.settimeout(1)
            assert client.recv(1) == b"", "坏 XOR 后必须关闭连接"
        with socket.create_connection(("127.0.0.1", port), timeout=1) as client:
            client.sendall(heartbeat)
            # 保持旧 TCP 存活但停发心跳，模拟无 FIN 的断网遗留连接。
            time.sleep(2.2)
            with socket.create_connection(("127.0.0.1", port), timeout=1) as replacement:
                replacement.sendall(heartbeat)
                client.settimeout(1)
                assert client.recv(1) == b"", "超时旧连接不得阻塞新连接"
                time.sleep(0.1)
        assert process.wait(timeout=10) == 0
        reader.join(timeout=2)
        output = "".join(lines)
        pathlib.Path(logfile).write_text(output, encoding="utf-8")
        assert "heartbeats=4 keys=26" in output, output
        assert "XOR 校验失败" in output
        assert "心跳恢复" in output
        delays = [float(v) for v in re.findall(r"elapsed_ms=([\d.]+)", output)]
        assert len(delays) >= 2, delays
        assert all(2000 < value <= 2100 for value in delays), delays
        assert output.count("已连接") == 3
        assert "新连接替换心跳超时的旧连接" in output
        print("receiver TCP integration PASS: fragments, coalescing, 26 keys, "
              "bad XOR disconnect, reconnect, timeout despite keys, recovery")
        print("timeout_ms:", delays)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        reader.join(timeout=2)
        pathlib.Path(logfile).write_text("".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
