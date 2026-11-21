using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using MixIt.Core.Production;
using MixIt.Desktop.ViewModels;
using MixIt.Interop;

namespace MixIt.Desktop.Services;

/// <summary>
/// Full-screen output on another display (M8, EXPERIMENTAL): program, preview or
/// multiview for an LED wall, projector or confidence monitor. Frames are copied
/// from the engine at the display's pace through a WPF bitmap — not a
/// frame-locked GPU swap chain yet, so occasional repeated frames are possible.
/// </summary>
public sealed class ExternalOutputWindow : Window
{
    private readonly ProductionService _production;
    private readonly BusMonitor _monitor;
    private readonly DispatcherTimer _timer;
    private readonly Image _image = new() { Stretch = Stretch.Uniform };

    public ExternalOutputWindow(ProductionService production, VideoBus bus, DisplayInfo display)
    {
        ArgumentNullException.ThrowIfNull(display);
        _production = production;
        _monitor = new BusMonitor(bus);
        Display = display;
        Title = $"MIX IT — {bus} on display {display.Index}";
        WindowStyle = WindowStyle.None;
        ResizeMode = ResizeMode.NoResize;
        ShowInTaskbar = false;
        Background = Brushes.Black;
        Topmost = false;
        Content = _image;
        WindowStartupLocation = WindowStartupLocation.Manual;
        _image.SetBinding(Image.SourceProperty, new System.Windows.Data.Binding(nameof(BusMonitor.Bitmap)) { Source = _monitor });
        RenderOptions.SetBitmapScalingMode(_image, BitmapScalingMode.Linear);
        _timer = new DispatcherTimer(DispatcherPriority.Render) { Interval = TimeSpan.FromMilliseconds(15) };
        _timer.Tick += (_, _) =>
        {
            if (_production.IsRunning) _monitor.Refresh(_production, (int)_production.Format.Width, (int)_production.Format.Height);
            else _monitor.Clear();
        };
        SourceInitialized += (_, _) => PlaceOnDisplay();
        Loaded += (_, _) => _timer.Start();
        Closed += (_, _) => _timer.Stop();
    }

    public DisplayInfo Display { get; }

    public VideoBus Bus => _monitor.Bus;

    public long FramesShown => _monitor.FramesShown;

    private void PlaceOnDisplay()
    {
        // Physical pixels → this window's device-independent units, then maximize on that display.
        Matrix toDip = PresentationSource.FromVisual(this)?.CompositionTarget?.TransformFromDevice ?? Matrix.Identity;
        Point topLeft = toDip.Transform(new Point(Display.Left, Display.Top));
        Point size = toDip.Transform(new Point(Display.Width, Display.Height));
        Left = topLeft.X;
        Top = topLeft.Y;
        Width = size.X;
        Height = size.Y;
        WindowState = WindowState.Maximized;
        _ = new WindowInteropHelper(this).Handle;
    }
}
