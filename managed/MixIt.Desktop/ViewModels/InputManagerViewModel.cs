using System.Collections.ObjectModel;
using System.Windows.Input;
using Microsoft.Extensions.Logging;
using MixIt.Core.Control;
using MixIt.Core.Production;
using MixIt.Desktop.Services;
using MixIt.Interop;
using MixIt.Protocol;

namespace MixIt.Desktop.ViewModels;

/// <summary>One input row of the Input Manager.</summary>
public sealed class InputRowViewModel : ObservableObject
{
    private readonly InputManagerViewModel _owner;
    private string _name;
    private bool _program;
    private bool _preview;

    public InputRowViewModel(int number, ProductionInput input, InputManagerViewModel owner)
    {
        _owner = owner;
        Number = number;
        Id = input.Id;
        _name = input.Name;
        Kind = input.Kind.ToString();
        Detail = input.Detail;
        RenameCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.RenameInput(Id, _name)), _ => _name.Trim().Length > 0);
        RemoveCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.RemoveInput(Id)), _ => _owner.Production.CanEditInputs);
        PreviewCommand = new RelayCommand(_ => _owner.Run("SetPreview", Id), _ => _owner.Production.IsRunning);
        ProgramCommand = new RelayCommand(_ => _owner.Run("SetProgram", Id), _ => _owner.Production.IsRunning);
    }

    public int Number { get; }
    public uint Id { get; }
    public string Kind { get; }
    public string Detail { get; }
    public ICommand RenameCommand { get; }
    public ICommand RemoveCommand { get; }
    public ICommand PreviewCommand { get; }
    public ICommand ProgramCommand { get; }

    public string Name
    {
        get => _name;
        set => SetProperty(ref _name, value ?? "");
    }

    public bool OnProgram
    {
        get => _program;
        set => SetProperty(ref _program, value);
    }

    public bool OnPreview
    {
        get => _preview;
        set => SetProperty(ref _preview, value);
    }
}

/// <summary>
/// Input Manager (M8): every input with its number (used by shortcuts, macros
/// and the remote API), kind, source, live tally; rename (also the multiview
/// label) and remove. Adding inputs stays in Production Studio.
/// </summary>
public sealed class InputManagerViewModel : ObservableObject
{
    private readonly ControlHost _control;
    private readonly ILogger<InputManagerViewModel> _logger;
    private string? _error;

    public InputManagerViewModel(ProductionService production, ControlHost control, ILogger<InputManagerViewModel> logger)
    {
        Production = production;
        _control = control;
        _logger = logger;
        production.Changed += (_, _) => Sync();
        control.Tally.Changed += (_, tally) => ApplyTally(tally);
        Sync();
    }

    internal ProductionService Production { get; }

    public ObservableCollection<InputRowViewModel> Rows { get; } = [];

    public string? Error
    {
        get => _error;
        private set => SetProperty(ref _error, value);
    }

    internal void Guard(Action action)
    {
        Error = null;
        try
        {
            action();
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            _logger.LogWarning("Input operation failed: {Message}", ex.Message);
            Error = ex.Message;
        }
    }

    internal async void Run(string command, uint inputId)
    {
        CommandResult r = await _control.Commands.ExecuteAsync(CommandRequest.Of(command, ("input", $"id:{inputId}")), "ui").ConfigureAwait(true);
        Error = r.Ok ? null : r.Error;
    }

    private void Sync()
    {
        Rows.Clear();
        int n = 1;
        foreach (ProductionInput i in Production.Inputs) Rows.Add(new InputRowViewModel(n++, i, this));
        ApplyTally(_control.Tally.Current);
    }

    private void ApplyTally(IReadOnlyList<TallyEntry> tally)
    {
        foreach (InputRowViewModel row in Rows)
        {
            TallyEntry? t = tally.FirstOrDefault(e => e.InputId == row.Id);
            row.OnProgram = t?.Program ?? false;
            row.OnPreview = t?.Preview ?? false;
        }
    }
}
