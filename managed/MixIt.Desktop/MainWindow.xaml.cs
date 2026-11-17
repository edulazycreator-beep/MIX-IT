using System.Windows;
using MixIt.Desktop.ViewModels;

namespace MixIt.Desktop;

/// <summary>Main shell window. Code-behind only wires the view model (MVVM).</summary>
public partial class MainWindow : Window
{
    public MainWindow(MainViewModel viewModel)
    {
        InitializeComponent();
        DataContext = viewModel;
    }
}
