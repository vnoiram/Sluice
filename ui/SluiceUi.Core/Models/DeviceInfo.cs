// DeviceInfo.cs : engine/ipc/device_report.h が返す JSON 形状の C# 側モデル
// (実装ガイド §5.6「UI に必ず出すもの: 各デバイスのレーン、実効レイテンシ、
// xrun カウンタ、ASRC の現在比」)。
//
// フィールド名は engine 側の DeviceReportToJson(engine/ipc/device_report.h)
// と一致させている。

using System.Text.Json.Nodes;

namespace SluiceUi.Core.Models;

public sealed class DeviceInfo
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public string Backend { get; init; } = "";
    public string Direction { get; init; } = "";  // "capture" / "render"
    public string Lane { get; init; } = "";        // "rt" / "compat"
    public string RecommendedLane { get; init; } = "";
    public double EffectiveLatencyMs { get; init; }
    public long BufferSizeFrames { get; init; }
    public ulong CallbackCount { get; init; }
    public ulong UnderrunCount { get; init; }
    public ulong OverrunCount { get; init; }
    public ulong XrunCount { get; init; }
    public bool ResetRequested { get; init; }
    public double AsrcRatio { get; init; }
    public bool Supports64 { get; init; }
    // tools/latencybench --json の実測値。0 = 未測定(main.cpp が
    // ipc::LookupMeasuredLatencyMs でマージしなければこのまま、gap 8)。
    public double MeasuredLatencyMs { get; init; }

    public static DeviceInfo FromJson(JsonObject obj) => new()
    {
        Id = GetString(obj, "id"),
        Name = GetString(obj, "name"),
        Backend = GetString(obj, "backend"),
        Direction = GetString(obj, "direction"),
        Lane = GetString(obj, "lane"),
        RecommendedLane = GetString(obj, "recommendedLane"),
        EffectiveLatencyMs = GetDouble(obj, "effectiveLatencyMs"),
        BufferSizeFrames = (long)GetDouble(obj, "bufferSizeFrames"),
        CallbackCount = (ulong)GetDouble(obj, "callbackCount"),
        UnderrunCount = (ulong)GetDouble(obj, "underrunCount"),
        OverrunCount = (ulong)GetDouble(obj, "overrunCount"),
        XrunCount = (ulong)GetDouble(obj, "xrunCount"),
        ResetRequested = GetBool(obj, "resetRequested"),
        AsrcRatio = GetDouble(obj, "asrcRatio"),
        Supports64 = GetBool(obj, "supports64"),
        MeasuredLatencyMs = GetDouble(obj, "measuredLatencyMs"),
    };

    private static string GetString(JsonObject obj, string key) =>
        obj.TryGetPropertyValue(key, out JsonNode? n) && n is not null ? n.GetValue<string>() : "";

    private static double GetDouble(JsonObject obj, string key) =>
        obj.TryGetPropertyValue(key, out JsonNode? n) && n is not null ? n.GetValue<double>() : 0.0;

    private static bool GetBool(JsonObject obj, string key) =>
        obj.TryGetPropertyValue(key, out JsonNode? n) && n is not null && n.GetValue<bool>();
}
