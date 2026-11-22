/*
 * Windows TCP 上位机：严格解析原任务的 6/8 字节帧，记录日志并检测 2s 心跳超时。
 * 仅使用 Windows 自带 .NET Framework；无需安装依赖或修改防火墙。
 * TCP 没有报文边界，Parser 保存不完整帧；协议无同步头，非法帧直接关闭连接。
 */
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

public sealed class Frame
{
    public byte Type;
    public uint Timestamp;
    public byte[] Bytes;
}

public sealed class Parser
{
    private readonly byte[] pending = new byte[8];
    private int used, expected;

    // 可接收任意 TCP 分段；每次回调对应一帧完整且校验通过的报文。
    public void Feed(byte[] data, int length, Action<Frame> emit)
    {
        for (int i = 0; i < length; ++i)
        {
            byte value = data[i];
            if (used == 0)
            {
                if (value != 1 && value != 2) throw new InvalidDataException("未知报文类型");
                expected = value == 1 ? 6 : 8;
            }
            pending[used++] = value;
            if (used != expected) continue;
            byte xor = 0;
            for (int j = 0; j < expected; ++j) xor ^= pending[j];
            if (xor != 0) throw new InvalidDataException("XOR 校验失败");
            if (pending[0] == 2 && (pending[1] != 0 || pending[2] != 1))
                throw new InvalidDataException("非法按键编号或事件类型");
            int offset = pending[0] == 1 ? 1 : 3;
            uint stamp = ((uint)pending[offset] << 24) | ((uint)pending[offset + 1] << 16)
                | ((uint)pending[offset + 2] << 8) | pending[offset + 3];
            byte[] copy = new byte[expected];
            Array.Copy(pending, copy, expected);
            used = 0;
            emit(new Frame { Type = copy[0], Timestamp = stamp, Bytes = copy });
        }
    }
    public bool HasPartial { get { return used != 0; } }
}

public sealed class HeartbeatMonitor
{
    public double ReferenceMs;
    public bool Active, Alarm;
    // 新连接从建立时开始计时；随后只有合法心跳能刷新，不被按键帧延期。
    public void Begin(double now) { Active = true; ReferenceMs = now; Alarm = false; }
    public void Receive(double now) { ReferenceMs = now; Alarm = false; }
    public bool Expired(double now)
    {
        if (Active && !Alarm && now - ReferenceMs > 2000)
        { Alarm = true; return true; }
        return false;
    }
}

// UDP 每个数据报必须恰好是一帧；不能跨包拼接，也不能接受一包多帧或尾随字节。
public static class DatagramParser
{
    public static Frame Parse(byte[] bytes, int length)
    {
        if (length < 1 || length != (bytes[0] == 1 ? 6 : bytes[0] == 2 ? 8 : -1))
            throw new InvalidDataException("UDP 数据报长度/类型不匹配");
        Frame result = null;
        new Parser().Feed(bytes, length, delegate(Frame frame) { result = frame; });
        return result;
    }
}

public static class Receiver
{
    private static volatile bool stop;
    private static readonly Stopwatch clock = Stopwatch.StartNew();
    private static int assertions;
    private static AsyncLog logger;

    private static void Log(string text, double receivedMs = -1)
    {
        logger.Enqueue(text, receivedMs);
    }
    private static void Check(bool value, string name)
    {
        ++assertions;
        if (!value) throw new Exception("FAIL: " + name);
    }
    private static void Reject(byte[] bytes, string name)
    {
        bool rejected = false;
        try { new Parser().Feed(bytes, bytes.Length, delegate(Frame f) { }); }
        catch (InvalidDataException) { rejected = true; }
        Check(rejected, name);
    }

