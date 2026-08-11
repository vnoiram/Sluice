// Program.cs : EngineClient の回帰テスト。
//
// 実際の engine(C++)プロセスは使わず、このテスト自身が
// NamedPipeServerStream で engine/ipc/pipe_server.h と同じ改行区切り
// JSON プロトコルを話す簡易モックサーバを立て、EngineClient(C#)の
// リクエスト構築・レスポンス解析・エラー伝播を検証する。
//
// 追加の NuGet テストフレームワーク(xunit 等)を増やさない方針。
// C++ 側のテスト(exit code で成否を返す)と同じ流儀。

using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;
using SluiceUi.Core;
using SluiceUi.Core.Models;

static void Fail(string what)
{
    Console.Error.WriteLine($"FAIL: {what}");
    Environment.Exit(1);
}

static void RunMockServer(string pipeName, ManualResetEventSlim ready, Func<JsonObject, JsonObject> respond)
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

    string? line = reader.ReadLine();
    if (line is null) return;
    JsonObject request = JsonNode.Parse(line)!.AsObject();
    JsonObject response = respond(request);
    writer.WriteLine(response.ToJsonString());
}

void TestEchoRoundTrip()
{
    string pipeName = "sluice-ui-test-echo-" + Environment.ProcessId;
    using var ready = new ManualResetEventSlim(false);
    Task serverTask = Task.Run(() => RunMockServer(pipeName, ready, request =>
    {
        var value = request["params"]?["value"]?.DeepClone();
        return new JsonObject
        {
            ["id"] = request["id"]!.DeepClone(),
            ["result"] = new JsonObject { ["echoed"] = value },
        };
    }));
    ready.Wait();

    using var client = new EngineClient(pipeName);
    JsonNode? result = client.Call("echo", new JsonObject { ["value"] = "hi" });
    string? echoed = result?["echoed"]?.GetValue<string>();
    if (echoed != "hi") Fail($"echo round-trip: expected 'hi', got '{echoed}'");

    serverTask.Wait();
    Console.WriteLine("PASS: echo round-trip");
}

void TestErrorPropagation()
{
    string pipeName = "sluice-ui-test-error-" + Environment.ProcessId;
    using var ready = new ManualResetEventSlim(false);
    Task serverTask = Task.Run(() => RunMockServer(pipeName, ready, request => new JsonObject
    {
        ["id"] = request["id"]!.DeepClone(),
        ["error"] = "intentional test failure",
    }));
    ready.Wait();

    using var client = new EngineClient(pipeName);
    bool threw = false;
    try
    {
        client.Call("boom");
    }
    catch (EngineErrorException ex)
    {
        threw = true;
        if (ex.Message != "intentional test failure")
            Fail($"error propagation: unexpected message '{ex.Message}'");
    }
    if (!threw) Fail("error propagation: expected EngineErrorException");

    serverTask.Wait();
    Console.WriteLine("PASS: error propagation");
}

void TestGetDevicesDeserialize()
{
    string pipeName = "sluice-ui-test-getdevices-" + Environment.ProcessId;
    using var ready = new ManualResetEventSlim(false);
    Task serverTask = Task.Run(() => RunMockServer(pipeName, ready, request =>
    {
        var devices = new JsonArray
        {
            new JsonObject
            {
                ["id"] = "asio:in:Test Driver",
                ["name"] = "Test Driver",
                ["backend"] = "asio",
                ["direction"] = "capture",
                ["lane"] = "rt",
                ["recommendedLane"] = "rt",
                ["effectiveLatencyMs"] = 1.33,
                ["bufferSizeFrames"] = 64,
                ["callbackCount"] = 100,
                ["underrunCount"] = 0,
                ["overrunCount"] = 1,
                ["xrunCount"] = 1,
                ["resetRequested"] = false,
                ["asrcRatio"] = 1.0000132,
                ["supports64"] = true,
                ["measuredLatencyMs"] = 2.75,
            },
        };
        return new JsonObject
        {
            ["id"] = request["id"]!.DeepClone(),
            ["result"] = devices,
        };
    }));
    ready.Wait();

    using var client = new EngineClient(pipeName);
    List<DeviceInfo> devices = client.GetDevices();
    serverTask.Wait();

    if (devices.Count != 1) Fail($"get_devices: expected 1 device, got {devices.Count}");
    DeviceInfo d = devices[0];
    if (d.Id != "asio:in:Test Driver") Fail($"get_devices: unexpected id '{d.Id}'");
    if (d.Lane != "rt") Fail($"get_devices: unexpected lane '{d.Lane}'");
    if (d.XrunCount != 1) Fail($"get_devices: unexpected xrunCount {d.XrunCount}");
    if (Math.Abs(d.AsrcRatio - 1.0000132) > 1e-9) Fail($"get_devices: unexpected asrcRatio {d.AsrcRatio}");
    if (Math.Abs(d.MeasuredLatencyMs - 2.75) > 1e-9)
        Fail($"get_devices: unexpected measuredLatencyMs {d.MeasuredLatencyMs}");

    Console.WriteLine("PASS: get_devices deserialize");
}

