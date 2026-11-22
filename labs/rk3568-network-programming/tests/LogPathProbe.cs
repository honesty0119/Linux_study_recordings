/* 日志路径归因小实验：分开测格式化与 WriteLine，记录墙钟和当前线程 CPU 周期。
 * 仅诊断，不替换 Receiver，不更改网络。样本频率高于正式心跳，不能当作正式验收。
 */
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public static class LogPathProbe
{
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentThread();
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool QueryThreadCycleTime(IntPtr thread, out ulong cycles);
    static ulong Cycles() { ulong cycles; if (!QueryThreadCycleTime(GetCurrentThread(), out cycles)) throw new Exception("CPU counter unavailable"); return cycles; }
    public static int Main(string[] args)
    {
        if (args.Length != 2) return 1;
        Console.OutputEncoding = new UTF8Encoding(false);
        string mode = args[0], prefix = args[1];
        TextWriter writer;
        if (mode == "null") writer = TextWriter.Null;
        else if (mode == "file") writer = new StreamWriter(prefix + ".direct.log", false, new UTF8Encoding(false)) { AutoFlush = true };
        else if (mode == "stdout") writer = Console.Out;
        else return 1;
        Stopwatch clock = Stopwatch.StartNew();
        StringBuilder csv = new StringBuilder("index,format_ms,write_ms,format_cycles,write_cycles\n");
        string message = "收到心跳报文，连接正常 heartbeat=100 delta_ms=1000.000 board_unix=1795338000 rx_unix_us=1795338000000000 hex=01 6B 02 AF 10 D7";
        for (int i = 0; i < 1000; ++i)
        {
            ulong c0 = Cycles(); long t0 = Stopwatch.GetTimestamp();
            string line = String.Format(CultureInfo.CurrentCulture, "[{0:yyyy-MM-dd HH:mm:ss.fff}] mono_ms={1:F3} {2}", DateTime.Now, clock.Elapsed.TotalMilliseconds, message);
            long t1 = Stopwatch.GetTimestamp(); ulong c1 = Cycles();
            writer.WriteLine(line);
            long t2 = Stopwatch.GetTimestamp(); ulong c2 = Cycles();
            csv.AppendFormat(CultureInfo.InvariantCulture, "{0},{1:F6},{2:F6},{3},{4}\n", i,
                (t1-t0)*1000.0/Stopwatch.Frequency, (t2-t1)*1000.0/Stopwatch.Frequency, c1-c0, c2-c1);
            Thread.Sleep(2);
        }
        writer.Flush();
        if (mode == "file") writer.Dispose();
        File.WriteAllText(prefix + ".csv", csv.ToString(), new UTF8Encoding(false));
        return 0;
    }
}