    // 使用独立黄金帧验证所有拆分点、合帧、坏帧和超时边界，不依赖开发板。
    private static int SelfTest()
    {
        byte[] both = { 1, 0x12, 0x34, 0x56, 0x78, 9, 2, 0, 1, 0x12, 0x34, 0x56, 0x78, 11 };
        for (int split = 0; split <= both.Length; ++split)
        {
            Parser parser = new Parser();
            List<Frame> frames = new List<Frame>();
            byte[] first = new byte[split], last = new byte[both.Length - split];
            Array.Copy(both, first, split);
            Array.Copy(both, split, last, 0, last.Length);
            parser.Feed(first, first.Length, frames.Add);
            parser.Feed(last, last.Length, frames.Add);
            Check(frames.Count == 2 && frames[0].Type == 1 && frames[1].Type == 2 &&
                frames[0].Timestamp == 0x12345678 && frames[1].Timestamp == 0x12345678 &&
                !parser.HasPartial, "拆分点 " + split);
        }
        Parser single = new Parser();
        int count = 0;
        foreach (byte b in both) single.Feed(new byte[] { b }, 1, delegate(Frame f) { ++count; });
        Check(count == 2, "逐字节接收");
        Parser partial = new Parser();
        partial.Feed(new byte[] { 1, 0, 0 }, 3, delegate(Frame f) { throw new Exception("提前输出"); });
        Check(partial.HasPartial, "半帧保留");
        Reject(new byte[] { 3 }, "未知类型");
        Reject(new byte[] { 1, 0, 0, 0, 0, 0 }, "错误 XOR");
        Reject(new byte[] { 2, 1, 1, 0, 0, 0, 0, 2 }, "错误按键编号");
        Reject(new byte[] { 2, 0, 2, 0, 0, 0, 0, 0 }, "错误事件类型");
        Parser max = new Parser();
        max.Feed(new byte[] { 1, 255, 255, 255, 255, 1 }, 6,
            delegate(Frame f) { Check(f.Timestamp == UInt32.MaxValue, "无符号大端时间戳"); });
        HeartbeatMonitor monitor = new HeartbeatMonitor();
        Check(!monitor.Expired(9000), "未连接不报警");
        monitor.Begin(100);
        Check(!monitor.Expired(2100), "恰好 2s 不超过阈值");
        Check(monitor.Expired(2101), "超过 2s 报警");
        Check(!monitor.Expired(2200), "同次超时不刷屏");
        monitor.Receive(2500);
        Check(!monitor.Alarm && !monitor.Expired(4500), "心跳恢复并刷新计时");
        Check(monitor.Expired(4501), "再次超时");
        Console.WriteLine("receiver: {0} checks PASS", assertions);
        return 0;
    }

