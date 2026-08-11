// MainWindow.xaml.cs : 最小の設定ウィンドウ(実装ガイド §5.6)。
//
// エンジン(engine/ipc/pipe_server.h)へ名前付きパイプで接続し、get_devices
// で初回のデバイス一覧を取得したのち、devices_changed push 通知を購読して
// 自動更新する(実装ガイド §5.6「UI に必ず出すもの: 各デバイスのレーン、
// 実効レイテンシ、xrun カウンタ、ASRC の現在比」)。set_param 等のミキサー
// 操作の実配線は、engine 側で EngineGraph が実際に稼働するアプリへ
// 組み込まれてから(今回のスコープ外)。

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

    public MainWindow()
    {
        InitializeComponent();
        DevicesGrid.ItemsSource = _devices;
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

    protected override void OnClosed(EventArgs e)
    {
        if (_client is not null) _client.DevicesChanged -= OnDevicesChanged;
        _client?.Dispose();
        base.OnClosed(e);
    }
}
