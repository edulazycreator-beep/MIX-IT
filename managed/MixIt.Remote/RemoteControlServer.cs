using System.Net.WebSockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using MixIt.Core.Control;
using MixIt.Protocol;

namespace MixIt.Remote;

/// <summary>
/// Remote control (M7, ADR-0012): a documented HTTP + WebSocket API
/// (docs/REMOTE_API.md) on Kestrel. Every request needs the bearer token;
/// localhost only unless the operator allows the network. Commands go through
/// the same <see cref="CommandService"/> as the UI. A slow or vanished client
/// only loses its own events (bounded queue per client).
/// </summary>
public sealed class RemoteControlServer : IAsyncDisposable, IDisposable
{
    private const int MaxClients = 32;
    private const int ClientQueue = 256;
    private readonly CommandService _commands;
    private readonly MacroRunner _macros;
    private readonly Func<Task<ProductionState>> _snapshot;
    private readonly TallyService _tally;
    private readonly ILogger<RemoteControlServer> _logger;
    private readonly object _clientsLock = new();
    private readonly List<Channel<string>> _clients = [];
    private WebApplication? _app;
    private byte[] _tokenHash = [];

    public RemoteControlServer(CommandService commands, MacroRunner macros, TallyService tally,
        Func<Task<ProductionState>> snapshot, ILogger<RemoteControlServer> logger)
    {
        _commands = commands;
        _macros = macros;
        _tally = tally;
        _snapshot = snapshot;
        _logger = logger;
        _tally.Changed += (_, t) => Publish("tally", t);
        _commands.Executed += (_, e) => Publish("command", new { name = e.Request.Name, ok = e.Result.Ok, error = e.Result.Error });
        _macros.Finished += (_, r) => Publish("macro", r);
    }

    public bool IsRunning => _app is not null;

    /// <summary>Listening address (http://host:port), null when stopped.</summary>
    public string? Address { get; private set; }

    public int ClientCount
    {
        get
        {
            lock (_clientsLock) return _clients.Count;
        }
    }

    /// <summary>A new random token (256 bits, URL-safe).</summary>
    public static string NewToken() => Convert.ToBase64String(RandomNumberGenerator.GetBytes(32))
        .TrimEnd('=').Replace('+', '-').Replace('/', '_');

    public async Task StartAsync(RemoteSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        if (settings.Token.Length < 16) throw new ArgumentException("The remote-control token must be at least 16 characters.");
        if (settings.Port is < 1024 or > 65535) throw new ArgumentException("Port must be 1024..65535.");
        await StopAsync().ConfigureAwait(false);
        _tokenHash = SHA256.HashData(Encoding.UTF8.GetBytes(settings.Token));

        WebApplicationBuilder builder = WebApplication.CreateSlimBuilder();
        builder.Logging.ClearProviders();  // our own log lines below; Kestrel's go nowhere
        builder.WebHost.ConfigureKestrel(k =>
        {
            if (settings.AllowNetwork) k.ListenAnyIP(settings.Port);
            else k.ListenLocalhost(settings.Port);
            k.Limits.MaxRequestBodySize = 64 * 1024;
            k.Limits.MaxConcurrentConnections = 64;
            k.AddServerHeader = false;
        });
        WebApplication app = builder.Build();
        app.UseWebSockets(new WebSocketOptions { KeepAliveInterval = TimeSpan.FromSeconds(20) });
        app.Use(Authenticate);
        Map(app);
        try
        {
            await app.StartAsync().ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is IOException or System.Net.Sockets.SocketException or InvalidOperationException)
        {
            await app.DisposeAsync().ConfigureAwait(false);
            throw new InvalidOperationException($"Remote control could not listen on port {settings.Port}: {ex.Message}", ex);
        }
        _app = app;
        Address = $"http://{(settings.AllowNetwork ? "<this-pc>" : "localhost")}:{settings.Port}";
        _logger.LogInformation("Remote control listening on port {Port} ({Scope})", settings.Port, settings.AllowNetwork ? "network" : "localhost only");
    }

