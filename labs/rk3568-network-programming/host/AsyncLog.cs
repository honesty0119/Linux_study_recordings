/* 有界异步日志：接收线程仅入队，独立线程写入并 Flush。
 * 性能终点是实际输出调用完成，包含排队时间；队列满/写失败/关闭超时均明确失败。
 */
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading;

public sealed class LogPipelineException : Exception
{
    public LogPipelineException(string message) : base(message) { }
}

public sealed class AsyncLog
{
    private sealed class Entry
    {
        public string Line;
        public double QueuedMs, ReceivedMs;
    }
    private readonly BlockingCollection<Entry> queue;
    private readonly TextWriter output;
    private readonly bool ownsOutput;
    private readonly Stopwatch clock;
    private readonly Thread worker;
    private string failure;
    private bool completed;
    private long accepted, written;
    private int peak;
    // 以下统计只由日志线程更新，成功 Join 后才能读取最终结果。
    public long FramesWritten { get; private set; }
    public long FramesOver50Ms { get; private set; }
    public double MaxReceiveToLogMs { get; private set; }
    public double MaxQueueWaitMs { get; private set; }
    public double MaxWriteMs { get; private set; }
    public long Accepted { get { return Interlocked.Read(ref accepted); } }
    public long Written { get { return Interlocked.Read(ref written); } }
    public string Failure { get { return Interlocked.CompareExchange(ref failure, null, null); } }

    public AsyncLog(TextWriter output, bool ownsOutput, Stopwatch clock, int capacity)
    {
        if (capacity < 1) throw new ArgumentOutOfRangeException("capacity");
        this.output = output;
        this.ownsOutput = ownsOutput;
        this.clock = clock;
        queue = new BlockingCollection<Entry>(capacity);
        worker = new Thread(WriteLoop) { IsBackground = true, Name = "task2-log-writer" };
        worker.Start();
    }

    private void Fail(string reason) { Interlocked.CompareExchange(ref failure, reason, null); }
    public void ThrowIfFaulted()
    {
        string reason = Failure;
        if (reason != null) throw new LogPipelineException(reason);
    }

    // receivedMs<0 表示状态日志，不计入报文接收→打印完成指标。
    public void Enqueue(string text, double receivedMs)
    {
        ThrowIfFaulted();
        double now = clock.Elapsed.TotalMilliseconds;
        string line = String.Format(CultureInfo.InvariantCulture,
            "[{0:yyyy-MM-dd HH:mm:ss.fff}] mono_ms={1:F3} {2}", DateTime.Now, now, text);
        Entry entry = new Entry { Line = line, QueuedMs = clock.Elapsed.TotalMilliseconds, ReceivedMs = receivedMs };
        try
        {
            if (!queue.TryAdd(entry))
            {
                Fail("Log queue full; stopping reception rather than silently dropping records");
                throw new LogPipelineException(Failure);
            }
        }
        catch (InvalidOperationException)
        {
            Fail("Log queue closed unexpectedly");
            throw new LogPipelineException(Failure);
        }
        Interlocked.Increment(ref accepted);
        peak = Math.Max(peak, queue.Count);
    }

    private void WriteLoop()
    {
        try
        {
            foreach (Entry entry in queue.GetConsumingEnumerable())
            {
                double before = clock.Elapsed.TotalMilliseconds;
                MaxQueueWaitMs = Math.Max(MaxQueueWaitMs, before - entry.QueuedMs);
                output.WriteLine(entry.Line);
                output.Flush(); // 计时到输出/Flush 返回；不冒称物理磁盘持久化。
                double after = clock.Elapsed.TotalMilliseconds;
                MaxWriteMs = Math.Max(MaxWriteMs, after - before);
                if (entry.ReceivedMs >= 0)
                {
                    double elapsed = after - entry.ReceivedMs;
                    ++FramesWritten;
                    if (elapsed > 50) ++FramesOver50Ms;
                    MaxReceiveToLogMs = Math.Max(MaxReceiveToLogMs, elapsed);
                }
                Interlocked.Increment(ref written);
            }
            // 在所有先前日志写完以后写最终统计，包含真实排队和输出耗时。
            output.WriteLine(String.Format(CultureInfo.InvariantCulture,
                "LogSummary accepted={0} written={1} frame_logs={2} over_50ms={3} max_receive_to_log_ms={4:F3} max_queue_wait_ms={5:F3} max_write_ms={6:F3} queue_peak={7}",
                Accepted, Written, FramesWritten, FramesOver50Ms, MaxReceiveToLogMs, MaxQueueWaitMs, MaxWriteMs, peak));
            output.Flush();
        }
        catch (Exception e)
        {
            Fail("Log output failed: " + e.Message);
        }
        finally
        {
            if (ownsOutput)
            {
                try { output.Dispose(); }
                catch (Exception e) { Fail("Closing log failed: " + e.Message); }
            }
        }
    }

    // 有界关闭：排空已经接受的日志；永久阻塞的输出不能使主程序无限挂住。
    public bool Complete(int timeoutMs)
    {
        if (!completed) { queue.CompleteAdding(); completed = true; }
        if (!worker.Join(timeoutMs))
        {
            Fail("Log shutdown timed out; pending or in-flight records are incomplete");
            return false;
        }
        if (Accepted != Written) Fail("Not all accepted records reached output");
        return Failure == null;
    }
}
