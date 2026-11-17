using System.Threading.Channels;
using MixIt.Interop;

namespace MixIt.Core.Engine;

/// <summary>
/// Bounded hand-off of engine log records to a single consumer (the UI log
/// view, the probe tool). The native logging thread only ever calls
/// <see cref="Publish"/>, which never blocks: when the consumer falls behind,
/// the OLDEST records are dropped and counted. The complete record stream is
/// always in the engine's JSON-lines log file.
/// </summary>
public sealed class EngineLogStream
{
    private readonly Channel<NativeLogEntry> _channel;
    private long _dropped;

    /// <param name="capacity">Maximum buffered records (must be positive).</param>
    public EngineLogStream(int capacity)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(capacity);
        Capacity = capacity;
        _channel = Channel.CreateBounded<NativeLogEntry>(
            new BoundedChannelOptions(capacity)
            {
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
                SingleWriter = true,  // only the native logging thread writes
            },
            _ => Interlocked.Increment(ref _dropped));
    }

    /// <summary>Configured capacity in records.</summary>
    public int Capacity { get; }

    /// <summary>Reader for the single consumer.</summary>
    public ChannelReader<NativeLogEntry> Reader => _channel.Reader;

    /// <summary>Records discarded because the consumer fell behind.</summary>
    public long DroppedCount => Interlocked.Read(ref _dropped);

    /// <summary>Never blocks. Called on the native logging thread.</summary>
    internal void Publish(NativeLogEntry entry) => _channel.Writer.TryWrite(entry);
}
