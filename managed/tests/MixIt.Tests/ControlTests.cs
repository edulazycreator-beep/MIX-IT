using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging.Abstractions;
using MixIt.Core.Control;
using MixIt.Core.Engine;
using MixIt.Core.Production;
using MixIt.Interop;
using MixIt.Protocol;
using MixIt.Remote;
using MixIt.SDK;
using Xunit;

namespace MixIt.Tests;

/// <summary>M7: command layer, macros, automation, tally, PTZ, remote control, extensions.</summary>
public class ControlTests
{
    private static readonly ProductionFormat Small = new("test 640x360", 640, 360, MediaRate.Fps30);

    /// <summary>A production with two test signals and the whole control stack.</summary>
    private sealed class Rig : IDisposable
    {
        public Rig()
        {
            Host = new EngineHost(new EngineHostOptions(), NullLogger<EngineHost>.Instance);
            Host.Start();
            Production = new ProductionService(Host, NullLogger<ProductionService>.Instance);
            Production.SetFormat(Small);
            Commands = new CommandService(Dispatcher, NullLogger<CommandService>.Instance);
            ProductionCommands.Register(Commands, Production);
            Macros = new MacroRunner(Commands, NullLogger<MacroRunner>.Instance);
            Macros.Register();
            Tally = new TallyService(Production, NullLogger<TallyService>.Instance);
            Ptz = new PtzService(NullLogger<PtzService>.Instance);
            Ptz.Register(Commands);
        }

        public SerialDispatcher Dispatcher { get; } = new();
        public EngineHost Host { get; }
        public ProductionService Production { get; }
        public CommandService Commands { get; }
        public MacroRunner Macros { get; }
        public TallyService Tally { get; }
        public PtzService Ptz { get; }

        public Task<CommandResult> Run(string name, params (string, string)[] args) => Commands.ExecuteAsync(CommandRequest.Of(name, args), "test");

        public void Dispose()
        {
            Tally.Dispose();
            Ptz.Dispose();
            Production.Dispose();
            Host.Dispose();
            Dispatcher.Dispose();
        }
    }

    [Fact]
    public void MacroTextRoundTripsWithQuotesAndWaitShorthand()
    {
        Macro m = Macro.Parse("Intro", """
            # opening
            SetText input="Lower third" field=name value="Siti \"Rahma\""
            Wait 500
            OverlayIn channel=1 fade=15

            Auto kind=mix frames=30
            """);
        Assert.Equal(4, m.Steps.Count);
        Assert.Equal("Lower third", m.Steps[0].Args["input"]);
        Assert.Equal("Siti \"Rahma\"", m.Steps[0].Args["value"]);
        Assert.Equal("500", m.Steps[1].Args["ms"]);
        Macro again = Macro.Parse("Intro", m.ToText());
        Assert.Equal(m.Steps.Count, again.Steps.Count);
        for (int i = 0; i < m.Steps.Count; i++)
        {
            Assert.Equal(m.Steps[i].Command, again.Steps[i].Command);
            Assert.Equal<KeyValuePair<string, string>>(m.Steps[i].Args.OrderBy(kv => kv.Key), again.Steps[i].Args.OrderBy(kv => kv.Key));
        }
        FormatException ex = Assert.Throws<FormatException>(() => Macro.Parse("Bad", "Cut\nSetText value=\"open"));
        Assert.StartsWith("Line 2", ex.Message);
        Assert.Throws<FormatException>(() => Macro.Parse("Bad", "SetPreview 2"));
    }

