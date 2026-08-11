// App.xaml.cs : エントリポイント。実装ガイド §5.6 の
// 「まず最小の運用UIとして、システムトレイ+設定ウィンドウでも成立する」
// に沿った最小実装。起動時はウィンドウを出さずトレイ常駐のみ行う。

using System.Windows;
using Application = System.Windows.Application;

namespace SluiceUi;

public partial class App : Application
{
    private TrayIconManager? _tray;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        _tray = new TrayIconManager(this);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _tray?.Dispose();
        base.OnExit(e);
    }
}
