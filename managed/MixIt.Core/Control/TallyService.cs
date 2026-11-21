using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using Microsoft.Extensions.Logging;
using MixIt.Core.Production;
using MixIt.Interop;
using MixIt.Protocol;

namespace MixIt.Core.Control;

/// <summary>A TSL UMD v5.0 tally destination (UDP).</summary>
/// <param name="Host">Host name or IP of the tally controller / multiviewer.</param>
/// <param name="Port">UDP port (often 8900 or 4003; see the device).</param>
/// <param name="Screen">TSL screen number (0 = default).</param>
/// <param name="IndexOffset">TSL display index of input 1 (default 1: index = input number).</param>
public sealed record TallyDestination(string Host, int Port, int Screen = 0, int IndexOffset = 1)
{
    public override string ToString() => $"TSL 5.0 → {Host}:{Port} (screen {Screen})";
}

/// <summary>
/// Tally (M7): which inputs are on program (red) or preview (green), computed
/// from the engine state — program bus, a running transition's incoming input,
/// the picture-in-picture box and visible overlay channels count as program.
/// Published as an event (remote API, extensions) and as TSL UMD v5.0 over UDP
/// (EXPERIMENTAL: no tally hardware tested).
/// </summary>
public sealed class TallyService : IDisposable
{
    private readonly ProductionService _production;
    private readonly ILogger<TallyService> _logger;
    private readonly UdpClient _udp = new(AddressFamily.InterNetwork);
    private IReadOnlyList<TallyEntry> _current = [];
    private List<TallyDestination> _destinations = [];
    private DateTime _lastSend = DateTime.MinValue;

    public TallyService(ProductionService production, ILogger<TallyService> logger)
    {
        _production = production;
        _logger = logger;
    }

    /// <summary>Raised (on the caller of <see cref="Refresh"/>) when any entry changes.</summary>
    public event EventHandler<IReadOnlyList<TallyEntry>>? Changed;

    public IReadOnlyList<TallyEntry> Current => _current;

    public IReadOnlyList<TallyDestination> Destinations => _destinations;

    public void SetDestinations(IEnumerable<TallyDestination> destinations)
    {
        _destinations = destinations.ToList();
        foreach (TallyDestination d in _destinations)
        {
            if (string.IsNullOrWhiteSpace(d.Host) || d.Port is < 1 or > 65535 || d.Screen is < 0 or > 65534 || d.IndexOffset is < 0 or > 1000)
                throw new ArgumentException($"Invalid tally destination {d}.");
        }
        _lastSend = DateTime.MinValue;  // send the current state to new destinations
        Send(_current);
    }

    /// <summary>Recomputes the tally (call on the production thread, ~10 times a second).</summary>
    public void Refresh()
    {
        IReadOnlyList<TallyEntry> next = Compute();
        bool changed = next.Count != _current.Count || next.Where((e, i) => e != _current[i]).Any();
        _current = next;
        if (changed) Changed?.Invoke(this, next);
        // TSL receivers expect a periodic refresh; resend every second or on change.
        if (changed || DateTime.UtcNow - _lastSend > TimeSpan.FromSeconds(1)) Send(next);
    }

    private List<TallyEntry> Compute()
    {
        IReadOnlyList<ProductionInput> inputs = _production.Inputs;
        var program = new HashSet<uint>();
        uint preview = 0;
        if (_production.IsRunning && _production.GetStatistics() is { } s)
        {
            program.Add(s.ProgramInputId);
            preview = s.PreviewInputId;
            if (s.ActiveTransition is not null) program.Add(s.PreviewInputId);  // incoming input is already visible
            if (_production.PictureInPictureInputId is uint pip) program.Add(pip);
            for (int c = 0; c < NativeSession.OverlayChannels; c++)
            {
                OverlayState o = _production.GetOverlay(c).State;
                if (o.InputId != 0 && o.Level > 0) program.Add(o.InputId);
            }
        }
        return inputs.Select((i, n) => new TallyEntry(i.Id, n + 1, i.Name, program.Contains(i.Id), !program.Contains(i.Id) && i.Id == preview))
            .ToList();
    }

    private void Send(IReadOnlyList<TallyEntry> entries)
    {
        if (_destinations.Count == 0) return;
        _lastSend = DateTime.UtcNow;
        foreach (TallyDestination d in _destinations)
        {
            try
            {
                byte[] packet = BuildTslPacket(entries, d.Screen, d.IndexOffset);
                _udp.Send(packet, packet.Length, d.Host, d.Port);
            }
            catch (Exception ex) when (ex is SocketException or ArgumentException)
            {
                // A tally receiver that is gone must never disturb the production.
                _logger.LogWarning("Tally to {Destination} failed: {Message}", d, ex.Message);
            }
        }
    }

    /// <summary>
    /// TSL UMD v5.0 packet (little-endian): PBC (byte count after this field),
    /// VER 0, FLAGS 0 (ASCII text), SCREEN, then per input: INDEX, CONTROL
    /// (bits 0-1 right tally, 2-3 text tally, 4-5 left tally: 1 red, 2 green;
    /// bits 6-7 brightness 3 = full), LENGTH, ASCII text (≤ 64 bytes here).
    /// </summary>
    public static byte[] BuildTslPacket(IReadOnlyList<TallyEntry> entries, int screen, int indexOffset)
    {
        ArgumentNullException.ThrowIfNull(entries);
        var body = new List<byte>(6 + entries.Count * 24) { 0, 0 };  // VER, FLAGS
        AddUInt16(body, (ushort)screen);
        foreach (TallyEntry e in entries)
        {
            int lamp = e.Program ? 1 : e.Preview ? 2 : 0;
            ushort control = (ushort)(lamp | (lamp << 2) | (lamp << 4) | (3 << 6));
            byte[] text = Encoding.ASCII.GetBytes(Ascii(e.Name, 64));
            AddUInt16(body, (ushort)(e.Number - 1 + indexOffset));
            AddUInt16(body, control);
            AddUInt16(body, (ushort)text.Length);
            body.AddRange(text);
        }
        var packet = new byte[2 + body.Count];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, (ushort)body.Count);
        body.CopyTo(packet, 2);
        return packet;
    }

    private static void AddUInt16(List<byte> b, ushort v)
    {
        b.Add((byte)(v & 0xFF));
        b.Add((byte)(v >> 8));
    }

    private static string Ascii(string s, int max)
    {
        var sb = new StringBuilder(Math.Min(s.Length, max));
        foreach (char c in s)
        {
            if (sb.Length == max) break;
            sb.Append(c is >= ' ' and <= '~' ? c : '?');
        }
        return sb.ToString();
    }

    public void Dispose() => _udp.Dispose();
}
