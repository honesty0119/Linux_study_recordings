"""Ubuntu/板端 Linux：真实 UDP 回环，验证监听晚启动、停止/恢复和单帧边界。
不改变接口/路由，使用系统分配的回环端口；仅模拟事件，不冒充物理按键。
"""
import pathlib
import re
import socket
import subprocess
import sys
import time


def listen(port=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', port))
    s.settimeout(.04)
    return s


def main():
    exe = str(pathlib.Path(sys.argv[1]).resolve())
    reserve = listen()
    port = reserve.getsockname()[1]
    reserve.close()
    process = subprocess.Popen([exe, '127.0.0.1', str(port), '--synthetic', '7'],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    start = time.monotonic()
    receiver = None
    frames = []
    try:
        while process.poll() is None:
            elapsed = time.monotonic() - start
            if elapsed > 12:
                raise AssertionError('Sender hung')
            phase = 1 if 1.2 <= elapsed < 3.2 else 2 if elapsed >= 4.4 else 0
            if phase and receiver is None:
                receiver = listen(port)
            elif not phase and receiver is not None:
                receiver.close()
                receiver = None
            if receiver is None:
                time.sleep(.02)
                continue
            try:
                data, source = receiver.recvfrom(65535)
            except socket.timeout:
                continue
            assert len(data) == (6 if data[0] == 1 else 8), data
            assert data[0] in (1, 2), data
            checksum = 0
            for byte in data:
                checksum ^= byte
            assert checksum == 0, data
            if data[0] == 2:
                assert data[1:3] == bytes([0, 1]), data
            frames.append((phase, data[0], source[1]))
        output = process.communicate(timeout=2)[0]
        assert process.returncode == 0, output
        assert 'UDP mode=--synthetic' in output, 'Not a UDP build: ' + output
        assert '[tcp]' not in output and 'send_failures=0' in output, output
        summary = re.search(r'enqueued=(\d+) socket_sent=(\d+) pending=0 uncertain=0 status=OK', output)
        assert summary and summary[1] == summary[2], output
        for phase in (1, 2):
            assert any(p == phase and kind == 1 for p, kind, _ in frames), frames
            assert any(p == phase and kind == 2 for p, kind, _ in frames), frames
        assert len(set(port for _, _, port in frames)) == 1, 'Sender recreated socket'
        keys = sum(kind == 2 for _, kind, _ in frames)
        assert 0 < keys < int(summary[1]), 'Offline packets must not be replayed'
        qid = re.search(r'qid=(\d+)', output)[1]
        rows = pathlib.Path('/proc/sysvipc/msg').read_text().splitlines()[1:]
        assert all(row.split()[1] != qid for row in rows), 'Queue leaked'
        print('udp-sender: PASS; late listener, listener restart, frame boundaries, no replay, queue cleanup; '
              'sent_keys=' + summary[1] + ' received_keys=' + str(keys))
    finally:
        if receiver is not None:
            receiver.close()
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=3)


if __name__ == '__main__':
    main()
