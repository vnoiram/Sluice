// BusViewModel.cs : Mixer タブのバス行(StripViewModel.cs と対、gap 10)。

using System.ComponentModel;
using System.Runtime.CompilerServices;

using SluiceUi.Core;
using SluiceUi.Core.Models;

namespace SluiceUi;

public sealed class BusViewModel : INotifyPropertyChanged
{
    private readonly EngineClient _client;
    public int Index { get; }
    public string Label { get; }

    private double _gainDb;
    public double GainDb
    {
        get => _gainDb;
        set { if (Set(ref _gainDb, value)) _client.SetBusParam(Index, gainDb: value); }
    }

    public BusViewModel(EngineClient client, BusInfo info)
    {
        _client = client;
        Index = info.Index;
        Label = $"dev[{info.DeviceIndex}] ch[{info.Channel}]";
        _gainDb = info.GainDb;
    }

    private bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}
