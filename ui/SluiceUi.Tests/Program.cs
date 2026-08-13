// Program.cs : StripViewModel/BusViewModel/RoutingCell の回帰テスト。
//
// SluiceUi.Core.Tests/Program.cs と同じ手法(NamedPipeServerStream で
// engine/ipc/pipe_server.h と同じ改行区切り JSON プロトコルを話す簡易
// モックサーバを立てる)を使う。EngineClient のコンストラクタはパイプへ
// 即座に接続する(SluiceUi.Core/EngineClient.cs 参照)ため、
// StripViewModel/BusViewModel を構築するだけでも生きているモックサーバが
// 要る。
//
// 追加の NuGet テストフレームワーク(xunit 等)を増やさない方針。
// C++ 側のテスト・SluiceUi.Core.Tests と同じ、exit code で成否を返す流儀。

using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;

using SluiceUi;
using SluiceUi.Core;
using SluiceUi.Core.Models;

static void Fail(string what)
{
    Console.Error.WriteLine($"FAIL: {what}");
    Environment.Exit(1);
}

// TestSetStripParamRequestShape (SluiceUi.Core.Tests) の RunMockServer と
// 違い、こちらは接続後に複数リクエストを処理し続ける(1 テストの中で
// ViewModel の複数プロパティを set することがあるため)。クライアントが
// Dispose されてパイプが閉じると ReadLine() が null を返してループを抜ける。
static void RunMockServerLoop(string pipeName, ManualResetEventSlim ready, List<JsonObject> captured,
                              Func<JsonObject, JsonObject> respond)
{
    using var server = new NamedPipeServerStream(pipeName, PipeDirection.InOut, 1);
    ready.Set();
    server.WaitForConnection();

    using var reader = new StreamReader(server, new UTF8Encoding(false), leaveOpen: true);
    using var writer = new StreamWriter(server, new UTF8Encoding(false), leaveOpen: true)
    {
        AutoFlush = true,
        NewLine = "\n",
    };

    // クライアント(EngineClient)が Dispose されてパイプが閉じられると、
    // 実装によっては ReadLine() が null を返す代わりに IOException を投げる
    // (OS/タイミング依存)。テスト対象のアサーションは既に済んでいる後始末
    // の段階なので、ここは単にループを終える扱いにする。
    try
    {
        string? line;
        while ((line = reader.ReadLine()) != null)
        {
            JsonObject request = JsonNode.Parse(line)!.AsObject();
            lock (captured) captured.Add(request);
            JsonObject response = respond(request);
            writer.WriteLine(response.ToJsonString());
        }
    }
    catch (IOException)
    {
        // クライアント切断によるパイプ破棄。正常終了として扱う。
    }
}

// 全リクエストに空の result で応答するだけの、set_param 用モックサーバを
// 起動する共通ヘルパー。
static (EngineClient client, Task serverTask, List<JsonObject> captured) StartClientWithMockServer(
    string pipeNameSuffix)
{
    string pipeName = "sluice-ui-vm-test-" + pipeNameSuffix + "-" + Environment.ProcessId;
    using var ready = new ManualResetEventSlim(false);
    var captured = new List<JsonObject>();
    Task serverTask = Task.Run(() => RunMockServerLoop(pipeName, ready, captured, request =>
        new JsonObject { ["id"] = request["id"]!.DeepClone(), ["result"] = new JsonObject() }));
    ready.Wait();

    var client = new EngineClient(pipeName);
    return (client, serverTask, captured);
}

