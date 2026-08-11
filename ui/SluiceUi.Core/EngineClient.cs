// EngineClient.cs : engine/ipc/pipe_server.h が話す名前付きパイプ JSON-RPC
// 的プロトコルの C# 側クライアント(実装ガイド §5.6)。
//
// プロトコル(engine/ipc/pipe_server.h と対になる):
//   リクエスト : {"id": <number>, "method": <string>, "params": {...}}
//   レスポンス : {"id": <同じ number>, "result": {...}} または
//               {"id": <同じ number>, "error": <string>}
//   通知(サーバ→クライアント、id なし): {"event": <string>, "data": {...}}
//
// 受信は専用のバックグラウンドスレッドで行を読み続け、"event" を持つ行は
// push 通知(DevicesChanged イベント)として即座にディスパッチし、それ以外
// (id を持つ応答)は _responseQueue に積む。Call() はこのキューから
// 同期的に取り出す —— サーバは 1 クライアントあたりのリクエストを
// 順番にしか処理しない(pipe_server.h は同時接続 1 クライアントまでの
// ミニマム実装)ため、Call() が複数スレッドから並行に呼ばれない限り
// キューは常に「直前のリクエストへの応答」だけを保持する。
//
// 追加の NuGet 依存を増やさない方針(実装ガイド §5.6 の「軽量」方針を
// C# 側でも踏襲): .NET 標準の System.IO.Pipes / System.Text.Json /
// System.Collections.Concurrent のみ使用。

using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;

using SluiceUi.Core.Models;

namespace SluiceUi.Core;

public sealed class EngineClient : IDisposable
{
    public const string DefaultPipeName = "sluice-engine";

    private readonly NamedPipeClientStream _pipe;
    private readonly StreamReader _reader;
    private readonly StreamWriter _writer;
    private readonly object _writeLock = new();
    private readonly BlockingCollection<JsonObject> _responseQueue = new();
    private readonly Thread _readThread;
    private int _nextId = 1;

    // "devices_changed" push 通知を受けるたびに発火する。ハンドラは
    // バックグラウンドの読み取りスレッドから呼ばれるため、WPF 側は
    // Dispatcher 経由で UI スレッドへマーシャリングすること。
    public event Action<List<DeviceInfo>>? DevicesChanged;

    public EngineClient(string pipeName = DefaultPipeName, int connectTimeoutMs = 2000)
    {
        _pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut,
                                          PipeOptions.None);
        _pipe.Connect(connectTimeoutMs);

        // engine 側(engine/ipc/pipe_server.h)は改行区切りの UTF-8 テキスト
        // なので、BOM なしの UTF-8 で揃える。
        _writer = new StreamWriter(_pipe, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
        _reader = new StreamReader(_pipe, new UTF8Encoding(false));

        _readThread = new Thread(ReadLoop) { IsBackground = true, Name = "EngineClient-Read" };
        _readThread.Start();
    }

    private void ReadLoop()
    {
        try
        {
            string? line;
            while ((line = _reader.ReadLine()) != null)
            {
                JsonObject obj;
                try
                {
                    obj = JsonNode.Parse(line)!.AsObject();
                }
                catch
                {
                    continue;  // 不正な行は無視(サーバ側の HandleLine と対称の割り切り)
                }

                if (obj.TryGetPropertyValue("event", out JsonNode? eventNode) && eventNode is not null)
                    DispatchEvent(eventNode.GetValue<string>(), obj);
                else
                    _responseQueue.Add(obj);
            }
        }
        catch (Exception)
        {
            // パイプ切断・Dispose 中の例外等。ループを終えるだけでよい
            // (Call() 側は _responseQueue.CompleteAdding() 経由で気付く)。
        }
        finally
        {
            _responseQueue.CompleteAdding();
        }
    }

    private void DispatchEvent(string eventName, JsonObject obj)
    {
        if (eventName != "devices_changed") return;
        if (!obj.TryGetPropertyValue("data", out JsonNode? dataNode) || dataNode is not JsonArray arr)
            return;

        var devices = new List<DeviceInfo>();
        foreach (JsonNode? item in arr)
            if (item is JsonObject deviceObj) devices.Add(DeviceInfo.FromJson(deviceObj));

        DevicesChanged?.Invoke(devices);
    }