    [Fact]
    public async Task CommandsDriveTheProductionAndReportErrors()
    {
        using var rig = new Rig();
        ProductionInput a = rig.Production.AddTestSignal();
        ProductionInput b = rig.Production.AddTestSignal();
        Assert.False((await rig.Run("Cut")).Ok);  // not running
        Assert.Equal("Unknown command 'Nope'.", (await rig.Run("Nope")).Error);
        Assert.Contains("missing argument 'input'", (await rig.Run("SetPreview")).Error);
        Assert.True((await rig.Run("StartProduction")).Ok);
        try
        {
            Assert.True((await rig.Run("SetPreview", ("input", "2"))).Ok);
            Assert.Equal(b.Id, rig.Production.GetStatistics()!.Value.PreviewInputId);
            Assert.True((await rig.Run("Cut")).Ok);
            Assert.Equal(b.Id, rig.Production.GetStatistics()!.Value.ProgramInputId);
            Assert.True((await rig.Run("SetProgram", ("input", "Test signal 1"))).Ok);
            Assert.Equal(a.Id, rig.Production.GetStatistics()!.Value.ProgramInputId);
            Assert.True((await rig.Run("SetPreview", ("input", $"id:{b.Id}"))).Ok);
            Assert.Contains("No input 7", (await rig.Run("SetPreview", ("input", "7"))).Error);
            Assert.Contains("must be a whole number", (await rig.Run("Auto", ("frames", "abc"))).Error);
            Assert.True((await rig.Run("Auto", ("kind", "mix"), ("frames", "10"))).Ok);
            await Task.Delay(600);
            Assert.Equal(b.Id, rig.Production.GetStatistics()!.Value.ProgramInputId);
            Assert.True((await rig.Run("PictureInPicture", ("input", "1"))).Ok);
            Assert.Equal(a.Id, rig.Production.PictureInPictureInputId);

            Assert.True((await rig.Run("SetVolume", ("input", "1"), ("db", "-6.5"))).Ok);
            Assert.Equal(-6.5f, rig.Production.GetAudioStrip(a.Id).FaderDb);
            Assert.True((await rig.Run("Mute", ("input", "1"))).Ok);
            Assert.True(rig.Production.GetAudioStrip(a.Id).Mute);
            Assert.True((await rig.Run("Mute", ("input", "1"), ("value", "toggle"))).Ok);
            Assert.False(rig.Production.GetAudioStrip(a.Id).Mute);

            // Tally: program = b (after AUTO) + PiP a; nothing on preview but a, which is on program.
            rig.Tally.Refresh();
            Assert.True(rig.Tally.Current.Single(t => t.InputId == b.Id).Program);
            Assert.True(rig.Tally.Current.Single(t => t.InputId == a.Id).Program);
            Assert.True((await rig.Run("PictureInPicture", ("input", "none"))).Ok);
            rig.Tally.Refresh();
            TallyEntry ta = rig.Tally.Current.Single(t => t.InputId == a.Id);
            Assert.False(ta.Program);
            Assert.True(ta.Preview);
        }
        finally
        {
            await rig.Run("StopProduction");
        }
    }

    [Fact]
    public async Task MacrosRunInOrderStopOnErrorsAndRefuseEndlessNesting()
    {
        using var rig = new Rig();
        rig.Production.AddTestSignal();
        rig.Production.AddTestSignal();
        await rig.Production.StartAsync();
        try
        {
            rig.Macros.SetMacros([
                Macro.Parse("Swap", "SetPreview input=2\nWait 50\nCut"),
                Macro.Parse("Broken", "SetPreview input=9\nCut"),
                Macro.Parse("Lenient", "SetPreview input=9\nSetPreview input=2", stopOnError: false),
                Macro.Parse("Loop", "RunMacro name=Loop wait=true"),
            ]);
            MacroRun swap = await rig.Macros.RunAsync("Swap");
            Assert.True(swap.Ok);
            Assert.Equal(3, swap.StepsRun);
            Assert.Equal(rig.Production.Inputs[1].Id, rig.Production.GetStatistics()!.Value.ProgramInputId);

            MacroRun broken = await rig.Macros.RunAsync("Broken");
            Assert.False(broken.Ok);
            Assert.Equal(1, broken.StepsRun);
            Assert.Contains("step 1 (SetPreview)", broken.Error);

            Assert.Equal(2, (await rig.Macros.RunAsync("Lenient")).StepsRun);

            MacroRun loop = await rig.Macros.RunAsync("Loop");
            Assert.False(loop.Ok);
            Assert.Contains("nested deeper", loop.Error);

            // Through the command layer (as the remote API and automation do), and cancellation.
            rig.Macros.Save(Macro.Parse("Long", "Wait 5000\nCut"));
            Task<MacroRun> longRun = rig.Macros.RunAsync("Long");
            await Task.Delay(100);
            Assert.True((await rig.Run("StopMacros")).Ok);
            MacroRun stopped = await longRun.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.True(stopped.Cancelled);
            Assert.True((await rig.Run("RunMacro", ("name", "Swap"), ("wait", "true"))).Ok);
            Assert.Contains("No macro", (await rig.Run("RunMacro", ("name", "Missing"))).Error);
        }
        finally
        {
            await rig.Production.StopAsync();
        }
    }