void TestStripViewModelMapsFromStripInfo()
{
    var (client, serverTask, _) = StartClientWithMockServer("strip-map");

    var busInfo = new BusInfo { Index = 0, DeviceIndex = 2, Channel = 0, GainDb = 0.0 };
    var bus = new BusViewModel(client, busInfo);

    var stripInfo = new StripInfo
    {
        Index = 3,
        DeviceIndex = 1,
        Channel = 0,
        BoundaryIndex = 0,
        GainDb = -6.0,
        Mute = true,
        Solo = false,
        RoutingGain = new List<double> { -2.0 },
    };
    var strip = new StripViewModel(client, stripInfo, new List<BusViewModel> { bus });

    if (strip.Index != 3) Fail($"StripViewModel: unexpected Index {strip.Index}");
    if (strip.Label != "dev[1] ch[0]") Fail($"StripViewModel: unexpected Label '{strip.Label}'");
    if (Math.Abs(strip.GainDb - (-6.0)) > 1e-9) Fail($"StripViewModel: unexpected GainDb {strip.GainDb}");
    if (strip.Mute != true) Fail("StripViewModel: unexpected Mute");
    if (strip.Solo != false) Fail("StripViewModel: unexpected Solo");
    if (strip.Routing.Count != 1) Fail($"StripViewModel: unexpected Routing.Count {strip.Routing.Count}");
    if (Math.Abs(strip.Routing[0].GainDb - (-2.0)) > 1e-9)
        Fail($"StripViewModel: unexpected routing gain {strip.Routing[0].GainDb}");
    if (!strip.Routing[0].IsSending)
        Fail("StripViewModel: routing cell should be sending (gain > muted sentinel)");
    if (strip.Routing[0].BusLabel != bus.Label)
        Fail($"StripViewModel: routing cell bus label mismatch ('{strip.Routing[0].BusLabel}' vs '{bus.Label}')");

    client.Dispose();
    serverTask.Wait();
    Console.WriteLine("PASS: StripViewModel maps fields from StripInfo/BusViewModel");
}

void TestStripViewModelRoutingDefaultsToMuted()
{
    var (client, serverTask, _) = StartClientWithMockServer("strip-routing-default");

    var bus = new BusViewModel(client, new BusInfo { Index = 0, DeviceIndex = 2, Channel = 0 });
    // RoutingGain が buses の数より短い場合、足りない分は RoutingCell.MutedSentinel
    // で埋まる(StripViewModel.cs のコンストラクタ参照)。
    var stripInfo = new StripInfo { Index = 0, RoutingGain = new List<double>() };
    var strip = new StripViewModel(client, stripInfo, new List<BusViewModel> { bus });

    if (Math.Abs(strip.Routing[0].GainDb - RoutingCell.MutedSentinel) > 1e-9)
        Fail($"StripViewModel: expected routing default to MutedSentinel, got {strip.Routing[0].GainDb}");
    if (strip.Routing[0].IsSending)
        Fail("StripViewModel: routing cell at MutedSentinel should not be sending");

    client.Dispose();
    serverTask.Wait();
    Console.WriteLine("PASS: StripViewModel routing defaults to muted when RoutingGain is short");
}

void TestStripViewModelSettersSendSetParam()
{
    var (client, serverTask, captured) = StartClientWithMockServer("strip-setters");

    var strip = new StripViewModel(client, new StripInfo { Index = 5 }, new List<BusViewModel>());

    strip.GainDb = -9.5;
    strip.Mute = true;
    strip.Solo = true;

    client.Dispose();
    serverTask.Wait();

    if (captured.Count != 3) Fail($"StripViewModel setters: expected 3 set_param requests, got {captured.Count}");

    JsonObject gainParams = captured[0]["params"]!.AsObject();
    if (gainParams["target"]?.GetValue<string>() != "strip")
        Fail($"StripViewModel setters: unexpected target '{gainParams["target"]}'");
    if (gainParams["index"]?.GetValue<int>() != 5)
        Fail($"StripViewModel setters: unexpected index {gainParams["index"]}");
    if (Math.Abs(gainParams["gainDb"]!.GetValue<double>() - (-9.5)) > 1e-9)
        Fail($"StripViewModel setters: unexpected gainDb {gainParams["gainDb"]}");

    JsonObject muteParams = captured[1]["params"]!.AsObject();
    if (muteParams["mute"]?.GetValue<bool>() != true)
        Fail($"StripViewModel setters: unexpected mute {muteParams["mute"]}");

    JsonObject soloParams = captured[2]["params"]!.AsObject();
    if (soloParams["solo"]?.GetValue<bool>() != true)
        Fail($"StripViewModel setters: unexpected solo {soloParams["solo"]}");

    Console.WriteLine("PASS: StripViewModel property setters send set_param(strip)");
}

