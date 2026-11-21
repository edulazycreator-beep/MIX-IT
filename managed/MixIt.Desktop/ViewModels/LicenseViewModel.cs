using System.IO;
using System.Windows;
using System.Windows.Input;
using Microsoft.Extensions.Logging;
using Microsoft.Win32;
using MixIt.Licensing;

namespace MixIt.Desktop.ViewModels;

/// <summary>
/// License Management (M9): edition, licence details, install from a file,
/// remove, this PC's id for machine-bound licences, and what each edition allows.
/// </summary>
public sealed class LicenseViewModel : ObservableObject
{
    private readonly LicenseService _license;
    private readonly ILogger<LicenseViewModel> _logger;
    private string? _message;

    public LicenseViewModel(LicenseService license, ILogger<LicenseViewModel> logger)
    {
        _license = license;
        _logger = logger;
        InstallCommand = new RelayCommand(_ => Install());
        RemoveCommand = new RelayCommand(_ => Remove(), _ => _license.Status.License is not null);
        CopyMachineIdCommand = new RelayCommand(_ => Clipboard.SetText(_license.MachineId));
        _license.Changed += (_, _) => Refresh();
    }

    public ICommand InstallCommand { get; }
    public ICommand RemoveCommand { get; }
    public ICommand CopyMachineIdCommand { get; }

    public string Edition => _license.Edition == Licensing.Edition.Professional ? "MIX IT Professional" : "MIX IT Free";

    public string Details => _license.Status switch
    {
        { Valid: true, License: { } l } => $"Licensed to {l.Licensee}\nLicence {l.LicenseId}, issued {l.Issued:yyyy-MM-dd}, " +
            (l.Expires is { } e ? $"valid until {e:yyyy-MM-dd}" : "perpetual") + (l.MachineId is null ? "" : ", for this PC"),
        { Problem: { } p } => $"The installed licence is not valid: {p}",
        _ => "No licence installed.",
    };

    public string MachineId => _license.MachineId;

    public string BuildNote => _license.CanVerify ? "" : "This build has no publisher key configured, so it cannot verify Professional licences (development build).";

    public string Limits
    {
        get
        {
            EditionPolicy f = EditionPolicy.Free;
            return $"Free: up to {f.MaxVideoInputs} video inputs, {f.MaxStreams} stream destination, unlimited recordings, graphics, keys, macros, tally, PTZ.\n" +
                   "Professional: inputs and outputs limited only by the PC, ISO recording, instant replay, multiview output, remote control API, extensions.\n" +
                   "Limits apply when the production is set up — a running show is never stopped or changed by licensing.";
        }
    }

    public string? Message
    {
        get => _message;
        private set => SetProperty(ref _message, value);
    }

    private void Install()
    {
        var dialog = new OpenFileDialog { Filter = "MIX IT licence (*.mixit-license)|*.mixit-license|All files (*.*)|*.*", Title = "Install licence" };
        if (dialog.ShowDialog() != true) return;
        try
        {
            LicenseCheck check = _license.Install(File.ReadAllText(dialog.FileName));
            Message = check.Valid ? $"Installed: {check.License!.Edition} for {check.License.Licensee}." : $"Not installed: {check.Problem}";
            _logger.LogInformation("Licence install: {Result}", Message);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Message = $"Not installed: {ex.Message} (installing for all users may need administrator rights).";
        }
        Refresh();
    }

    private void Remove()
    {
        if (MessageBox.Show("Remove the licence from this PC? MIX IT continues as the Free edition.", "MIX IT",
                MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK) return;
        try
        {
            _license.Remove();
            Message = "Licence removed.";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Message = ex.Message;
        }
        Refresh();
    }

    private void Refresh()
    {
        OnPropertyChanged(nameof(Edition));
        OnPropertyChanged(nameof(Details));
    }
}
