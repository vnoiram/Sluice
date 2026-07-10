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

TestEchoRoundTrip();
TestErrorPropagation();
Console.WriteLine("ALL PASS: SluiceUi.Core (EngineClient)");
