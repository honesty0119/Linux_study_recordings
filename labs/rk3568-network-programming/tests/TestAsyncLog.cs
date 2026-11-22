/* 对异步日志进行故障注入：输出阻塞、队列满、写入失败和关闭超时。 */
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

public static class TestAsyncLog
{
    private static int checks;
    private static void Check(bool value, string name)
    {
        ++checks;
        if (!value) throw new Exception("FAIL: " + name);
    }
    private sealed class GateWriter : StringWriter
    {
        public readonly ManualResetEvent Entered = new ManualResetEvent(false);
        public readonly ManualResetEvent Release = new ManualResetEvent(false);
        public override void WriteLine(string value)
        {
            Entered.Set();
            if (!Release.WaitOne(5000)) throw new IOException("Test gate timed out");
            base.WriteLine(value);
        }
    }
    private sealed class FailedWriter : StringWriter
    {
        public override void WriteLine(string value) { throw new IOException("Injected disk/output failure"); }
    }
    public static int Main()
    {
        Stopwatch clock = Stopwatch.StartNew();
        GateWriter slow = new GateWriter();
        AsyncLog log = new AsyncLog(slow, false, clock, 8);
        try
        {
            log.Enqueue("first", clock.Elapsed.TotalMilliseconds);
            Check(slow.Entered.WaitOne(2000), "writer entered");
            log.Enqueue("second", clock.Elapsed.TotalMilliseconds);
            log.Enqueue("third", clock.Elapsed.TotalMilliseconds);
            Check(log.Accepted == 3 && log.Written == 0, "producer progresses while output blocked");
            Thread.Sleep(80);
        }
        finally { slow.Release.Set(); }
        Check(log.Complete(2000), "drain succeeds");
        Check(log.Written == 3 && log.FramesWritten == 3, "all frames written");
        Check(log.FramesOver50Ms == 3 && log.MaxReceiveToLogMs >= 75, "queue time included in completion latency");
        Check(slow.ToString().IndexOf("first") < slow.ToString().IndexOf("second"), "FIFO logging");

        GateWriter full = new GateWriter();
        AsyncLog bounded = new AsyncLog(full, false, clock, 2);
        bool rejected = false;
        try
        {
            bounded.Enqueue("inflight", -1);
            Check(full.Entered.WaitOne(2000), "overflow gate entered");
            bounded.Enqueue("queued1", -1);
            bounded.Enqueue("queued2", -1);
            try { bounded.Enqueue("overflow", -1); } catch (LogPipelineException) { rejected = true; }
            Check(rejected && bounded.Accepted == 3 && bounded.Failure.Contains("full"), "queue overflow is explicit");
        }
        finally { full.Release.Set(); }
        Check(!bounded.Complete(2000) && bounded.Written == 3, "drain accepted records but retain overflow failure");

        AsyncLog broken = new AsyncLog(new FailedWriter(), false, clock, 4);
        broken.Enqueue("failed output", -1);
        Check(SpinWait.SpinUntil(delegate { return broken.Failure != null; }, 2000), "output failure propagated");
        Check(!broken.Complete(2000) && broken.Written == 0, "failed write is not counted complete");
        rejected = false;
        try { broken.Enqueue("after failure", -1); } catch (LogPipelineException) { rejected = true; }
        Check(rejected, "future enqueue rejects failed sink");

        GateWriter stuck = new GateWriter();
        AsyncLog boundedShutdown = new AsyncLog(stuck, false, clock, 4);
        try
        {
            boundedShutdown.Enqueue("stuck", -1);
            Check(stuck.Entered.WaitOne(2000), "shutdown gate entered");
            Check(!boundedShutdown.Complete(10), "shutdown is bounded and reports timeout");
        }
        finally { stuck.Release.Set(); }
        Check(!boundedShutdown.Complete(2000) && boundedShutdown.Written == 1, "timeout failure retained after release");
        Console.WriteLine("async-log: {0} checks PASS", checks);
        return 0;
    }
}
