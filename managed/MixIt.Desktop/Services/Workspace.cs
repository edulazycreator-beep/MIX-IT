using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using MixIt.Core.Production;

namespace MixIt.Desktop.Services;

/// <summary>The operator's workspace (M8): window, last section, shortcuts, recent shows.</summary>
public sealed record WorkspaceSettings
{
    public string? LastSection { get; init; }
    public double? Left { get; init; }
    public double? Top { get; init; }
    public double? Width { get; init; }
    public double? Height { get; init; }
    public bool Maximized { get; init; }
    public Dictionary<string, string> Shortcuts { get; init; } = DefaultShortcuts();
    public List<string> RecentShows { get; init; } = [];

    /// <summary>Gesture (e.g. "Space", "Ctrl+D1", "F1") → command line (macro syntax).</summary>
    public static Dictionary<string, string> DefaultShortcuts()
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["Space"] = "Cut",
            ["Return"] = "Auto",
            ["Escape"] = "StopMacros",
            ["Ctrl+R"] = "StartRecordings",
            ["Ctrl+Shift+R"] = "StopRecordings",
        };
        for (int i = 1; i <= 9; i++)
        {
            map[$"D{i}"] = $"SetPreview input={i}";
            map[$"Ctrl+D{i}"] = $"SetProgram input={i}";
        }
        for (int c = 1; c <= 4; c++) map[$"F{c}"] = $"OverlayToggle channel={c}";
        return map;
    }
}

/// <summary>Loads/saves <see cref="WorkspaceSettings"/> as JSON (a broken file is ignored, never fatal).</summary>
public sealed class WorkspaceStore(string path, ILogger<WorkspaceStore> logger)
{
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web) { WriteIndented = true };

    public static string DefaultPath() =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MIX IT", "workspace.json");

    public WorkspaceSettings Load()
    {
        try
        {
            if (!File.Exists(path)) return new WorkspaceSettings();
            WorkspaceSettings? s = JsonSerializer.Deserialize<WorkspaceSettings>(File.ReadAllText(path), Options);
            if (s is null) return new WorkspaceSettings();
            return s with { Shortcuts = new Dictionary<string, string>(s.Shortcuts, StringComparer.OrdinalIgnoreCase) };
        }
        catch (Exception ex) when (ex is JsonException or IOException or UnauthorizedAccessException)
        {
            logger.LogWarning("Workspace settings ignored: {Message}", ex.Message);
            return new WorkspaceSettings();
        }
    }

    public void Save(WorkspaceSettings settings)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(settings, Options));
            File.Move(path + ".tmp", path, overwrite: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            logger.LogWarning("Workspace settings not saved: {Message}", ex.Message);
        }
    }
}

/// <summary>Stream keys in show files, protected with Windows DPAPI for the current user.</summary>
public sealed class DpapiSecretProtector : ISecretProtector
{
    private static readonly byte[] Entropy = Encoding.UTF8.GetBytes("MIX IT show file v1");

    public string Protect(string secret) =>
        Convert.ToBase64String(ProtectedData.Protect(Encoding.UTF8.GetBytes(secret), Entropy, DataProtectionScope.CurrentUser));

    public string Unprotect(string protectedSecret)
    {
        try
        {
            return Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(protectedSecret), Entropy, DataProtectionScope.CurrentUser));
        }
        catch (Exception ex) when (ex is CryptographicException or FormatException)
        {
            throw new InvalidOperationException("The stream key was saved by another Windows user or PC.", ex);
        }
    }
}

/// <summary>A connected display (physical pixels).</summary>
public sealed record DisplayInfo(int Index, string Name, int Left, int Top, int Width, int Height, bool IsPrimary)
{
    public override string ToString() => $"{Index}: {Width}×{Height}{(IsPrimary ? " (main display)" : "")}";
}

/// <summary>Enumerates displays (Win32 EnumDisplayMonitors; WPF has no screen list).</summary>
public static partial class Displays
{
    public static IReadOnlyList<DisplayInfo> List()
    {
        var result = new List<DisplayInfo>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (monitor, _, _, _) =>
        {
            var info = new MonitorInfoEx { Size = Marshal.SizeOf<MonitorInfoEx>() };
            if (GetMonitorInfo(monitor, ref info))
            {
                result.Add(new DisplayInfo(result.Count + 1, info.Device, info.Monitor.Left, info.Monitor.Top,
                    info.Monitor.Right - info.Monitor.Left, info.Monitor.Bottom - info.Monitor.Top, (info.Flags & 1) != 0));
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    private delegate bool MonitorEnumProc(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data);

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left, Top, Right, Bottom;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MonitorInfoEx
    {
        public int Size;
        public Rect Monitor;
        public Rect Work;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string Device;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc proc, IntPtr data);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetMonitorInfoW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfoEx info);
}