    public static int Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        try
        {
            if (args.Length == 1 && args[0] == "--self-test") return SelfTest();
            if (args.Length < 2)
            {
                Console.Error.WriteLine("Usage: Receiver.exe BIND_IP PORT [SECONDS] [--log NEW_LOCAL_FILE] [--udp --peer BOARD_IP] | --self-test");
                return 1;
            }
            IPAddress address = IPAddress.Parse(args[0]);
            int port = Int32.Parse(args[1], CultureInfo.InvariantCulture);
            double seconds = 0;
            bool durationSet = false;
            string logPath = null;
            bool udp = false;
            IPAddress peer = null;
            for (int i = 2; i < args.Length; ++i)
            {
                if (args[i] == "--log" && logPath == null && i + 1 < args.Length) logPath = args[++i];
                else if (args[i] == "--udp" && !udp) udp = true;
                else if (args[i] == "--peer" && peer == null && i + 1 < args.Length) peer = IPAddress.Parse(args[++i]);
                else if (!durationSet) { seconds = Double.Parse(args[i], CultureInfo.InvariantCulture); durationSet = true; }
                else throw new ArgumentException("Invalid or duplicate arguments");
            }
            if (address.AddressFamily != AddressFamily.InterNetwork || port < 1 || port > 65535 ||
                Double.IsNaN(seconds) || Double.IsInfinity(seconds) || seconds < 0) return 1;
            if (udp != (peer != null) || (peer != null && peer.AddressFamily != AddressFamily.InterNetwork))
                throw new ArgumentException("UDP requires --peer BOARD_IPV4; TCP does not use --peer");
            Console.CancelKeyPress += delegate(object sender, ConsoleCancelEventArgs e) { e.Cancel = true; stop = true; };
            // 推荐直接打开本地文件，绕开父进程的 stdout 重定向管道；不覆盖历史日志。
            TextWriter sink = Console.Out;
            if (logPath != null)
                sink = new StreamWriter(new FileStream(logPath, FileMode.CreateNew, FileAccess.Write, FileShare.Read), new UTF8Encoding(false));
            logger = new AsyncLog(sink, logPath != null, clock, 1024);
            int result;
            try { result = udp ? ServeUdp(address, port, seconds, peer) : Serve(address, port, seconds); }
            finally
            {
                if (!logger.Complete(3000))
                    Console.Error.WriteLine("LOG FAILURE: " + logger.Failure + " accepted=" + logger.Accepted + " written=" + logger.Written);
            }
            return logger.Failure == null ? result : 2;
        }
        catch (Exception e) { Console.Error.WriteLine(e.Message); return 1; }
    }

    // UDP 不调用 Listen/Accept/Connect；绑定后持续接收指定板端 IP 的数据报。
    private static int ServeUdp(IPAddress address, int port, double seconds, IPAddress peer)
    {
        using (Socket socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
        {
            socket.ExclusiveAddressUse = true;
            socket.Bind(new IPEndPoint(address, port));
            byte[] buffer = new byte[65535]; // IPv4 UDP 最大载荷以内完整接收，畸形大包也不按前缀解析。
            HeartbeatMonitor heartbeat = new HeartbeatMonitor();
            double started = clock.Elapsed.TotalMilliseconds;
            heartbeat.Begin(started); // 监听启动后若始终没有心跳，也会明确超时。
            double previous = -1, min = Double.MaxValue, max = 0, total = 0;
            long heartbeats = 0, keys = 0, invalid = 0, ignored = 0, intervals = 0;
            Log("监听成功 UDP " + address + ":" + port + " peer=" + peer);
            while (!stop && (seconds == 0 || clock.Elapsed.TotalMilliseconds - started < seconds * 1000))
            {
                logger.ThrowIfFaulted();
                if (socket.Poll(20000, SelectMode.SelectRead))
                {
                    EndPoint sender = new IPEndPoint(IPAddress.Any, 0);
                    int n = socket.ReceiveFrom(buffer, ref sender);
                    double receivedAt = clock.Elapsed.TotalMilliseconds;
                    long receivedUnixUs = (DateTime.UtcNow.Ticks - 621355968000000000L) / 10;
                    if (!((IPEndPoint)sender).Address.Equals(peer)) ++ignored;
                    else
                    {
                        Frame frame = null;
                        try { frame = DatagramParser.Parse(buffer, n); }
                        catch (InvalidDataException) { ++invalid; }
                        if (frame != null)
                        {
                            string hex = BitConverter.ToString(frame.Bytes).Replace("-", " ");
                            if (frame.Type == 1)
                            {
                                double now = clock.Elapsed.TotalMilliseconds;
                                bool recovered = heartbeat.Alarm;
                                heartbeat.Receive(now);
                                string delta = "first";
                                if (previous >= 0)
                                {
                                    double gap = now - previous;
                                    min = Math.Min(min, gap); max = Math.Max(max, gap); total += gap; ++intervals;
                                    delta = gap.ToString("F3", CultureInfo.InvariantCulture);
                                }
                                previous = now; ++heartbeats;
                                Log("收到心跳报文，连接正常 transport=UDP heartbeat=" + heartbeats + " delta_ms=" + delta +
                                    " board_unix=" + frame.Timestamp + " rx_unix_us=" + receivedUnixUs + " hex=" + hex +
                                    (recovered ? " 心跳恢复" : ""), receivedAt);
                            }
                            else
                            {
                                ++keys;
                                Log("收到 KEY0 按下事件 transport=UDP key=" + keys + " board_unix=" + frame.Timestamp +
                                    " rx_unix_us=" + receivedUnixUs + " hex=" + hex, receivedAt);
                            }
                        }
                    }
                }
                // 坏包、其他来源或连续按键均不能延长心跳期限。
                double tick = clock.Elapsed.TotalMilliseconds;
                if (heartbeat.Expired(tick))
                    Log("[端口" + port + "] 心跳超时，连接异常 transport=UDP elapsed_ms=" +
                        (tick - heartbeat.ReferenceMs).ToString("F3", CultureInfo.InvariantCulture));
            }
            Log("Summary transport=UDP heartbeats=" + heartbeats + " keys=" + keys + " invalid_datagrams=" + invalid +
                " ignored_peer=" + ignored + " intervals=" + intervals + (intervals > 0 ? String.Format(CultureInfo.InvariantCulture,
                " gap_min_ms={0:F3} gap_max_ms={1:F3} gap_avg_ms={2:F3}", min, max, total / intervals) : ""));
        }
        return 0;
    }

    // 单开发板实验：同时处理一个连接；关闭后继续监听，保留旧心跳计时直至超时。
    private static int Serve(IPAddress address, int port, double seconds)
    {
        TcpListener listener = new TcpListener(address, port);
        listener.Server.ExclusiveAddressUse = true;
        TcpClient client = null;
        Parser parser = null;
        HeartbeatMonitor heartbeat = new HeartbeatMonitor();
        byte[] buffer = new byte[4096];
        long heartbeatCount = 0, keyCount = 0;
        double previous = -1, min = Double.MaxValue, max = 0, total = 0;
        long intervals = 0;
        double maxPollMs = 0, maxReadMs = 0, maxParseMs = 0;
        double maxLogEnqueueMs = 0;
        double started = clock.Elapsed.TotalMilliseconds;
        try
        {
            listener.Start(4);
            Log("监听成功 TCP " + address + ":" + port);
            while (!stop && (seconds == 0 || clock.Elapsed.TotalMilliseconds - started < seconds * 1000))
            {
                logger.ThrowIfFaulted();
                // 物理断网可能没有 FIN/RST：超时旧连接不能永久堵住恢复后的新连接。
                if ((client == null || heartbeat.Alarm) && listener.Pending())
                {
                    if (client != null)
                    {
                        Log("新连接替换心跳超时的旧连接");
                        client.Close();
                    }
                    client = listener.AcceptTcpClient();
                    client.NoDelay = true;
                    parser = new Parser();
                    heartbeat.Begin(clock.Elapsed.TotalMilliseconds);
                    previous = -1;
                    Log("已连接 " + client.Client.RemoteEndPoint);
                }
                if (client != null)
                {
                    try
                    {
                        // 有界等待，持续收按键也必须每轮执行超时判断。
                        double pollStart = clock.Elapsed.TotalMilliseconds;
                        bool readable = client.Client.Poll(20000, SelectMode.SelectRead);
                        maxPollMs = Math.Max(maxPollMs, clock.Elapsed.TotalMilliseconds - pollStart);
                        if (readable)
                        {
                            double readStart = clock.Elapsed.TotalMilliseconds;
                            int n = client.Client.Receive(buffer);
                            double receivedAt = clock.Elapsed.TotalMilliseconds;
                            // 与 Windows 本机抓包时间对齐；不与未同步的板端时钟直接相减。
                            long receivedUnixUs = (DateTime.UtcNow.Ticks - 621355968000000000L) / 10;
                            maxReadMs = Math.Max(maxReadMs, receivedAt - readStart);
                            if (n == 0) throw new IOException(parser.HasPartial ? "连接关闭，遗留半帧" : "对端关闭连接");
                            parser.Feed(buffer, n, delegate(Frame frame)
                            {
                                double now = clock.Elapsed.TotalMilliseconds;
                                maxParseMs = Math.Max(maxParseMs, now - receivedAt);
                                double logEnqueueStart;
                                string hex = BitConverter.ToString(frame.Bytes).Replace("-", " ");
                                if (frame.Type == 1)
                                {
                                    bool recovered = heartbeat.Alarm;
                                    heartbeat.Receive(now);
                                    string delta = "first";
                                    if (previous >= 0)
                                    {
                                        double gap = now - previous;
                                        min = Math.Min(min, gap); max = Math.Max(max, gap); total += gap; ++intervals;
                                        delta = gap.ToString("F3", CultureInfo.InvariantCulture);
                                    }
                                    previous = now; ++heartbeatCount;
                                    logEnqueueStart = clock.Elapsed.TotalMilliseconds;
                                    Log("收到心跳报文，连接正常 heartbeat=" + heartbeatCount + " delta_ms=" + delta +
                                        " board_unix=" + frame.Timestamp + " rx_unix_us=" + receivedUnixUs +
                                        " hex=" + hex + (recovered ? " 心跳恢复" : ""), receivedAt);
                                }
                                else
                                {
                                    ++keyCount;
                                    logEnqueueStart = clock.Elapsed.TotalMilliseconds;
                                    Log("收到 KEY0 按下事件 key=" + keyCount + " board_unix=" + frame.Timestamp + " hex=" + hex, receivedAt);
                                }
                                maxLogEnqueueMs = Math.Max(maxLogEnqueueMs, clock.Elapsed.TotalMilliseconds - logEnqueueStart);
                            });
                        }
                    }
                    catch (Exception e)
                    {
                        if (!(e is SocketException) && !(e is IOException) && !(e is InvalidDataException)) throw;
                        Log("连接结束：" + e.Message);
                        client.Close(); client = null;
                    }
                }
                else Thread.Sleep(20);
                double tick = clock.Elapsed.TotalMilliseconds;
                if (heartbeat.Expired(tick))
                    Log("[端口" + port + "] 心跳超时，连接异常 elapsed_ms=" +
                        (tick - heartbeat.ReferenceMs).ToString("F3", CultureInfo.InvariantCulture));
            }
        }
        finally
        {
            if (client != null) client.Close();
            listener.Stop();
        }
        Log("Summary heartbeats=" + heartbeatCount + " keys=" + keyCount + " intervals=" + intervals +
            (intervals > 0 ? String.Format(CultureInfo.InvariantCulture,
                " gap_min_ms={0:F3} gap_max_ms={1:F3} gap_avg_ms={2:F3}", min, max, total / intervals) : ""));
        Log(String.Format(CultureInfo.InvariantCulture,
            "Timing max_poll_ms={0:F3} max_receive_ms={1:F3} max_parse_ms={2:F3} max_log_enqueue_ms={3:F3}",
            maxPollMs, maxReadMs, maxParseMs, maxLogEnqueueMs));
        return 0;
    }
}
