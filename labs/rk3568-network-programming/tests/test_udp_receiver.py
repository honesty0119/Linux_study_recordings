"""Windows UDP 回环：数据报边界、非法来源/坏包、按键不延期、心跳恢复与源端口变化。"""
import pathlib
import re
import socket
import subprocess
import sys
import time


def frame(kind):
    payload = bytearray([kind] + ([0, 1] if kind == 2 else []) + [0x12, 0x34, 0x56, 0x78])
    checksum = 0
    for b in payload:
        checksum ^= b
    return bytes(payload + bytes([checksum]))


def main():
    exe, logname = sys.argv[1:3]
    log = pathlib.Path(logname)
    log.parent.mkdir(parents=True, exist_ok=True)
    assert not log.exists(), 'Choose a fresh log file'
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    process = subprocess.Popen([exe, '127.0.0.1', str(port), '7', '--udp', '--peer', '127.0.0.1', '--log', str(log)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    good = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    bad = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    bad.bind(('127.0.0.2', 0))
    destination = ('127.0.0.1', port)
    try:
        deadline = time.monotonic()+3
        while not log.exists() or '监听成功 UDP' not in log.read_text(encoding='utf-8'):
            if time.monotonic() > deadline or process.poll() is not None:
                raise AssertionError('Listener not ready')
            time.sleep(.02)
        heartbeat, key = frame(1), frame(2)
        good.sendto(heartbeat, destination)
        invalid = [b'', heartbeat[:3], heartbeat[3:], heartbeat+heartbeat, heartbeat+b'X',
                   bytes([3, 0, 0, 0, 0, 3]), heartbeat[:-1]+b'\0',
                   bytes([2, 1, 1, 0, 0, 0, 0, 2]), b'x'*60000]
        for data in invalid:
            good.sendto(data, destination)
        count = 0
        stop = time.monotonic()+2.35
        while time.monotonic() < stop:
            good.sendto(key, destination)
            bad.sendto(heartbeat, destination)
            count += 1
            time.sleep(.1)
        assert '心跳超时' in log.read_text(encoding='utf-8'), 'Key/bad peer masked timeout'
        good.close()
        good = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        good.sendto(heartbeat, destination)  # 板端进程重启后源端口可变化。
        good.sendto(key, destination)
        count += 1
        _, error = process.communicate(timeout=10)
        assert process.returncode == 0, error
        text = log.read_text(encoding='utf-8')
        summary = re.search(r'Summary transport=UDP heartbeats=(\d+) keys=(\d+) invalid_datagrams=(\d+) ignored_peer=(\d+)', text)
        assert summary and tuple(map(int, summary.groups())) == (2, count, len(invalid), count-1), text
        assert '心跳恢复' in text, text
        timeouts = [float(v) for v in re.findall(r'elapsed_ms=([0-9.]+)', text)]
        assert len(timeouts) == 2 and all(2000 < v < 2300 for v in timeouts), timeouts
        complete = re.search(r'LogSummary accepted=(\d+) written=(\d+) frame_logs=(\d+)', text)
        assert complete and complete[1] == complete[2] and int(complete[3]) == count+2, text
        print('udp-receiver: PASS; heartbeat=2 keys=%d invalid=%d ignored_peer=%d timeout_ms=%s' %
              (count, len(invalid), count-1, timeouts))
    finally:
        good.close()
        bad.close()
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=3)


if __name__ == '__main__':
    main()
