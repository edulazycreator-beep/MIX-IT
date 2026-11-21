using System.Reflection;
using System.Runtime.Loader;
using Microsoft.Extensions.Logging;
using MixIt.Protocol;
using MixIt.SDK;

namespace MixIt.Core.Control;

/// <summary>An extension and how it loaded.</summary>
public sealed record LoadedExtension(string Id, string DisplayName, string Source, string? Error)
{
    public bool Ok => Error is null;
}

/// <summary>
/// Loads extensions (M7, EXPERIMENTAL): every <see cref="IMixItExtension"/>
/// type in the assemblies of <c>&lt;folder&gt;/&lt;name&gt;/&lt;name&gt;.dll</c>, each folder in its
/// own load context (MixIt.SDK and MixIt.Protocol are shared with the app).
/// Off unless the operator enables extensions. An extension that throws is
/// reported and isolated; it can still crash the process (same process).
/// </summary>
public sealed class ExtensionManager : IDisposable
{
    private readonly CommandService _commands;
    private readonly TallyService _tally;
    private readonly ILogger<ExtensionManager> _logger;
    private readonly List<(IMixItExtension Extension, Host Host)> _active = [];
    private readonly List<LoadedExtension> _loaded = [];

    public ExtensionManager(CommandService commands, TallyService tally, ILogger<ExtensionManager> logger)
    {
        _commands = commands;
        _tally = tally;
        _logger = logger;
    }

    public IReadOnlyList<LoadedExtension> Extensions => _loaded;

