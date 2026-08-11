#pragma once
// vasio_bridge_device.h : vasio.dll(仮想 ASIO ドライバ)との共有メモリ
// コンシューマ(IAudioDevice 実装、実装ガイド §8.1 手順3 の engine 側)。
//
// vasio/README.md がこれまで「engine 側の共有メモリコンシューマは未実装」
// としていた部分の実装。プロトコル定義本体(vasio::SharedControlBlock 等)は
// vasio/shared_protocol.h に置かれており、vasio.dll(vasio/vasio_driver.cpp)
// と本ファイルの両方がそれを参照する(単一の情報源)。
//
// ★ 他の IAudioDevice 実装(ASIO/WASAPI/KS)との決定的な違い:
// この実装は**専用の RT スレッドを持たない**。vasio の DAW 側 bufferSwitch は
// 「エンジンのマスタークロックに従属する」設計(実装ガイド §8.1 手順4)であり、
// 逆に言えばエンジン側もこのデバイス自身の独立したタイマーを持たない。
// 共有メモリの読み書きは PumpSharedMemory() を通じて、マスターの RT スレッド
// (main.cpp の master ブロックコールバック)から EngineGraph::Process() の
// 直後に呼ばれる想定。そのため vasio ブリッジはマスタークロックの**候補には
// ならない**(main.cpp 側で除外する)。
//
// データパス(2 段階の中継になる理由: vasio.dll は DAW プロセス内にロードされる
// 別プロセスなので、共有メモリを介さないと越えられない):
//
//   [DAW] <--ASIO--> [vasio.dll(DAW プロセス内)] <--共有メモリ(vasio::RingRead/Write)-->
//     [VasioBridgeDevice::PumpSharedMemory (engine プロセス)] <--SpscRing<float>--> [EngineGraph]
//
// CaptureRing = ToEngine(DAW → エンジン、エンジンにとっての「仮想入力」)
// RenderRing  = FromEngine(エンジン → DAW、エンジンにとっての「仮想出力」)
//
// 既知の簡略化(このフェーズの意図的な割り切り):
//   - 常に kMaxChannels(8)チャンネル固定で開く(DeviceStreamConfig.channels は
//     参照しない。vasio.dll 側も既定 8in/8out 固定のため、要求チャンネル数を
//     絞る意味が薄い)
//   - kAsioResetRequest 相当の「エンジン側からの ResetPending 送出」は
//     RequestDawReset()(下記)で実装済み(実装ガイド §8.1 手順6)。呼び出しは
//     main.cpp の監視ループが「マスタークロックのブロックサイズが変わった」
//     ことを検出したときに行う(このデバイス自身が自発的に検出することはない)。
//   - Windows 実機/DAW での実接続確認は未実施(vasio.dll 側と同様、この
//     フェーズはビルド・オフラインロジックの実装までが対象)

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "device/iaudio_device.h"
#include "rt/spsc_ring.h"

// vasio/shared_protocol.h はリポジトリ直下 /vasio に独立して存在する
// (engine/ と同じ階層)。engine 側コンシューマ自体がこのヘッダの想定利用者
// なので、include パスをいじらずファイル相対で直接参照する。
#include "../../vasio/shared_protocol.h"

namespace vasiobridge {

class VasioBridgeDevice : public IAudioDevice {
public:
    VasioBridgeDevice() = default;
    ~VasioBridgeDevice() override { Close(); }
    VasioBridgeDevice(const VasioBridgeDevice&) = delete;

    // vasio 経由の DAW 接続は常に RT Lane(実装ガイド §8.1: クロックがエンジンに
    // 従属するため ASRC 不要)。共有メモリを実際に開かず定数を返す(Probe() は
    // 破棄可能な一時ハンドルで軽量に判定する契約だが、このバックエンドには
    // 「開く前に問い合わせるべき可変の周期」が無いため)。
    DeviceCaps Probe(double sampleRate) override;

    bool Open(const DeviceStreamConfig& config, std::wstring* errorOut) override;
    void Start() override;
    void Stop() override;
    void Close() override;

    SpscRing<float>* CaptureRing(int ch) override;
    SpscRing<float>* RenderRing(int ch) override;
    DeviceStatus Status() const override;

    // vasio は自身のブロックコールバックを駆動しない(マスタークロックの
    // 候補にならない)ため、実際には呼ばれない想定だが IAudioDevice の契約上
    // 実装は必要。登録だけ受け付ける。
    void SetBlockCallback(std::function<void(int frames)> fn) override {
        blockCallback_ = std::move(fn);
    }

    // マスターの blockCallback から、EngineGraph::Process(frames) の**後**に
    // 呼ぶ(実装ガイド §8.1 手順4 の「エンジンのマスタークロックが SetEvent
    // する」を体現する)。
    //   1. 共有メモリの ToEngine リング → プロセスローカル CaptureRing へコピー
    //      (この呼び出しで書いたデータは、次のブロックの EngineGraph::Process()
    //      から読まれる。他デバイスの RT スレッドと同様、1 ブロック程度の
    //      ズレは InputBoundary のプリフィル機構が許容する設計)
    //   2. プロセスローカル RenderRing(このブロックで EngineGraph が書いた
    //      ばかりのデータ)→ 共有メモリの FromEngine リングへコピー
    //   3. readyEvent を SetEvent して vasio.dll のワーカースレッドを起こす
    // 共有メモリ未接続(mappedBase_ が null、または相手がまだ
    // ringCapacityFrames を確定していない)の間は何もしない。
    void PumpSharedMemory(int frames);

    // 実装ガイド §8.1 手順6: エンジン側の設定変化(マスタークロックの
    // ブロックサイズ変更等)を DAW に伝える。vasio.dll(vasio_driver.cpp の
    // PumpOneBuffer)は ResetPending を見たら kAsioResetRequest を DAW へ
    // 一度送出し、自分で Connected に戻す — この関数はその ResetPending
    // への遷移だけを行う(vasio.dll 側は変更しない)。
    // 接続中(Connected)でなければ何もしない(Disconnected を誤って
    // 上書きしない、共有メモリ未接続時も安全に no-op)。
    void RequestDawReset();

    int Channels() const { return channels_; }

private:
    bool ConnectSharedMemory(std::wstring* errorOut);

    HANDLE mapping_ = nullptr;
    void* mappedBase_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    vasio::SharedMemoryLayout layout_{};

    int channels_ = 0;
    double sampleRate_ = 48000.0;

    std::vector<std::unique_ptr<SpscRing<float>>> captureRings_;  // ToEngine(プロセスローカル)
    std::vector<std::unique_ptr<SpscRing<float>>> renderRings_;   // FromEngine(プロセスローカル)
    std::vector<float> scratch_;  // PumpSharedMemory 内の中継用(起動前確保、1ch 分)

    std::function<void(int)> blockCallback_;
    std::atomic<uint64_t> pumpCount_{0};
};

}  // namespace vasiobridge