    [Fact]
    public async Task AutomationFiresOnlyWhenArmedAndOnlyOnce()
    {
        using var rig = new Rig();
        var now = new DateTime(2026, 11, 21, 18, 59, 58);
        var scheduler = new AutomationScheduler(rig.Macros, NullLogger<AutomationScheduler>.Instance, () => now);
        var fired = new List<string>();
        scheduler.Fired += (_, r) => fired.Add(r.Name);
        rig.Macros.SetMacros([Macro.Parse("Noop", "Wait 1")]);
        scheduler.SetRules([
            new AutomationRule { Name = "Seven", Trigger = TriggerKind.DailyAt, TimeOfDay = new TimeOnly(19, 0), Macro = "Noop" },
            new AutomationRule { Name = "Every10", Trigger = TriggerKind.Interval, IntervalSeconds = 10, Macro = "Noop" },
            new AutomationRule { Name = "OnStart", Trigger = TriggerKind.ProductionStarted, Macro = "Noop" },
            new AutomationRule { Name = "Later", Trigger = TriggerKind.Once, At = new DateTime(2026, 11, 21, 19, 0, 5), Macro = "Noop" },
            new AutomationRule { Name = "Off", Trigger = TriggerKind.Once, At = new DateTime(2026, 11, 21, 19, 0, 1), Macro = "Noop", Enabled = false },
        ]);
        Assert.Throws<ArgumentException>(() => scheduler.SetRules([new AutomationRule { Name = "x", Trigger = TriggerKind.Interval, IntervalSeconds = 1, Macro = "Noop" }]));

        now = now.AddSeconds(5);  // 19:00:03 — disarmed: nothing
        scheduler.Tick();
        scheduler.ProductionStarted();
        Assert.Empty(fired);

        scheduler.Armed = true;  // times that passed while disarmed are not caught up
        scheduler.Tick();
        Assert.Empty(fired);
        for (int i = 0; i < 12; i++)
        {
            now = now.AddSeconds(1);
            scheduler.Tick();
        }
        Assert.Equal(new[] { "Later", "Every10" }, fired);  // 19:00:05 once; interval after 10 s
        scheduler.ProductionStarted();
        Assert.Equal("OnStart", fired[^1]);

        fired.Clear();
        now = new DateTime(2026, 11, 22, 18, 59, 59);
        scheduler.Tick();  // jump (PC slept): no catch-up of the missed interval/once rules beyond one fire
        fired.Clear();
        now = now.AddSeconds(2);  // crosses 19:00 the next day
        scheduler.Tick();
        Assert.Contains("Seven", fired);
        now = now.AddSeconds(1);
        scheduler.Tick();
        Assert.Equal(1, fired.Count(f => f == "Seven"));
        await Task.Delay(50);  // let the fire-and-forget macros finish
    }

    [Fact]
    public async Task TallyIsSentAsTslUmd5()
    {
        using var rig = new Rig();
        ProductionInput a = rig.Production.AddTestSignal();
        rig.Production.AddTestSignal();
        using var listener = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        int port = ((IPEndPoint)listener.Client.LocalEndPoint!).Port;
        await rig.Production.StartAsync();
        try
        {
            rig.Tally.SetDestinations([new TallyDestination("127.0.0.1", port, Screen: 3)]);
            rig.Tally.Refresh();
            byte[] packet = await ReceiveLast(listener);
            int pbc = packet[0] | (packet[1] << 8);
            Assert.Equal(packet.Length - 2, pbc);
            Assert.Equal(0, packet[2]);  // version
            Assert.Equal(3, packet[4] | (packet[5] << 8));  // screen
            // first display message: input 1 → index 1, program (red = 1 in every lamp field), full brightness
            Assert.Equal(1, packet[6] | (packet[7] << 8));
            int control = packet[8] | (packet[9] << 8);
            Assert.Equal(1 | (1 << 2) | (1 << 4) | (3 << 6), control);
            int length = packet[10] | (packet[11] << 8);
            Assert.Equal(a.Name, Encoding.ASCII.GetString(packet, 12, length));
            // second: preview (green = 2)
            int second = 12 + length;
            Assert.Equal(2, packet[second] | (packet[second + 1] << 8));
            Assert.Equal(2 | (2 << 2) | (2 << 4) | (3 << 6), packet[second + 2] | (packet[second + 3] << 8));
        }
        finally
        {
            await rig.Production.StopAsync();
        }
    }