void TestRoutingCellPushesAllRoutingGains()
{
    var (client, serverTask, captured) = StartClientWithMockServer("routing-cell-push");

    var bus0 = new BusViewModel(client, new BusInfo { Index = 0 });
    var bus1 = new BusViewModel(client, new BusInfo { Index = 1 });
    var stripInfo = new StripInfo { Index = 7, RoutingGain = new List<double> { -1000.0, -1000.0 } };
    var strip = new StripViewModel(client, stripInfo, new List<BusViewModel> { bus0, bus1 });

    // 1 つの RoutingCell だけを変更しても、engine 側 set_param の routingGain は
    // 「渡された先頭 N 個を上書きする」実装なので、全バスぶんまとめて送る
    // (StripViewModel.PushRouting のコメント参照)。
    strip.Routing[1].GainDb = -3.0;

    client.Dispose();
    serverTask.Wait();

    if (captured.Count != 1)
        Fail($"RoutingCell push: expected exactly 1 set_param request, got {captured.Count}");
    JsonObject routingParams = captured[0]["params"]!.AsObject();
    if (routingParams["index"]?.GetValue<int>() != 7)
        Fail($"RoutingCell push: unexpected strip index {routingParams["index"]}");
    if (routingParams["routingGain"] is not JsonArray arr || arr.Count != 2)
        Fail("RoutingCell push: expected routingGain array of length 2");
    else
    {
        if (Math.Abs(arr[0]!.GetValue<double>() - (-1000.0)) > 1e-9)
            Fail($"RoutingCell push: unexpected routingGain[0] {arr[0]}");
        if (Math.Abs(arr[1]!.GetValue<double>() - (-3.0)) > 1e-9)
            Fail($"RoutingCell push: unexpected routingGain[1] {arr[1]}");
    }

    Console.WriteLine("PASS: RoutingCell setter pushes full routingGain array");
}

void TestBusViewModelMapsAndSetsGain()
{
    var (client, serverTask, captured) = StartClientWithMockServer("bus-map-set");

    var busInfo = new BusInfo { Index = 4, DeviceIndex = 2, Channel = 1, GainDb = -1.5 };
    var bus = new BusViewModel(client, busInfo);

    if (bus.Index != 4) Fail($"BusViewModel: unexpected Index {bus.Index}");
    if (bus.Label != "dev[2] ch[1]") Fail($"BusViewModel: unexpected Label '{bus.Label}'");
    if (Math.Abs(bus.GainDb - (-1.5)) > 1e-9) Fail($"BusViewModel: unexpected GainDb {bus.GainDb}");

    bus.GainDb = 2.0;

    client.Dispose();
    serverTask.Wait();

    if (captured.Count != 1) Fail($"BusViewModel: expected 1 set_param request, got {captured.Count}");
    JsonObject busParams = captured[0]["params"]!.AsObject();
    if (busParams["target"]?.GetValue<string>() != "bus")
        Fail($"BusViewModel: unexpected target '{busParams["target"]}'");
    if (busParams["index"]?.GetValue<int>() != 4)
        Fail($"BusViewModel: unexpected index {busParams["index"]}");
    if (Math.Abs(busParams["gainDb"]!.GetValue<double>() - 2.0) > 1e-9)
        Fail($"BusViewModel: unexpected gainDb {busParams["gainDb"]}");

    Console.WriteLine("PASS: BusViewModel maps fields and setter sends set_param(bus)");
}

void TestPropertyChangedFiresOnlyOnActualChange()
{
    var (client, serverTask, captured) = StartClientWithMockServer("property-changed");

    var strip = new StripViewModel(client, new StripInfo { Index = 0, GainDb = -3.0 },
                                   new List<BusViewModel>());

    var changedProperties = new List<string?>();
    strip.PropertyChanged += (_, e) => changedProperties.Add(e.PropertyName);

    strip.GainDb = -3.0;  // 値が変わらないので PropertyChanged は発火しないはず
    if (changedProperties.Count != 0)
        Fail($"PropertyChanged: unexpected notification for no-op set ({changedProperties.Count})");

    strip.GainDb = 1.0;  // 実際に変わるので発火するはず
    if (changedProperties.Count != 1 || changedProperties[0] != nameof(StripViewModel.GainDb))
        Fail("PropertyChanged: expected exactly one GainDb notification for actual change");

    client.Dispose();
    serverTask.Wait();
    // 値が変わらない set_param は EngineClient まで届かないはず(Set<T> の
    // Equals チェックで早期リターンするため、captured には1件だけ入る)。
    if (captured.Count != 1)
        Fail($"PropertyChanged: expected exactly 1 set_param sent (no-op should be skipped), got {captured.Count}");

    Console.WriteLine("PASS: PropertyChanged fires only on actual value change");
}

TestStripViewModelMapsFromStripInfo();
TestStripViewModelRoutingDefaultsToMuted();
TestStripViewModelSettersSendSetParam();
TestRoutingCellPushesAllRoutingGains();
TestBusViewModelMapsAndSetsGain();
TestPropertyChangedFiresOnlyOnActualChange();
Console.WriteLine("ALL PASS: SluiceUi (StripViewModel/BusViewModel/RoutingCell)");
