// EngineClient.cs : engine/ipc/pipe_server.h が話す名前付きパイプ JSON-RPC
// 的プロトコルの C# 側クライアント(実装ガイド §5.7)。
//
// プロトコル(engine/ipc/pipe_server.h と対になる):
//   リクエスト : {"id": <number>, "method": <string>, "params": {...}}
//   レスポンス : {"id": <同じ number>, "result": {...}} または
//               {"id": <同じ number>, "error": <string>}
//
// 通知({"event":...} の非同期購読)は今回のミニマム実装では未対応
// (同期リクエスト/レスポンスのみ)。UI 側でメータをライブ表示する際に
// 追加する。
//
// 追加の NuGet 依存を増やさない方針(実装ガイド §5.7 の「軽量」方針を
// C# 側でも踏襲): .NET 標準の System.IO.Pipes / System.Text.Json のみ使用。

using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;

namespace SluiceUi.Core;

public sealed class EngineClient : IDisposable
{
    public const string DefaultPipeName = "sluice-engine";

    private readonly NamedPipeClientStream _pipe;
    private readonly StreamReader _reader;
    private readonly StreamWriter _writer;
    private int _nextId = 1;

    public EngineClient(string pipeName = DefaultPipeName, int connectTimeoutMs = 2000)
    {
        _pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut,
                                          PipeOptions.None);
        _pipe.Connect(connectTimeoutMs);

        // engine 側(engine/ipc/pipe_server.h)は改行区切りの UTF-8 テキスト
        // なので、BOM なしの UTF-8 で揃える。
        _writer = new StreamWriter(_pipe, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
        _reader = new StreamReader(_pipe, new UTF8Encoding(false));
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

        _writer.WriteLine(request.ToJsonString());

        string? line = _reader.ReadLine();
        if (line is null)
            throw new IOException("engine pipe closed unexpectedly while waiting for a response");

        JsonObject response = JsonNode.Parse(line)!.AsObject();
        if (response.TryGetPropertyValue("error", out JsonNode? errorNode) && errorNode is not null)
            throw new EngineErrorException(errorNode.GetValue<string>());

        response.TryGetPropertyValue("result", out JsonNode? result);
        return result;
    }

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
