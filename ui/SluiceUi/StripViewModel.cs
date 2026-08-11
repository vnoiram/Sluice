// StripViewModel.cs : Mixer タブのストリップ行(実装ガイド §5.6 の続き、
// gap 10「UI のミキサー操作」)。
//
// SluiceUi.Core は読み取り専用の DTO(StripInfo, init のみ)しか持たないため、
// 双方向編集が要る UI 側だけにこの薄い ViewModel を置く。楽観的更新
// (setter で表示状態を更新すると同時に EngineClient.SetStripParam を呼ぶ)
// で、往復フェッチはしない。トポロジの形(ストリップ/バスの数)は
// パラメータ編集だけでは変わらない(AddStrip/RemoveStrip 経由のみ)ため、
// これで整合性は保てる。

using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

using SluiceUi.Core;
using SluiceUi.Core.Models;

namespace SluiceUi;

// 1 ストリップ × 1 バスぶんのルーティングセンドゲイン(dB)。
// StripParams::kRoutingGainMuted(engine/graph/strip.h)と同じ値を「送らない」
// の意味で使う。
public sealed class RoutingCell : INotifyPropertyChanged
{
    public const double MutedSentinel = -1000.0;

    private readonly StripViewModel _owner;
    public int BusIndex { get; }
    public string BusLabel { get; }

    private double _gainDb;
    public double GainDb
    {
        get => _gainDb;
        set
        {
            if (Set(ref _gainDb, value)) _owner.PushRouting();
        }
    }

    public bool IsSending => GainDb > MutedSentinel;

    internal RoutingCell(StripViewModel owner, int busIndex, string busLabel, double gainDb)
    {
        _owner = owner;
        BusIndex = busIndex;
        BusLabel = busLabel;
        _gainDb = gainDb;
    }

    private bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsSending)));
        return true;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}

public sealed class StripViewModel : INotifyPropertyChanged
{
    private readonly EngineClient _client;
    public int Index { get; }
    public string Label { get; }

    private double _gainDb;
    public double GainDb
    {
        get => _gainDb;
        set { if (Set(ref _gainDb, value)) _client.SetStripParam(Index, gainDb: value); }
    }

    private bool _mute;
    public bool Mute
    {
        get => _mute;
        set { if (Set(ref _mute, value)) _client.SetStripParam(Index, mute: value); }
    }

    private bool _solo;
    public bool Solo
    {
        get => _solo;
        set { if (Set(ref _solo, value)) _client.SetStripParam(Index, solo: value); }
    }

    public ObservableCollection<RoutingCell> Routing { get; } = new();

    public StripViewModel(EngineClient client, StripInfo info, IReadOnlyList<BusViewModel> buses)
    {
        _client = client;
        Index = info.Index;
        Label = $"dev[{info.DeviceIndex}] ch[{info.Channel}]";
        _gainDb = info.GainDb;
        _mute = info.Mute;
        _solo = info.Solo;

        for (int bi = 0; bi < buses.Count; ++bi)
        {
            double g = bi < info.RoutingGain.Count ? info.RoutingGain[bi] : RoutingCell.MutedSentinel;
            Routing.Add(new RoutingCell(this, bi, buses[bi].Label, g));
        }
    }

    // いずれか 1 つの RoutingCell が変更されたら、現在の全バス分をまとめて
    // 送る(engine 側 set_param の routingGain は「渡された先頭 N 個を
    // 上書きする」実装なので、常に全要素を渡すのが安全 —
    // engine/main.cpp の set_param ハンドラ参照)。
    internal void PushRouting()
    {
        var values = new List<double>(Routing.Count);
        foreach (var cell in Routing) values.Add(cell.GainDb);
        _client.SetStripParam(Index, routingGain: values);
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