    private static async Task<byte[]> ReceiveLast(UdpClient listener)
    {
        UdpReceiveResult r = await listener.ReceiveAsync().WaitAsync(TimeSpan.FromSeconds(2));
        while (listener.Available > 0) r = await listener.ReceiveAsync();
        return r.Buffer;
    }

    [Fact]
    public async Task PtzCommandsSendViscaOverIp()
    {
        Assert.Equal<byte>(new byte[] { 0x81, 0x01, 0x04, 0x3F, 0x02, 0x05, 0xFF }, Visca.PresetRecall(1, 5));
        Assert.Equal<byte>(new byte[] { 0x82, 0x01, 0x04, 0x3F, 0x01, 0x0A, 0xFF }, Visca.PresetStore(2, 10));
        Assert.Equal<byte>(new byte[] { 0x81, 0x01, 0x06, 0x01, 0x08, 0x08, 0x01, 0x01, 0xFF }, Visca.PanTilt(1, -1, 1, 8, 8));  // up-left
        Assert.Equal<byte>(new byte[] { 0x81, 0x01, 0x06, 0x01, 0x01, 0x01, 0x03, 0x03, 0xFF }, Visca.PanTilt(1, 0, 0, 1, 1));  // stop
        Assert.Equal<byte>(new byte[] { 0x81, 0x01, 0x04, 0x07, 0x23, 0xFF }, Visca.Zoom(1, 1, 3));
        Assert.Equal<byte>(new byte[] { 0x81, 0x01, 0x04, 0x07, 0x37, 0xFF }, Visca.Zoom(1, -1, 9));  // speed clamped to 7
        Assert.Equal<byte>(new byte[] { 0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x2A, 0x81, 0x01, 0x04, 0x3F, 0x02, 0x05, 0xFF },
            Visca.SonyFrame(Visca.PresetRecall(1, 5), 42));

        using var rig = new Rig();
        using var sony = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        using var raw = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        rig.Ptz.SetCameras([
            new PtzCamera("Stage", "127.0.0.1", ((IPEndPoint)sony.Client.LocalEndPoint!).Port, ViscaTransport.SonyUdp),
            new PtzCamera("Pulpit", "127.0.0.1", ((IPEndPoint)raw.Client.LocalEndPoint!).Port, ViscaTransport.RawUdp, Address: 2),
        ]);
        Assert.True((await rig.Run("PtzPreset", ("camera", "Stage"), ("preset", "3"))).Ok);
        byte[] s = (await sony.ReceiveAsync().WaitAsync(TimeSpan.FromSeconds(2))).Buffer;
        Assert.Equal<byte>(new byte[] { 0x81, 0x01, 0x04, 0x3F, 0x02, 0x03, 0xFF }, s[8..]);
        Assert.Equal(0x01, s[0]);
        Assert.False((await rig.Run("PtzMove", ("camera", "2"), ("direction", "right"), ("speed", "30"))).Ok);  // speed out of range
        Assert.True((await rig.Run("PtzZoom", ("camera", "2"), ("direction", "in"))).Ok);
        byte[] r = (await raw.ReceiveAsync().WaitAsync(TimeSpan.FromSeconds(2))).Buffer;
        Assert.Equal<byte>(new byte[] { 0x82, 0x01, 0x04, 0x07, 0x23, 0xFF }, r);
        Assert.Contains("No PTZ camera", (await rig.Run("PtzHome", ("camera", "Balcony"))).Error);
        Assert.Throws<ArgumentException>(() => rig.Ptz.SetCameras([new PtzCamera("A", "h", 1), new PtzCamera("a", "h", 2)]));
    }

