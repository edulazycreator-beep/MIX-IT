using System.Windows;
using System.Windows.Controls;
using MixIt.Desktop.ViewModels;

namespace MixIt.Desktop.Views;

/// <summary>Streaming &amp; Recording view (bindings; the stream key box forwards its value).</summary>
public partial class OutputsView : UserControl
{
    public OutputsView() => InitializeComponent();

    // PasswordBox.Password is not bindable by design; hand the key to the view
    // model, which never exposes it again.
    private void OnStreamKeyChanged(object sender, RoutedEventArgs e)
    {
        if (DataContext is OutputsViewModel vm) vm.StreamKey = StreamKeyBox.Password;
    }
}
