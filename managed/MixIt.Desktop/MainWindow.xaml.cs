using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using MixIt.Desktop.ViewModels;

namespace MixIt.Desktop;

/// <summary>
/// Main shell window. Code-behind only wires the view model (MVVM), turns key
/// presses into shortcut gestures and keeps the window placement.
/// </summary>
public partial class MainWindow : Window
{
    private readonly MainViewModel _viewModel;

    public MainWindow(MainViewModel viewModel)
    {
        InitializeComponent();
        _viewModel = viewModel;
        DataContext = viewModel;
        RestorePlacement();
        PreviewKeyDown += OnPreviewKeyDown;
        Closing += (_, _) => _viewModel.Settings.SaveWorkspace(w => w with
        {
            Left = RestoreBounds.Left,
            Top = RestoreBounds.Top,
            Width = RestoreBounds.Width,
            Height = RestoreBounds.Height,
            Maximized = WindowState == WindowState.Maximized,
        });
    }

    private void RestorePlacement()
    {
        var w = _viewModel.Settings.Workspace;
        if (w.Width is not double width || w.Height is not double height || w.Left is not double left || w.Top is not double top) return;
        // Only if the saved rectangle is still on a screen (a display may have been unplugged).
        if (left + 100 > SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth || top < SystemParameters.VirtualScreenTop
            || left + width < SystemParameters.VirtualScreenLeft + 100 || top + 50 > SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight) return;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = left;
        Top = top;
        Width = Math.Max(MinWidth, width);
        Height = Math.Max(MinHeight, height);
        if (w.Maximized) WindowState = WindowState.Maximized;
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        // Typing in a field never triggers production commands.
        if (e.OriginalSource is TextBoxBase or PasswordBox or ComboBox { IsEditable: true }) return;
        Key key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftShift or Key.RightShift or Key.LeftAlt or Key.RightAlt or Key.LWin or Key.RWin) return;
        ModifierKeys m = Keyboard.Modifiers;
        string gesture = (m.HasFlag(ModifierKeys.Control) ? "Ctrl+" : "") + (m.HasFlag(ModifierKeys.Shift) ? "Shift+" : "")
            + (m.HasFlag(ModifierKeys.Alt) ? "Alt+" : "") + key;
        if (_viewModel.Settings.TryRunShortcut(gesture)) e.Handled = true;
    }
}
