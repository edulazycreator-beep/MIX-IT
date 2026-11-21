using System.Buffers.Binary;
using System.Net.Sockets;
using Microsoft.Extensions.Logging;
using MixIt.Protocol;

namespace MixIt.Core.Control;

/// <summary>How VISCA commands are carried over IP.</summary>
public enum ViscaTransport
{
    /// <summary>Sony VISCA over IP: 8-byte header (type, length, sequence), UDP port 52381.</summary>
    SonyUdp,
    /// <summary>Raw VISCA bytes over UDP (PTZOptics and many others; port 1259).</summary>
    RawUdp,
    /// <summary>Raw VISCA bytes over TCP (port 5678 on many cameras).</summary>
    RawTcp,
}

/// <summary>A PTZ camera reachable with VISCA over IP.</summary>
public sealed record PtzCamera(string Name, string Host, int Port, ViscaTransport Transport = ViscaTransport.SonyUdp, int Address = 1)
{
    public override string ToString() => $"{Name} ({Host}:{Port}, {Transport})";
}

/// <summary>
/// VISCA command bytes (publicly documented Sony VISCA command set, as used by
/// most PTZ cameras). Speeds: pan 1..24, tilt 1..23, zoom 0..7.
/// </summary>
public static class Visca
{
    private static byte Head(int address) => (byte)(0x80 | (address & 0x07));

    public static byte[] PresetRecall(int address, int preset) => [Head(address), 0x01, 0x04, 0x3F, 0x02, (byte)preset, 0xFF];

    public static byte[] PresetStore(int address, int preset) => [Head(address), 0x01, 0x04, 0x3F, 0x01, (byte)preset, 0xFF];

    public static byte[] Home(int address) => [Head(address), 0x01, 0x06, 0x04, 0xFF];

    /// <param name="pan">-1 left, 0 stop, 1 right</param>
    /// <param name="tilt">-1 down, 0 stop, 1 up</param>
    public static byte[] PanTilt(int address, int pan, int tilt, int panSpeed, int tiltSpeed)
    {
        byte x = pan < 0 ? (byte)0x01 : pan > 0 ? (byte)0x02 : (byte)0x03;
        byte y = tilt > 0 ? (byte)0x01 : tilt < 0 ? (byte)0x02 : (byte)0x03;
        return [Head(address), 0x01, 0x06, 0x01, (byte)Math.Clamp(panSpeed, 1, 24), (byte)Math.Clamp(tiltSpeed, 1, 23), x, y, 0xFF];
    }

    /// <param name="direction">1 tele (in), -1 wide (out), 0 stop</param>
    public static byte[] Zoom(int address, int direction, int speed)
    {
        int s = Math.Clamp(speed, 0, 7);
        byte z = direction > 0 ? (byte)(0x20 | s) : direction < 0 ? (byte)(0x30 | s) : (byte)0x00;
        return [Head(address), 0x01, 0x04, 0x07, z, 0xFF];
    }

    /// <summary>Sony VISCA-over-IP framing: payload type 0x0100 (command), length, sequence number (big-endian).</summary>
    public static byte[] SonyFrame(byte[] visca, uint sequence)
    {
        var frame = new byte[8 + visca.Length];
        BinaryPrimitives.WriteUInt16BigEndian(frame, 0x0100);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(2), (ushort)visca.Length);
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(4), sequence);
        visca.CopyTo(frame, 8);
        return frame;
    }
}

/// <summary>
/// PTZ control (M7, EXPERIMENTAL: no camera tested — byte streams are checked
/// against the VISCA command set in tests). Commands are sent fire-and-forget
/// with a short timeout; a camera that is off never blocks the production.
/// </summary>
public sealed class PtzService : IDisposable
{
    private readonly ILogger<PtzService> _logger;
    private readonly UdpClient _udp = new(AddressFamily.InterNetwork);
    private List<PtzCamera> _cameras = [];
    private uint _sequence;

    public PtzService(ILogger<PtzService> logger) => _logger = logger;

    public IReadOnlyList<PtzCamera> Cameras => _cameras;

