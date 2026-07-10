// TrayIconManager.cs : システムトレイ常駐(実装ガイド §5.7)
//
// WPF に標準のトレイアイコン API が無いため、System.Windows.Forms の
// NotifyIcon を使う(WPF アプリでの一般的なやり方。UseWindowsForms を
// SluiceUi.csproj で有効にしている)。

using System.Drawing;
using System.Windows;
using System.Windows.Forms;
using Application = System.Windows.Application;

namespace SluiceUi;

public sealed class TrayIconManager : IDisposable
{
    private readonly Application _app;
    private readonly NotifyIcon _icon;
    private MainWindow? _window;

    public TrayIconManager(Application app)
    {
        _app = app;

        var menu = new ContextMenuStrip();
        menu.Items.Add("Open", null, (_, _) => ShowWindow());
        menu.Items.Add("Exit", null, (_, _) => ExitApp());

        _icon = new NotifyIcon
        {
            // TODO: 専用アイコンに差し替える(実装ガイド §10 の商標表記と
            // あわせてブランディング確定時に対応)。
            Icon = SystemIcons.Application,
            Visible = true,
            Text = "Sluice",
            ContextMenuStrip = menu,
        };
        _icon.DoubleClick += (_, _) => ShowWindow();
    }

    private void ShowWindow()
    {
        if (_window is null)
        {
            _window = new MainWindow();
            _window.Closed += (_, _) => _window = null;
        }
        _window.Show();
        _window.WindowState = WindowState.Normal;
        _window.Activate();
    }

    private void ExitApp()
    {
        _icon.Visible = false;
        _app.Shutdown();
    }

    public void Dispose()
    {
        _icon.Dispose();
    }
}
