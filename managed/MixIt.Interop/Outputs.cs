namespace MixIt.Interop;

/// <summary>What an output records or streams, and how (M5).</summary>
public sealed record OutputSettings
{
    /// <summary>File path, rtmp://, rtmps://, srt://, udp:// or a .m3u8 path (HLS).</summary>
    public required string Url { get; init; }

    /// <summary>null = from the url; else mp4, mov, matroska, mpegts, flv, hls.</summary>
    public string? Format { get; init; }

    /// <summary>
    /// 0 = program; <see cref="MultiviewSource"/> = the multiview with program audio;
    /// otherwise an input id (ISO recording at the input's own size).
    /// </summary>
    public uint SourceInputId { get; init; }

    /// <summary>Mirrors MIXIT_OUTPUT_SOURCE_MULTIVIEW.</summary>
    public const uint MultiviewSource = 0xFFFF;

    /// <summary>null = automatic (hardware first); else an encoder name from <see cref="VideoEncoderInfo"/>.</summary>
    public string? VideoEncoder { get; init; }

    public int VideoKbps { get; init; } = 6000;
    public int KeyframeMilliseconds { get; init; } = 2000;
    public int AudioKbps { get; init; } = 160;

    /// <summary>MP4/MOV: fragmented, so the file stays playable after a crash.</summary>
    public bool Fragmented { get; init; } = true;

    /// <summary>Network destinations: retry with back-off after errors.</summary>
    public bool Reconnect { get; init; } = true;

    /// <summary>Files: 0 = one file; else a new file every N (≥ 2) seconds at a keyframe.</summary>
    public int SegmentSeconds { get; init; }

    public bool IsNetwork => Url.Contains("://", StringComparison.Ordinal);
}

public enum OutputState
{
    Idle = 0,
    Starting = 1,
    Running = 2,
    Reconnecting = 3,
    Stopping = 4,
    Failed = 5,
}

public sealed record OutputStatistics(
    OutputState State,
    string Encoder,
    bool HardwareEncoder,
    string CurrentFile,
    string LastError,
    ulong BytesWritten,
    ulong VideoPackets,
    ulong AudioPackets,
    ulong DroppedPackets,
    ulong EncoderDroppedFrames,
    ulong Reconnects,
    int Segments,
    int BitrateKbps,
    int QueueMilliseconds,
    TimeSpan Duration,
    long? FreeBytes,
    bool DataLost);

public sealed record VideoEncoderInfo(string Name, string LongName, bool Hardware, bool Usable, string Error);

/// <summary>Hides stream keys, passwords and query values for display and logs (same rules as the engine).</summary>
public static class UrlRedaction
{
    public static string Redact(string url)
    {
        ArgumentNullException.ThrowIfNull(url);
        int schemeEnd = url.IndexOf("://", StringComparison.Ordinal);
        if (schemeEnd < 0) return url;  // a file path
        string scheme = url[..schemeEnd].ToLowerInvariant();
        string rest = url[(schemeEnd + 3)..];
        string query = "";
        int q = rest.IndexOf('?', StringComparison.Ordinal);
        if (q >= 0)
        {
            var parts = rest[(q + 1)..].Split('&', StringSplitOptions.RemoveEmptyEntries)
                .Select(kv => kv.Contains('=', StringComparison.Ordinal) ? kv[..kv.IndexOf('=', StringComparison.Ordinal)] + "=***" : kv);
            query = "?" + string.Join('&', parts);
            rest = rest[..q];
        }
        string authority = rest, path = "";
        int slash = rest.IndexOf('/', StringComparison.Ordinal);
        if (slash >= 0)
        {
            authority = rest[..slash];
            path = rest[slash..];
        }
        int at = authority.LastIndexOf('@');
        if (at >= 0) authority = "***@" + authority[(at + 1)..];
        if (scheme is "rtmp" or "rtmps" && path.Length > 0)
        {
            int last = path.LastIndexOf('/');
            if (last >= 0 && last + 1 < path.Length) path = path[..(last + 1)] + "***";
        }
        return scheme + "://" + authority + path + (query.Length > 1 ? query : "");
    }
}