    [Fact]
    public async Task RemoteApiNeedsTheTokenAndRunsCommandsAndStreamsEvents()
    {
        using var rig = new Rig();
        rig.Production.AddTestSignal();
        rig.Production.AddTestSignal();
        rig.Macros.SetMacros([Macro.Parse("Swap", "SetPreview input=2\nCut")]);
        Func<Task<ProductionState>> snapshot = () => rig.Dispatcher.InvokeAsync(() =>
        {
            rig.Tally.Refresh();
            return Task.FromResult(ProductionCommands.Snapshot(rig.Production, rig.Tally.Current));
        });
        await using var server = new RemoteControlServer(rig.Commands, rig.Macros, rig.Tally, snapshot, NullLogger<RemoteControlServer>.Instance);
        int port = FreePort();
        string token = RemoteControlServer.NewToken();
        await Assert.ThrowsAsync<ArgumentException>(() => server.StartAsync(new RemoteSettings { Port = port, Token = "short" }));
        await server.StartAsync(new RemoteSettings { Enabled = true, Port = port, Token = token });
        await rig.Production.StartAsync();
        try
        {
            using var http = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };
            Assert.Equal(HttpStatusCode.OK, (await http.GetAsync("/")).StatusCode);  // the control page (no data)
            Assert.Equal(HttpStatusCode.Unauthorized, (await http.GetAsync("/api/v1/state")).StatusCode);
            http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token + "x");
            Assert.Equal(HttpStatusCode.Unauthorized, (await http.GetAsync("/api/v1/state")).StatusCode);
            http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);

            ProductionState state = (await http.GetFromJsonAsync<ProductionState>("/api/v1/state", ProtocolInfo.Json))!;
            Assert.True(state.Running);
            Assert.Equal(2, state.Inputs.Count);
            Assert.True(state.Tally[0].Program);

            CommandDescriptor[] commands = (await http.GetFromJsonAsync<CommandDescriptor[]>("/api/v1/commands", ProtocolInfo.Json))!;
            Assert.Contains(commands, c => c.Name == "Cut");

            // WebSocket: initial state, then a tally event after a command sent over the socket.
            using var ws = new ClientWebSocket();
            await ws.ConnectAsync(new Uri($"ws://127.0.0.1:{port}/api/v1/events?token={token}"), CancellationToken.None);
            Assert.Equal("state", (await NextEvent(ws)).Type);

            HttpResponseMessage post = await http.PostAsync("/api/v1/commands/SetPreview", Json("""{"args":{"input":2}}"""));
            Assert.Equal(HttpStatusCode.OK, post.StatusCode);
            Assert.Equal(HttpStatusCode.NotFound, (await http.PostAsync("/api/v1/commands/Nope", Json("{}"))).StatusCode);
            HttpResponseMessage bad = await http.PostAsync("/api/v1/commands/SetPreview", Json("""{"input":"9"}"""));
            Assert.Equal(HttpStatusCode.BadRequest, bad.StatusCode);
            Assert.Contains("No input 9", (await bad.Content.ReadFromJsonAsync<CommandResult>(ProtocolInfo.Json))!.Error);
            Assert.Equal(HttpStatusCode.BadRequest, (await http.PostAsync("/api/v1/commands/Cut", Json("[1,2"))).StatusCode);

            await ws.SendAsync(Encoding.UTF8.GetBytes("""{"command":"Cut","id":"c1"}"""), WebSocketMessageType.Text, true, CancellationToken.None);
            ServerEvent result;
            do result = await NextEvent(ws);
            while (result.Type != "result");
            Assert.Equal("c1", result.Data.GetProperty("id").GetString());
            Assert.True(result.Data.GetProperty("result").GetProperty("ok").GetBoolean());

            rig.Tally.Refresh();  // the host refreshes tally ~10x/s; here by hand
            ServerEvent tally;
            do tally = await NextEvent(ws);
            while (tally.Type != "tally");
            Assert.True(tally.Data[1].GetProperty("program").GetBoolean());

            Assert.Equal(HttpStatusCode.OK, (await http.PostAsync("/api/v1/macros/Swap/run", null)).StatusCode);
            Assert.Equal(1, server.ClientCount);
        }
        finally
        {
            await rig.Production.StopAsync();
            await server.StopAsync();
        }
        Assert.False(server.IsRunning);
    }

    private static StringContent Json(string s) => new(s, Encoding.UTF8, "application/json");

    private static async Task<ServerEvent> NextEvent(ClientWebSocket ws)
    {
        var buffer = new byte[64 * 1024];
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        WebSocketReceiveResult r = await ws.ReceiveAsync(buffer, cts.Token);
        return JsonSerializer.Deserialize<ServerEvent>(buffer.AsSpan(0, r.Count), ProtocolInfo.Json)!;
    }

    private static int FreePort()
    {
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        int port = ((IPEndPoint)l.LocalEndpoint).Port;
        l.Stop();
        return port;
    }

    private sealed class SampleExtension : IMixItExtension
    {
        public string Id => "test.sample";
        public string DisplayName => "Sample";
        public IExtensionHost? Host { get; private set; }
        public int TallyEvents { get; private set; }
        public bool Disposed { get; private set; }

        public void Initialize(IExtensionHost host)
        {
            Host = host;
            host.TallyChanged += (_, _) => TallyEvents++;
            host.RegisterCommand(new CommandDescriptor("Sample.Echo", "Echo", [new("text", ParameterKind.String, true, "")]),
                r => Task.FromResult(CommandResult.Success(r.Args["text"])));
            host.RegisterCommand(new CommandDescriptor("Sample.Throw", "Throws", []), _ => throw new InvalidCastException("boom"));
            host.CommandExecuted += (_, _) => throw new InvalidCastException("handler bug");
        }

        public void Dispose() => Disposed = true;
    }

    private sealed class BrokenExtension : IMixItExtension
    {
        public string Id => "test.broken";
        public string DisplayName => "Broken";
        public void Initialize(IExtensionHost host) => throw new InvalidCastException("cannot start");
        public void Dispose() { }
    }

    [Fact]
    public async Task ExtensionsAddCommandsAndTheirFailuresAreIsolated()
    {
        using var rig = new Rig();
        rig.Production.AddTestSignal();
        var manager = new ExtensionManager(rig.Commands, rig.Tally, NullLogger<ExtensionManager>.Instance);
        var sample = new SampleExtension();
        manager.Add(sample);
        manager.Add(new BrokenExtension());
        manager.LoadFolder(Path.Combine(Path.GetTempPath(), "mixit-no-such-folder"));
        Assert.True(manager.Extensions[0].Ok);
        Assert.False(manager.Extensions[1].Ok);
        Assert.Equal("cannot start", manager.Extensions[1].Error);

        CommandResult echo = await rig.Run("Sample.Echo", ("text", "hello"));  // its throwing event handler is contained
        Assert.True(echo.Ok);
        Assert.Equal("hello", echo.Value);
        Assert.Contains("extension test.sample failed: boom", (await rig.Run("Sample.Throw")).Error);
        Assert.Equal("Extensions", rig.Commands.Commands.Single(c => c.Name == "Sample.Echo").Category);
        CommandResult viaHost = await sample.Host!.ExecuteAsync(CommandRequest.Of("Sample.Echo", ("text", "x")));
        Assert.Equal("x", viaHost.Value);
        rig.Tally.Refresh();
        Assert.Equal(1, sample.TallyEvents);
        manager.Dispose();
        Assert.True(sample.Disposed);
        rig.Production.RemoveInput(rig.Production.Inputs[0].Id);
        rig.Tally.Refresh();
        Assert.Equal(1, sample.TallyEvents);  // detached
    }

    [Fact]
    public void ControlSettingsSurviveASaveAndABrokenFileIsKeptAside()
    {
        string dir = Path.Combine(Path.GetTempPath(), "mixit_control_settings");
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        var store = new ControlSettingsStore(Path.Combine(dir, "control.json"), NullLogger<ControlSettingsStore>.Instance);
        Assert.Empty(store.Load().Macros);
        var settings = new ControlSettings
        {
            Macros = [new MacroDocument("Intro", "Cut\nWait 100")],
            Rules = [new AutomationRule { Name = "Seven", Trigger = TriggerKind.DailyAt, TimeOfDay = new TimeOnly(19, 0), Macro = "Intro" }],
            Tally = [new TallyDestination("10.0.0.5", 8900)],
            Cameras = [new PtzCamera("Stage", "10.0.0.20", 52381)],
            Remote = new RemoteSettings { Enabled = true, Port = 9000, Token = "secret-token-1234567890" },
        };
        store.Save(settings);
        ControlSettings back = store.Load();
        Assert.Equal("Cut\nWait 100", back.Macros[0].Text);
        Assert.Equal(new TimeOnly(19, 0), back.Rules[0].TimeOfDay);
        Assert.Equal(TriggerKind.DailyAt, back.Rules[0].Trigger);
        Assert.Equal(8900, back.Tally[0].Port);
        Assert.Equal(ViscaTransport.SonyUdp, back.Cameras[0].Transport);
        Assert.Equal(9000, back.Remote.Port);

        File.WriteAllText(store.Path, "{ not json");
        Assert.Empty(store.Load().Macros);
        Assert.Single(Directory.GetFiles(dir, "control.json.broken-*"));
    }
}