void TestGetTopologyDeserialize()
{
    // gap 10: Mixer タブが起動時に呼ぶ get_topology の往復(EngineClient.GetTopology
    // → TopologyInfo.FromJson)を検証する。engine/main.cpp の BuildTopologyJson と
    // 同じ形。
    string pipeName = "sluice-ui-test-gettopology-" + Environment.ProcessId;
    using var ready = new ManualResetEventSlim(false);
    Task serverTask = Task.Run(() => RunMockServer(pipeName, ready, request =>
    {
        var result = new JsonObject
        {
            ["strips"] = new JsonArray
            {
                new JsonObject
                {
                    ["index"] = 0,
                    ["deviceIndex"] = 1,
                    ["channel"] = 0,
                    ["boundaryIndex"] = 0,
                    ["gainDb"] = -3.0,
                    ["mute"] = false,
                    ["solo"] = false,
                    ["routingGain"] = new JsonArray { 0.0, -1000.0 },
                },
            },
            ["buses"] = new JsonArray
            {
                new JsonObject { ["index"] = 0, ["deviceIndex"] = 2, ["channel"] = 0, ["gainDb"] = 0.0 },
                new JsonObject { ["index"] = 1, ["deviceIndex"] = 3, ["channel"] = 0, ["gainDb"] = -6.0 },
            },
        };
        return new JsonObject { ["id"] = request["id"]!.DeepClone(), ["result"] = result };
    }));
    ready.Wait();

    using var client = new EngineClient(pipeName);
    TopologyInfo topology = client.GetTopology();
    serverTask.Wait();

    if (topology.Strips.Count != 1) Fail($"get_topology: expected 1 strip, got {topology.Strips.Count}");
    if (topology.Buses.Count != 2) Fail($"get_topology: expected 2 buses, got {topology.Buses.Count}");
    StripInfo s = topology.Strips[0];
    if (Math.Abs(s.GainDb - (-3.0)) > 1e-9) Fail($"get_topology: unexpected strip gainDb {s.GainDb}");
    if (s.RoutingGain.Count != 2) Fail($"get_topology: unexpected routingGain length {s.RoutingGain.Count}");
    if (Math.Abs(topology.Buses[1].GainDb - (-6.0)) > 1e-9)
        Fail($"get_topology: unexpected bus[1] gainDb {topology.Buses[1].GainDb}");

    Console.WriteLine("PASS: get_topology deserialize");
}

void TestSetStripParamRequestShape()
{
    // gap 10: StripViewModel が使う SetStripParam が、engine 側 set_param
    // ハンドラ(engine/main.cpp)の期待する params 形状を送ることを検証する。
    string pipeName = "sluice-ui-test-setparam-" + Environment.ProcessId;
    using var ready = new ManualResetEventSlim(false);
    JsonObject? capturedParams = null;
    Task serverTask = Task.Run(() => RunMockServer(pipeName, ready, request =>
    {
        capturedParams = request["params"]!.AsObject();
        return new JsonObject { ["id"] = request["id"]!.DeepClone(), ["result"] = new JsonObject() };
    }));
    ready.Wait();

    using var client = new EngineClient(pipeName);
    client.SetStripParam(2, gainDb: -4.5, mute: true, routingGain: new List<double> { 0.0, -1000.0 });
    serverTask.Wait();

    if (capturedParams is null) Fail("set_param: request was not captured");
    if (capturedParams!["target"]?.GetValue<string>() != "strip")
        Fail($"set_param: unexpected target '{capturedParams["target"]}'");
    if (capturedParams["index"]?.GetValue<int>() != 2)
        Fail($"set_param: unexpected index {capturedParams["index"]}");
    if (Math.Abs(capturedParams["gainDb"]!.GetValue<double>() - (-4.5)) > 1e-9)
        Fail($"set_param: unexpected gainDb {capturedParams["gainDb"]}");
    if (capturedParams["mute"]?.GetValue<bool>() != true)
        Fail($"set_param: unexpected mute {capturedParams["mute"]}");
    if (capturedParams["routingGain"] is not JsonArray routingArr || routingArr.Count != 2)
        Fail("set_param: unexpected routingGain shape");

    Console.WriteLine("PASS: set_param (strip) request shape");
}

void TestDevicesChangedPushNotification()
{
    // get_devices とは異なり、push 通知はリクエストなしにサーバから
    // 能動的に送られてくる(engine/ipc/pipe_server.h の Notify())。
    // このテストのモックサーバは接続後、応答を待たず即座に event 行を書く。
    string pipeName = "sluice-ui-test-push-" + Environment.ProcessId;
    using var ready = new ManualResetEventSlim(false);
    Task serverTask = Task.Run(() =>
    {
        using var server = new NamedPipeServerStream(pipeName, PipeDirection.InOut, 1);
        ready.Set();
        server.WaitForConnection();

        using var writer = new StreamWriter(server, new UTF8Encoding(false), leaveOpen: true)
        {
            AutoFlush = true,
            NewLine = "\n",
        };
        var evt = new JsonObject
        {
            ["event"] = "devices_changed",
            ["data"] = new JsonArray
            {
                new JsonObject { ["id"] = "x", ["name"] = "X", ["lane"] = "compat" },
            },
        };
        writer.WriteLine(evt.ToJsonString());
    });
    ready.Wait();

    using var client = new EngineClient(pipeName);
    var tcs = new TaskCompletionSource<List<DeviceInfo>>();
    client.DevicesChanged += devices => tcs.TrySetResult(devices);

    if (!tcs.Task.Wait(TimeSpan.FromSeconds(5)))
        Fail("devices_changed push notification: timed out waiting for event");

    List<DeviceInfo> received = tcs.Task.Result;
    if (received.Count != 1 || received[0].Id != "x")
        Fail($"devices_changed push notification: unexpected payload ({received.Count} items)");

    serverTask.Wait();
    Console.WriteLine("PASS: devices_changed push notification");
}

TestEchoRoundTrip();
TestErrorPropagation();
TestGetDevicesDeserialize();
TestGetTopologyDeserialize();
TestSetStripParamRequestShape();
TestDevicesChangedPushNotification();
Console.WriteLine("ALL PASS: SluiceUi.Core (EngineClient)");