    public void SetCameras(IEnumerable<PtzCamera> cameras)
    {
        var list = cameras.ToList();
        foreach (PtzCamera c in list)
        {
            if (string.IsNullOrWhiteSpace(c.Name) || string.IsNullOrWhiteSpace(c.Host) || c.Port is < 1 or > 65535 || c.Address is < 1 or > 7)
                throw new ArgumentException($"Invalid PTZ camera {c}.");
        }
        if (list.Select(c => c.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != list.Count)
            throw new ArgumentException("PTZ camera names must be unique.");
        _cameras = list;
    }

    public PtzCamera Find(string reference)
    {
        string r = reference.Trim();
        if (int.TryParse(r, out int n) && n >= 1 && n <= _cameras.Count) return _cameras[n - 1];
        return _cameras.FirstOrDefault(c => string.Equals(c.Name, r, StringComparison.OrdinalIgnoreCase))
            ?? throw new CommandException($"No PTZ camera '{r}'.");
    }

    public async Task SendAsync(PtzCamera camera, byte[] visca)
    {
        ArgumentNullException.ThrowIfNull(camera);
        switch (camera.Transport)
        {
            case ViscaTransport.SonyUdp:
                byte[] frame = Visca.SonyFrame(visca, unchecked(++_sequence));
                await _udp.SendAsync(frame, frame.Length, camera.Host, camera.Port).ConfigureAwait(false);
                break;
            case ViscaTransport.RawUdp:
                await _udp.SendAsync(visca, visca.Length, camera.Host, camera.Port).ConfigureAwait(false);
                break;
            case ViscaTransport.RawTcp:
                using (var tcp = new TcpClient())
                using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(2)))
                {
                    try
                    {
                        await tcp.ConnectAsync(camera.Host, camera.Port, cts.Token).ConfigureAwait(false);
                        await tcp.GetStream().WriteAsync(visca, cts.Token).ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                    {
                        throw new TimeoutException($"PTZ camera {camera.Name} did not answer within 2 s.");
                    }
                }
                break;
        }
        _logger.LogDebug("PTZ {Camera}: {Bytes}", camera.Name, Convert.ToHexString(visca));
    }

    /// <summary>Registers the PTZ commands.</summary>
    public void Register(CommandService commands)
    {
        ArgumentNullException.ThrowIfNull(commands);
        var cam = new CommandParameter("camera", ParameterKind.String, true, "PTZ camera number (1 = first) or name");
        var preset = new CommandParameter("preset", ParameterKind.Integer, true, "Preset 0..127");
        var speed = new CommandParameter("speed", ParameterKind.Integer, false, "Speed 1..24 (pan/tilt, default 8) or 0..7 (zoom, default 3)");
        commands.Register(new("PtzPreset", "Recall a camera preset.", [cam, preset], "PTZ"), async a =>
        {
            PtzCamera c = Find(a.String("camera"));
            await SendAsync(c, Visca.PresetRecall(c.Address, a.Int("preset", null, 0, 127))).ConfigureAwait(true);
            return null;
        });
        commands.Register(new("PtzStorePreset", "Store the camera position as a preset.", [cam, preset], "PTZ"), async a =>
        {
            PtzCamera c = Find(a.String("camera"));
            await SendAsync(c, Visca.PresetStore(c.Address, a.Int("preset", null, 0, 127))).ConfigureAwait(true);
            return null;
        });
        commands.Register(new("PtzMove", "Start moving (until PtzStop).",
            [cam, new("direction", ParameterKind.String, true, "left, right, up, down, upleft, upright, downleft, downright"), speed], "PTZ"), async a =>
        {
            PtzCamera c = Find(a.String("camera"));
            string d = a.String("direction").ToLowerInvariant();
            int pan = d.Contains("left", StringComparison.Ordinal) ? -1 : d.Contains("right", StringComparison.Ordinal) ? 1 : 0;
            int tilt = d.StartsWith("up", StringComparison.Ordinal) ? 1 : d.StartsWith("down", StringComparison.Ordinal) ? -1 : 0;
            if (pan == 0 && tilt == 0) throw new CommandException($"Unknown direction '{d}'.");
            int s = a.Int("speed", 8, 1, 24);
            await SendAsync(c, Visca.PanTilt(c.Address, pan, tilt, s, Math.Min(s, 23))).ConfigureAwait(true);
            return null;
        });
        commands.Register(new("PtzStop", "Stop pan, tilt and zoom.", [cam], "PTZ"), async a =>
        {
            PtzCamera c = Find(a.String("camera"));
            await SendAsync(c, Visca.PanTilt(c.Address, 0, 0, 1, 1)).ConfigureAwait(true);
            await SendAsync(c, Visca.Zoom(c.Address, 0, 0)).ConfigureAwait(true);
            return null;
        });
        commands.Register(new("PtzZoom", "Zoom in, out or stop.",
            [cam, new("direction", ParameterKind.String, true, "in, out or stop"), speed], "PTZ"), async a =>
        {
            PtzCamera c = Find(a.String("camera"));
            int dir = a.String("direction").ToLowerInvariant() switch
            {
                "in" or "tele" => 1,
                "out" or "wide" => -1,
                "stop" => 0,
                var x => throw new CommandException($"Unknown zoom direction '{x}'."),
            };
            await SendAsync(c, Visca.Zoom(c.Address, dir, a.Int("speed", 3, 0, 7))).ConfigureAwait(true);
            return null;
        });
        commands.Register(new("PtzHome", "Move the camera to its home position.", [cam], "PTZ"), async a =>
        {
            PtzCamera c = Find(a.String("camera"));
            await SendAsync(c, Visca.Home(c.Address)).ConfigureAwait(true);
            return null;
        });
    }

    public void Dispose() => _udp.Dispose();
}
