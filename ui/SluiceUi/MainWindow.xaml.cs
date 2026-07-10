// MainWindow.xaml.cs : 最小の設定ウィンドウ(実装ガイド §5.7)。
//
// エンジン(engine/ipc/pipe_server.h)へ名前付きパイプで接続できることを
// 確認するだけの最小実装。set_param 等の実配線は、engine 側で
// EngineGraph が実際に稼働するアプリへ組み込まれてから(今回のスコープ外。
// engine/ipc/pipe_server.h のコミットメッセージ参照)。

using System;
using System.Windows;
using SluiceUi.Core;

namespace SluiceUi;

public partial class MainWindow : Window
{
    private EngineClient? _client;

    public MainWindow()
    {
        InitializeComponent();
    }

    private void ConnectButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            _client?.Dispose();
            _client = new EngineClient();
            var result = _client.Call("echo", new System.Text.Json.Nodes.JsonObject
            {
                ["value"] = "hello from SluiceUi",
            });
            StatusText.Text = "Engine: connected";
            LogBox.AppendText($"echo -> {result}\n");
        }
        catch (Exception ex)
        {
            StatusText.Text = "Engine: disconnected";
            LogBox.AppendText($"connect failed: {ex.Message}\n");
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        _client?.Dispose();
        base.OnClosed(e);
    }
}