    public async Task StopAsync()
    {
        WebApplication? app = _app;
        if (app is null) return;
        _app = null;
        Address = null;
        lock (_clientsLock)
        {
            foreach (Channel<string> c in _clients) c.Writer.TryComplete();
        }
        using (var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2)))
        {
            await app.StopAsync(timeout.Token).ConfigureAwait(false);
        }
        await app.DisposeAsync().ConfigureAwait(false);
        _logger.LogInformation("Remote control stopped");
    }

    public async ValueTask DisposeAsync() => await StopAsync().ConfigureAwait(false);

    /// <summary>For synchronous owners (DI containers): stops within a few seconds, never hangs shutdown.</summary>
    public void Dispose()
    {
        try
        {
            Task.Run(StopAsync).Wait(TimeSpan.FromSeconds(4));
        }
        catch (AggregateException ex)
        {
            _logger.LogWarning("Remote control did not stop cleanly: {Message}", ex.InnerException?.Message);
        }
    }

    /// <summary>Sends an event to every connected WebSocket client (never blocks).</summary>
    public void Publish(string type, object data)
    {
        lock (_clientsLock)
        {
            if (_clients.Count == 0) return;
        }
        string json = JsonSerializer.Serialize(new ServerEvent(type, DateTimeOffset.Now, JsonSerializer.SerializeToElement(data, ProtocolInfo.Json)), ProtocolInfo.Json);
        lock (_clientsLock)
        {
            foreach (Channel<string> c in _clients) c.Writer.TryWrite(json);  // bounded, drop-oldest
        }
    }

    private async Task Authenticate(HttpContext context, Func<Task> next)
    {
        string path = context.Request.Path.Value ?? "/";
        if (path is "/" or "/index.html")
        {
            await next().ConfigureAwait(false);  // the control page itself holds no data; it asks for the token
            return;
        }
        string? token = null;
        string auth = context.Request.Headers.Authorization.ToString();
        if (auth.StartsWith("Bearer ", StringComparison.Ordinal)) token = auth[7..].Trim();
        // Browsers cannot set headers on WebSockets: the event stream also accepts ?token=.
        else if (context.WebSockets.IsWebSocketRequest) token = context.Request.Query["token"].ToString();
        if (token is null || !CryptographicOperations.FixedTimeEquals(SHA256.HashData(Encoding.UTF8.GetBytes(token)), _tokenHash))
        {
            context.Response.StatusCode = StatusCodes.Status401Unauthorized;
            context.Response.Headers.WWWAuthenticate = "Bearer";
            await context.Response.WriteAsJsonAsync(CommandResult.Failure("missing or wrong token"), ProtocolInfo.Json).ConfigureAwait(false);
            return;
        }
        await next().ConfigureAwait(false);
    }

    private void Map(WebApplication app)
    {
        string b = ProtocolInfo.BasePath;
        app.MapGet("/", async context =>
        {
            context.Response.ContentType = "text/html; charset=utf-8";
            context.Response.Headers.CacheControl = "no-store";
            context.Response.Headers["Content-Security-Policy"] = "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self' ws: wss:";
            using Stream page = Assembly.GetExecutingAssembly().GetManifestResourceStream("MixIt.Remote.WebPanel.html")!;
            await page.CopyToAsync(context.Response.Body).ConfigureAwait(false);
        });
        app.MapGet(b + "/info", () => Results.Json(new { product = "MIX IT", protocol = ProtocolInfo.Version, sdk = SDK.SdkInfo.Version }, ProtocolInfo.Json));
        app.MapGet(b + "/state", async () => Results.Json(await _snapshot().ConfigureAwait(false), ProtocolInfo.Json));
        app.MapGet(b + "/tally", () => Results.Json(_tally.Current, ProtocolInfo.Json));
        app.MapGet(b + "/commands", () => Results.Json(_commands.Commands, ProtocolInfo.Json));
        app.MapGet(b + "/macros", () => Results.Json(_macros.Macros.Select(m => new { name = m.Name, steps = m.Steps.Count }), ProtocolInfo.Json));
        app.MapPost(b + "/commands/{name}", async (string name, HttpContext context) =>
        {
            Dictionary<string, string> args;
            try
            {
                args = await ReadArgsAsync(context).ConfigureAwait(false);
            }
            catch (JsonException ex)
            {
                return Results.Json(CommandResult.Failure($"invalid JSON: {ex.Message}"), ProtocolInfo.Json, statusCode: 400);
            }
            if (!_commands.Contains(name)) return Results.Json(CommandResult.Failure($"Unknown command '{name}'."), ProtocolInfo.Json, statusCode: 404);
            CommandResult r = await _commands.ExecuteAsync(new CommandRequest(name, args), Source(context)).ConfigureAwait(false);
            return Results.Json(r, ProtocolInfo.Json, statusCode: r.Ok ? 200 : 400);
        });
        app.MapPost(b + "/macros/{name}/run", async (string name, HttpContext context) =>
        {
            CommandResult r = await _commands.ExecuteAsync(CommandRequest.Of("RunMacro", ("name", name)), Source(context)).ConfigureAwait(false);
            return Results.Json(r, ProtocolInfo.Json, statusCode: r.Ok ? 200 : 400);
        });
        app.Map(b + "/events", EventsAsync);
    }

    private static string Source(HttpContext c) => $"remote {c.Connection.RemoteIpAddress}";

    /// <summary>Body: <c>{"args": {...}}</c> or a flat object; values may be strings, numbers or booleans.</summary>
    private static async Task<Dictionary<string, string>> ReadArgsAsync(HttpContext context)
    {
        var args = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (context.Request.ContentLength is 0 or null && !context.Request.Headers.TransferEncoding.Any()) return args;
        using JsonDocument doc = await JsonDocument.ParseAsync(context.Request.Body).ConfigureAwait(false);
        JsonElement root = doc.RootElement;
        if (root.ValueKind == JsonValueKind.Object && root.TryGetProperty("args", out JsonElement inner) && inner.ValueKind == JsonValueKind.Object)
            root = inner;
        if (root.ValueKind is not (JsonValueKind.Object or JsonValueKind.Undefined or JsonValueKind.Null))
            throw new JsonException("expected an object");
        if (root.ValueKind != JsonValueKind.Object) return args;
        foreach (JsonProperty p in root.EnumerateObject())
        {
            args[p.Name] = p.Value.ValueKind switch
            {
                JsonValueKind.String => p.Value.GetString()!,
                JsonValueKind.Number or JsonValueKind.True or JsonValueKind.False => p.Value.GetRawText(),
                JsonValueKind.Null => "",
                _ => throw new JsonException($"argument '{p.Name}' must be a string, number or boolean"),
            };
        }
        return args;
    }

    private async Task EventsAsync(HttpContext context)
    {
        if (!context.WebSockets.IsWebSocketRequest)
        {
            context.Response.StatusCode = StatusCodes.Status400BadRequest;
            return;
        }
        var channel = Channel.CreateBounded<string>(new BoundedChannelOptions(ClientQueue)
        {
            FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true,
        });
        lock (_clientsLock)
        {
            if (_clients.Count >= MaxClients)
            {
                context.Response.StatusCode = StatusCodes.Status503ServiceUnavailable;
                return;
            }
            _clients.Add(channel);
        }
        using WebSocket socket = await context.WebSockets.AcceptWebSocketAsync().ConfigureAwait(false);
        _logger.LogInformation("Remote client connected from {Address}", context.Connection.RemoteIpAddress);
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(context.RequestAborted);
        try
        {
            ProductionState state = await _snapshot().ConfigureAwait(false);
            channel.Writer.TryWrite(JsonSerializer.Serialize(new ServerEvent("state", DateTimeOffset.Now,
                JsonSerializer.SerializeToElement(state, ProtocolInfo.Json)), ProtocolInfo.Json));
            Task send = SendLoopAsync(socket, channel.Reader, cts.Token);
            await ReceiveLoopAsync(socket, channel.Writer, context, cts.Token).ConfigureAwait(false);
            await cts.CancelAsync().ConfigureAwait(false);
            await send.ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is WebSocketException or OperationCanceledException or IOException)
        {
            // client went away
        }
        finally
        {
            lock (_clientsLock) _clients.Remove(channel);
            _logger.LogInformation("Remote client {Address} disconnected", context.Connection.RemoteIpAddress);
        }
    }

    private static async Task SendLoopAsync(WebSocket socket, ChannelReader<string> reader, CancellationToken token)
    {
        try
        {
            await foreach (string message in reader.ReadAllAsync(token).ConfigureAwait(false))
            {
                await socket.SendAsync(Encoding.UTF8.GetBytes(message), WebSocketMessageType.Text, true, token).ConfigureAwait(false);
            }
        }
        catch (Exception ex) when (ex is WebSocketException or OperationCanceledException or IOException)
        {
            // closing
        }
    }

    /// <summary>Clients may send <c>{"command": "Cut", "args": {...}, "id": "x"}</c>; the result comes back as a "result" event.</summary>
    private async Task ReceiveLoopAsync(WebSocket socket, ChannelWriter<string> writer, HttpContext context, CancellationToken token)
    {
        var buffer = new byte[16 * 1024];
        while (socket.State == WebSocketState.Open && !token.IsCancellationRequested)
        {
            using var message = new MemoryStream();
            WebSocketReceiveResult r;
            do
            {
                r = await socket.ReceiveAsync(buffer, token).ConfigureAwait(false);
                if (r.MessageType == WebSocketMessageType.Close)
                {
                    await socket.CloseAsync(WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None).ConfigureAwait(false);
                    return;
                }
                message.Write(buffer, 0, r.Count);
                if (message.Length > 64 * 1024)
                {
                    await socket.CloseAsync(WebSocketCloseStatus.MessageTooBig, "message too big", CancellationToken.None).ConfigureAwait(false);
                    return;
                }
            }
            while (!r.EndOfMessage);
            object reply;
            try
            {
                using JsonDocument doc = JsonDocument.Parse(message.ToArray());
                JsonElement root = doc.RootElement;
                string name = root.GetProperty("command").GetString() ?? "";
                string? id = root.TryGetProperty("id", out JsonElement idEl) ? idEl.ToString() : null;
                var args = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                if (root.TryGetProperty("args", out JsonElement a) && a.ValueKind == JsonValueKind.Object)
                {
                    foreach (JsonProperty p in a.EnumerateObject()) args[p.Name] = p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString()! : p.Value.GetRawText();
                }
                CommandResult result = await _commands.ExecuteAsync(new CommandRequest(name, args), Source(context)).ConfigureAwait(false);
                reply = new { id, result };
            }
            catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException)
            {
                reply = new { id = (string?)null, result = CommandResult.Failure($"invalid message: {ex.Message}") };
            }
            writer.TryWrite(JsonSerializer.Serialize(new ServerEvent("result", DateTimeOffset.Now, JsonSerializer.SerializeToElement(reply, ProtocolInfo.Json)), ProtocolInfo.Json));
        }
    }
}