    // method を呼び出し、result フィールドを返す。エンジン側がエラーを
    // 返した場合は EngineErrorException を投げる。
    public JsonNode? Call(string method, JsonObject? parameters = null)
    {
        int id = _nextId++;
        var request = new JsonObject
        {
            ["id"] = id,
            ["method"] = method,
            ["params"] = parameters ?? new JsonObject(),
        };

        lock (_writeLock)
        {
            _writer.WriteLine(request.ToJsonString());
        }

        JsonObject response;
        try
        {
            response = _responseQueue.Take();
        }
        catch (InvalidOperationException)
        {
            throw new IOException("engine pipe closed unexpectedly while waiting for a response");
        }

        if (response.TryGetPropertyValue("error", out JsonNode? errorNode) && errorNode is not null)
            throw new EngineErrorException(errorNode.GetValue<string>());

        response.TryGetPropertyValue("result", out JsonNode? result);
        return result;
    }

    // 初回取得用の同期ヘルパ。以降の更新は DevicesChanged イベント経由で届く。
    public List<DeviceInfo> GetDevices()
    {
        JsonNode? result = Call("get_devices");
        var devices = new List<DeviceInfo>();
        if (result is JsonArray arr)
            foreach (JsonNode? item in arr)
                if (item is JsonObject deviceObj) devices.Add(DeviceInfo.FromJson(deviceObj));
        return devices;
    }

    // 現在のストリップ/バス構成を取得する(実装ガイド §5.6 get_topology、
    // engine/main.cpp の BuildTopologyJson と対になる)。
    public TopologyInfo GetTopology()
    {
        JsonNode? result = Call("get_topology");
        return result is JsonObject obj ? TopologyInfo.FromJson(obj) : new TopologyInfo();
    }

    // ストリップのパラメータを部分更新する。null のフィールドは変更しない
    // (engine 側の set_param ハンドラは受け取ったキーだけを上書きする)。
    public void SetStripParam(int index, double? gainDb = null, bool? mute = null,
                              bool? solo = null, IReadOnlyList<double>? routingGain = null)
    {
        var parameters = new JsonObject { ["target"] = "strip", ["index"] = index };
        if (gainDb is not null) parameters["gainDb"] = gainDb.Value;
        if (mute is not null) parameters["mute"] = mute.Value;
        if (solo is not null) parameters["solo"] = solo.Value;
        if (routingGain is not null)
        {
            var arr = new JsonArray();
            foreach (double g in routingGain) arr.Add(g);
            parameters["routingGain"] = arr;
        }
        Call("set_param", parameters);
    }

    // バスのパラメータを部分更新する。
    public void SetBusParam(int index, double? gainDb = null)
    {
        var parameters = new JsonObject { ["target"] = "bus", ["index"] = index };
        if (gainDb is not null) parameters["gainDb"] = gainDb.Value;
        Call("set_param", parameters);
    }

    // 既に開いているデバイスの空きチャンネル(boundaryIndex で指定)に対して
    // 新しいストリップを追加する。デバイスそのものの実行時追加には対応して
    // いない(engine/main.cpp のコメント参照)。戻り値は新しいストリップの
    // index(get_topology で参照する番号と一致)。
    public int AddStrip(int boundaryIndex, int channel, double? gainDb = null)
    {
        var parameters = new JsonObject
        {
            ["boundaryIndex"] = boundaryIndex,
            ["channel"] = channel,
        };
        if (gainDb is not null) parameters["gainDb"] = gainDb.Value;
        JsonNode? result = Call("add_strip", parameters);
        return result is JsonObject obj && obj.TryGetPropertyValue("index", out JsonNode? n) &&
               n is not null
            ? n.GetValue<int>()
            : -1;
    }

    // ストリップを削除する。削除後、それ以降のストリップの index は 1 つずつ
    // 詰まる(engine/main.cpp: state->strips.erase())ため、呼び出し側は
    // 削除後に GetTopology() を取り直すこと。
    public void RemoveStrip(int index) =>
        Call("remove_strip", new JsonObject { ["index"] = index });

    public void Dispose()
    {
        _writer.Dispose();
        _reader.Dispose();
        _pipe.Dispose();
    }
}

public sealed class EngineErrorException : Exception
{
    public EngineErrorException(string message) : base(message) { }
}
