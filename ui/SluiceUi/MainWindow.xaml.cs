// MainWindow.xaml.cs : 最小の設定ウィンドウ(実装ガイド §5.6)。
//
// エンジン(engine/ipc/pipe_server.h)へ名前付きパイプで接続し、get_devices
// で初回のデバイス一覧を取得したのち、devices_changed push 通知を購読して
// 自動更新する(実装ガイド §5.6「UI に必ず出すもの: 各デバイスのレーン、
// 実効レイテンシ、xrun カウンタ、ASRC の現在比」)。
//
// gap 10: set_param 等のミキサー操作(ゲイン/ミュート/ソロ/ルーティング)は
// Mixer タブから EngineClient.SetStripParam/SetBusParam(既存の IPC 配線)へ
// 直接配線している。トポロジは接続直後に 1 回 get_topology で取得するのみ
// (devices_changed のようなトポロジ変更 push 通知は無いため。ストリップ/
// バスの数が変わるのは AddStrip/RemoveStrip のときだけで、この UI はまだ
// それらを呼ばないので取り直しの必要が無い)。

using System;
using System.Collections.ObjectModel;
using System.Windows;
using SluiceUi.Core;
using SluiceUi.Core.Models;

namespace SluiceUi;

public partial class MainWindow : Window
{
    private EngineClient? _client;
    private readonly ObservableCollection<DeviceInfo> _devices = new();
    private readonly ObservableCollection<StripViewModel> _strips = new();
    private readonly ObservableCollection<BusViewModel> _buses = new();

    public MainWindow()
    {
        InitializeComponent();
        DevicesGrid.ItemsSource = _devices;
        StripsGrid.ItemsSource = _strips;
        BusesGrid.ItemsSource = _buses;
    }

    private void ConnectButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            _client?.Dispose();
            _client = new EngineClient();
            _client.DevicesChanged += OnDevicesChanged;

            var echoResult = _client.Call("echo", new System.Text.Json.Nodes.JsonObject
            {
                ["value"] = "hello from SluiceUi",
            });
            StatusText.Text = "Engine: connected";
            LogBox.AppendText($"echo -> {echoResult}\n");

            // 初回取得。以降は devices_changed push 通知で自動更新される。
            ReplaceDevices(_client.GetDevices());

            // gap 10: ミキサー(ストリップ/バス)構成の初回取得。
            TopologyInfo topology = _client.GetTopology();
            ReplaceTopology(topology);
        }
        catch (Exception ex)
        {
            StatusText.Text = "Engine: disconnected";
            LogBox.AppendText($"connect failed: {ex.Message}\n");
        }
    }

    // EngineClient.DevicesChanged はバックグラウンドの読み取りスレッドから
    // 発火するため、Dispatcher 経由で UI スレッドへマーシャリングする。
    private void OnDevicesChanged(System.Collections.Generic.List<DeviceInfo> devices)
    {
        Dispatcher.Invoke(() => ReplaceDevices(devices));
    }

    private void ReplaceDevices(System.Collections.Generic.List<DeviceInfo> devices)
    {
        _devices.Clear();
        foreach (var d in devices) _devices.Add(d);
    }

    private void ReplaceTopology(TopologyInfo topology)
    {
        _strips.Clear();
        _buses.Clear();
        if (_client is null) return;

        var buses = new List<BusViewModel>();
        foreach (var b in topology.Buses) buses.Add(new BusViewModel(_client, b));
        foreach (var b in buses) _buses.Add(b);

        foreach (var s in topology.Strips) _strips.Add(new StripViewModel(_client, s, buses));

        RoutingGrid.ItemsSource = null;
    }

    private void StripsGrid_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        RoutingGrid.ItemsSource = StripsGrid.SelectedItem is StripViewModel strip ? strip.Routing : null;
    }

    protected override void OnClosed(EventArgs e)
    {
        if (_client is not null) _client.DevicesChanged -= OnDevicesChanged;
        _client?.Dispose();
        base.OnClosed(e);
    }
}
