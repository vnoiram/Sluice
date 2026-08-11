// TopologyInfo.cs : engine の get_topology が返す JSON 形状の C# 側モデル
// (実装ガイド §5.6)。engine/main.cpp の BuildTopologyJson と対になる。
//
// EQ/ゲート/コンプ/リミッタの詳細フィールドはここではモデル化していない
// (UI にまだそれらを編集するコントロールが無いため)。必要になったら
// StripInfo/BusInfo に追加する。基本のミキサー操作(ゲイン/ミュート/ソロ/
// ルーティング)だけを型付きで扱う。

using System.Text.Json.Nodes;

namespace SluiceUi.Core.Models;

public sealed class StripInfo
{
    public int Index { get; init; }
    public int DeviceIndex { get; init; }
    public int Channel { get; init; }
    public int BoundaryIndex { get; init; }
    public double GainDb { get; init; }
    public bool Mute { get; init; }
    public bool Solo { get; init; }
    public List<double> RoutingGain { get; init; } = new();

    public static StripInfo FromJson(JsonObject obj) => new()
    {
        Index = GetInt(obj, "index"),
        DeviceIndex = GetInt(obj, "deviceIndex"),
        Channel = GetInt(obj, "channel"),
        BoundaryIndex = GetInt(obj, "boundaryIndex"),
        GainDb = GetDouble(obj, "gainDb"),
        Mute = GetBool(obj, "mute"),
        Solo = GetBool(obj, "solo"),
        RoutingGain = obj.TryGetPropertyValue("routingGain", out JsonNode? n) && n is JsonArray arr
            ? arr.Select(x => x?.GetValue<double>() ?? 0.0).ToList()
            : new List<double>(),
    };

    private static int GetInt(JsonObject obj, string key) =>
        obj.TryGetPropertyValue(key, out JsonNode? n) && n is not null ? n.GetValue<int>() : 0;
    private static double GetDouble(JsonObject obj, string key) =>
        obj.TryGetPropertyValue(key, out JsonNode? n) && n is not null ? n.GetValue<double>() : 0.0;
    private static bool GetBool(JsonObject obj, string key) =>
        obj.TryGetPropertyValue(key, out JsonNode? n) && n is not null && n.GetValue<bool>();
}

public sealed class BusInfo
{
    public int Index { get; init; }
    public int DeviceIndex { get; init; }
    public int Channel { get; init; }
    public double GainDb { get; init; }

    public static BusInfo FromJson(JsonObject obj) => new()
    {
        Index = GetInt(obj, "index"),
        DeviceIndex = GetInt(obj, "deviceIndex"),
        Channel = GetInt(obj, "channel"),
        GainDb = GetDouble(obj, "gainDb"),
    };

    private static int GetInt(JsonObject obj, string key) =>
        obj.TryGetPropertyValue(key, out JsonNode? n) && n is not null ? n.GetValue<int>() : 0;
    private static double GetDouble(JsonObject obj, string key) =>
        obj.TryGetPropertyValue(key, out JsonNode? n) && n is not null ? n.GetValue<double>() : 0.0;
}

public sealed class TopologyInfo
{
    public List<StripInfo> Strips { get; init; } = new();
    public List<BusInfo> Buses { get; init; } = new();

    public static TopologyInfo FromJson(JsonObject obj)
    {
        var result = new TopologyInfo();
        if (obj.TryGetPropertyValue("strips", out JsonNode? stripsNode) && stripsNode is JsonArray stripsArr)
            foreach (JsonNode? item in stripsArr)
                if (item is JsonObject stripObj) result.Strips.Add(StripInfo.FromJson(stripObj));

        if (obj.TryGetPropertyValue("buses", out JsonNode? busesNode) && busesNode is JsonArray busesArr)
            foreach (JsonNode? item in busesArr)
                if (item is JsonObject busObj) result.Buses.Add(BusInfo.FromJson(busObj));

        return result;
    }
}