    public static string DefaultFolder() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "MIX IT", "Extensions");

    /// <summary>Loads every extension folder below <paramref name="folder"/> (call on the production thread).</summary>
    public void LoadFolder(string folder)
    {
        if (!Directory.Exists(folder)) return;
        foreach (string dir in Directory.EnumerateDirectories(folder).Order(StringComparer.OrdinalIgnoreCase))
        {
            string dll = Path.Combine(dir, Path.GetFileName(dir) + ".dll");
            if (!File.Exists(dll))
            {
                _loaded.Add(new LoadedExtension(Path.GetFileName(dir), Path.GetFileName(dir), dir, $"expected {Path.GetFileName(dll)} in the folder"));
                continue;
            }
            try
            {
                var context = new ExtensionLoadContext(dll);
                Assembly assembly = context.LoadFromAssemblyPath(dll);
                Type[] types = assembly.GetExportedTypes()
                    .Where(t => typeof(IMixItExtension).IsAssignableFrom(t) && t is { IsAbstract: false, IsInterface: false } && t.GetConstructor(Type.EmptyTypes) is not null)
                    .ToArray();
                if (types.Length == 0) _loaded.Add(new LoadedExtension(Path.GetFileName(dir), Path.GetFileName(dir), dll, "no IMixItExtension type"));
                foreach (Type t in types) Add((IMixItExtension)Activator.CreateInstance(t)!, dll);
            }
            catch (Exception ex) when (ex is BadImageFormatException or FileLoadException or FileNotFoundException or TypeLoadException
                                           or ReflectionTypeLoadException or TargetInvocationException or MissingMethodException)
            {
                _logger.LogError("Extension {Folder} could not be loaded: {Message}", dir, ex.Message);
                _loaded.Add(new LoadedExtension(Path.GetFileName(dir), Path.GetFileName(dir), dll, ex.Message));
            }
        }
    }

    /// <summary>Initializes an extension instance (also used for in-process extensions and tests).</summary>
    public void Add(IMixItExtension extension, string source = "(in process)")
    {
        ArgumentNullException.ThrowIfNull(extension);
        var host = new Host(this, extension);
        try
        {
            extension.Initialize(host);
            _active.Add((extension, host));
            _loaded.Add(new LoadedExtension(extension.Id, extension.DisplayName, source, null));
            _logger.LogInformation("Extension {Id} ({Name}) loaded from {Source}", extension.Id, extension.DisplayName, source);
        }
#pragma warning disable CA1031 // third-party code: any failure is reported, never fatal to the production
        catch (Exception ex)
#pragma warning restore CA1031
        {
            host.Detach();
            _loaded.Add(new LoadedExtension(SafeId(extension), SafeId(extension), source, ex.Message));
            _logger.LogError("Extension {Id} failed to initialize: {Message}", SafeId(extension), ex.Message);
        }
    }

    private static string SafeId(IMixItExtension e)
    {
        try
        {
            return e.Id;
        }
#pragma warning disable CA1031
        catch (Exception)
#pragma warning restore CA1031
        {
            return e.GetType().FullName ?? "?";
        }
    }

    public void Dispose()
    {
        foreach ((IMixItExtension e, Host h) in _active)
        {
            h.Detach();
            try
            {
                e.Dispose();
            }
#pragma warning disable CA1031
            catch (Exception ex)
#pragma warning restore CA1031
            {
                _logger.LogWarning("Extension {Id} failed to dispose: {Message}", SafeId(e), ex.Message);
            }
        }
        _active.Clear();
    }

    private sealed class Host : IExtensionHost
    {
        private readonly ExtensionManager _owner;
        private readonly IMixItExtension _extension;
        private bool _attached = true;

        public Host(ExtensionManager owner, IMixItExtension extension)
        {
            _owner = owner;
            _extension = extension;
            _owner._tally.Changed += OnTally;
            _owner._commands.Executed += OnCommand;
        }

        public string SdkVersion => SdkInfo.Version;

        public IReadOnlyList<CommandDescriptor> Commands => _owner._commands.Commands;

        public event EventHandler<IReadOnlyList<TallyEntry>>? TallyChanged;

        public event EventHandler<(CommandRequest Request, CommandResult Result)>? CommandExecuted;

        public Task<CommandResult> ExecuteAsync(CommandRequest request, CancellationToken cancellationToken = default) =>
            _owner._commands.ExecuteAsync(request, $"extension {SafeId(_extension)}");

        public void RegisterCommand(CommandDescriptor descriptor, Func<CommandRequest, Task<CommandResult>> handler)
        {
            ArgumentNullException.ThrowIfNull(handler);
            _owner._commands.Register(descriptor with { Category = "Extensions" }, async a =>
            {
                CommandResult r;
                try
                {
                    r = await handler(new CommandRequest(descriptor.Name, a.Raw)).ConfigureAwait(true);
                }
#pragma warning disable CA1031
                catch (Exception ex)
#pragma warning restore CA1031
                {
                    throw new CommandException($"extension {SafeId(_extension)} failed: {ex.Message}");
                }
                return r.Ok ? r.Value : throw new CommandException(r.Error ?? "failed");
            });
        }

        public void Log(string message, bool isError = false)
        {
            if (isError) _owner._logger.LogError("[{Id}] {Message}", SafeId(_extension), message);
            else _owner._logger.LogInformation("[{Id}] {Message}", SafeId(_extension), message);
        }

        public void Detach()
        {
            if (!_attached) return;
            _attached = false;
            _owner._tally.Changed -= OnTally;
            _owner._commands.Executed -= OnCommand;
        }

        private void OnTally(object? sender, IReadOnlyList<TallyEntry> e) => Guard(() => TallyChanged?.Invoke(this, e));

        private void OnCommand(object? sender, (CommandRequest, CommandResult) e) => Guard(() => CommandExecuted?.Invoke(this, e));

        private void Guard(Action a)
        {
            try
            {
                a();
            }
#pragma warning disable CA1031
            catch (Exception ex)
#pragma warning restore CA1031
            {
                _owner._logger.LogError("Extension {Id} event handler failed: {Message}", SafeId(_extension), ex.Message);
            }
        }
    }

    private sealed class ExtensionLoadContext(string mainAssembly) : AssemblyLoadContext(Path.GetFileNameWithoutExtension(mainAssembly))
    {
        private readonly AssemblyDependencyResolver _resolver = new(mainAssembly);

        protected override Assembly? Load(AssemblyName name)
        {
            // Contracts are shared with the host so types match.
            if (name.Name is "MixIt.SDK" or "MixIt.Protocol") return null;
            string? path = _resolver.ResolveAssemblyToPath(name);
            return path is null ? null : LoadFromAssemblyPath(path);
        }
    }
}
