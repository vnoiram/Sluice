#pragma once
// asio_abi.h : ASIO ドライバインターフェースの独自定義(クリーンルーム実装)
//
// Steinberg の ASIO SDK を一切使わずに、実在の ASIO ドライバ/DAW と相互運用
// できる ABI(vtable レイアウト・構造体のフィールド配置・定数の数値)だけを
// 独自に書き起こしたヘッダ。wineasio をはじめとする複数の OSS プロジェクトが
// 採る手法と同じ考え方に基づく:相互運用に必要なインターフェース形状
// (メソッド名・引数の型と順序・構造体のメモリレイアウト)は「事実」であり、
// SDK ヘッダの文章表現そのもの(コメント・説明文)とは別物である
// (実装ガイド §12「クリーンルーム ASIO ヘッダ方式」も同じ想定)。
//
// このファイルの正確性は、開発時にローカルへ配置した実 SDK ヘッダ
// (.gitignore 対象、リポジトリには含まれない)と突き合わせて確認している
// (asio-abi/README.md 参照)。ただし文章・コメント・ファイル構成は独自に
// 書き下ろしたものであり、SDK のテキストを転記したものではない。
//
// "ASIO" is a trademark and software of Steinberg Media Technologies GmbH.
// 本ファイルは Steinberg 社によって提供・承認・監修されたものではない。

// WIN32_LEAN_AND_MEAN 付きで <windows.h> を先にインクルードすると
// <objbase.h>(IUnknown 等 COM の基礎)が自動では入らない罠がある
// (engine/device/asio_host.h の「罠4」と同じ理由)。この順序を守ること。
#include <windows.h>
#include <objbase.h>  // IUnknown, HRESULT, REFIID 等(Windows SDK 標準。ASIO SDK ではない)

// 4 バイト境界に固定する。実在のドライバ/ホストは全員この境界でビルドされて
// おり、ここがずれると構造体越しのやり取りが即座に壊れる。
#pragma pack(push, 4)

// --- 基本型 -----------------------------------------------------------------
// ASIOSampleRate は本来プラットフォーム依存(64bit 整数のネイティブ対応有無、
// IEEE754 倍精度浮動小数点対応有無)で表現が変わるが、Windows(x86/x64)では
// 常に pure な double 表現になる。本プロジェクトは Windows 専用なのでここは
// 分岐させず double 固定とする。

typedef long ASIOBool;
enum : long { ASIOFalse = 0, ASIOTrue = 1 };

typedef long ASIOError;
enum : long {
    ASE_OK = 0,                  // 成功
    ASE_SUCCESS = 0x3f4847a0,    // future() が成功したときだけ使う特別な成功値(ASE_OK では不十分)
    ASE_NotPresent = -1000,      // 該当する入力/出力が存在しない
    ASE_HWMalfunction = -999,    // ハードウェア異常
    ASE_InvalidParameter = -998, // 引数が不正
    ASE_InvalidMode = -997,      // ハードウェアが現在のモードでは実行できない
    ASE_SPNotAdvancing = -996,   // クロックが進んでいない(サンプル位置問い合わせ時)
    ASE_NoClock = -995,          // クロック/サンプルレートが不明または未接続
    ASE_NoMemory = -994,         // メモリ不足
};

typedef long ASIOSampleType;
enum : long {
    ASIOSTInt16MSB = 0,
    ASIOSTInt24MSB = 1,
    ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16,
    ASIOSTInt24LSB = 17,
    ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19,  // IEEE754 単精度、x86 系のネイティブエンディアン
    ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27,
    ASIOSTDSDInt8LSB1 = 32,
    ASIOSTDSDInt8MSB1 = 33,
    ASIOSTDSDInt8NER8 = 40,
};

typedef double ASIOSampleRate;

// 64bit のサンプル数/タイムスタンプ。Windows ターゲットではネイティブ 64bit
// 整数ではなく上下 32bit に分けた構造体表現を使う(ドライバ/ホスト間の ABI が
// これで固定されているため、long long への変更は不可)。
struct ASIOSamples {
    unsigned long hi;
    unsigned long lo;
};
struct ASIOTimeStamp {
    unsigned long hi;
    unsigned long lo;
};

// --- タイムコード/タイムインフォ(bufferSwitchTimeInfo モード用) -----------
// このプロジェクトは timeInfo モードを使わない(vasio 側は future() で
// kAsioCanTimeInfo に非対応と答える)ため、フィールドを読み書きすることは
// 無いが、コールバック引数の型として ASIOTime* が必要になるため定義だけは
// 用意する。

struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
};

struct AsioTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
};

struct ASIOTime {
    long reserved[4];
    AsioTimeInfo timeInfo;
    ASIOTimeCode timeCode;
};

// --- チャンネル/バッファ ------------------------------------------------------

struct ASIOChannelInfo {
    long channel;          // in: 問い合わせ対象のチャンネル index
    ASIOBool isInput;      // in
    ASIOBool isActive;     // out
    long channelGroup;     // out
    ASIOSampleType type;   // out
    char name[32];         // out
};

struct ASIOBufferInfo {
    ASIOBool isInput;   // in: ASIOTrue なら入力用、そうでなければ出力用
    long channelNum;    // in: チャンネル index
    void* buffers[2];   // out: ダブルバッファの各半分の先頭アドレス
};

struct ASIOCallbacks {
    // 各ブロックの入出力切り替え時に呼ばれる(timeInfo モード未使用時)。
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    // サンプルレートが変化した(または不明になった)ときに呼ばれる。
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    // kAsioXxx セレクタによる汎用通知(下記の asioMessage セレクタ参照)。
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    // timeInfo モード用の bufferSwitch。本プロジェクトは対応しないため
    // 呼ばれない想定だが、フィールドとしては存在する必要がある
    // (呼び出し側がこの構造体全体をそのままドライバへ渡すため)。
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex,
                                      ASIOBool directProcess);
};

// asioMessage() の selector 値。数値そのものがワイヤ上の合意なので、
// 順序ではなく値を正確に合わせる必要がある。
enum : long {
    kAsioSelectorSupported = 1,
    kAsioEngineVersion = 2,
    kAsioResetRequest = 3,
    kAsioBufferSizeChange = 4,
    kAsioResyncRequest = 5,
    kAsioLatenciesChanged = 6,
    kAsioSupportsTimeInfo = 7,
    kAsioSupportsTimeCode = 8,
    kAsioMMCCommand = 9,
    kAsioSupportsInputMonitor = 10,
    kAsioSupportsInputGain = 11,
    kAsioSupportsInputMeter = 12,
    kAsioSupportsOutputGain = 13,
    kAsioSupportsOutputMeter = 14,
    kAsioOverload = 15,
    kAsioNumMessageSelectors = 16,
};

// --- クロックソース -----------------------------------------------------------

struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
};

// --- future() の selector(kAsioCanTimeInfo のみこのプロジェクトで使用) -----

enum : long {
    kAsioEnableTimeCodeRead = 1,
    kAsioDisableTimeCodeRead = 2,
    kAsioSetInputMonitor = 3,
    kAsioTransport = 4,
    kAsioSetInputGain = 5,
    kAsioGetInputMeter = 6,
    kAsioSetOutputGain = 7,
    kAsioGetOutputMeter = 8,
    kAsioCanInputMonitor = 9,
    kAsioCanTimeInfo = 10,
    kAsioCanTimeCode = 11,
    kAsioCanTransport = 12,
    kAsioCanInputGain = 13,
    kAsioCanInputMeter = 14,
    kAsioCanOutputGain = 15,
    kAsioCanOutputMeter = 16,
    kAsioOptionalOne = 17,
    kAsioSetIoFormat = 0x23111961,
    kAsioGetIoFormat = 0x23111983,
    kAsioCanDoIoFormat = 0x23112004,
    kAsioCanReportOverload = 0x24042012,
    kAsioGetInternalBufferSamples = 0x25042012,
};

// future() 経由でのみ使われる補助構造体(このプロジェクトでは未使用だが、
// ABI 上の完全性のために定義しておく)。
struct ASIOInputMonitor {
    long input;
    long output;
    long gain;
    ASIOBool state;
    long pan;
};
struct ASIOChannelControls {
    long channel;
    ASIOBool isInput;
    long gain;
    long meter;
    char future[32];
};
struct ASIOTransportParameters {
    long command;
    ASIOSamples samplePosition;
    long track;
    long trackSwitches[16];
    char future[64];
};
typedef long ASIOIoFormatType;
enum : long { kASIOFormatInvalid = -1, kASIOPCMFormat = 0, kASIODSDFormat = 1 };
struct ASIOIoFormat {
    ASIOIoFormatType FormatType;
    char future[512 - sizeof(ASIOIoFormatType)];
};
struct ASIOInternalBufferInfo {
    long inputSamples;
    long outputSamples;
};

// ASIOInit() 用(このプロジェクトは CoCreateInstance 直後に IASIO::init() を
// 呼ぶだけで ASIOInit() 相当のラッパー関数は使わないが、ABI 上の完全性の
// ために定義しておく)。
struct ASIODriverInfo {
    long asioVersion;
    long driverVersion;
    char name[32];
    char errorMessage[124];
    void* sysRef;
};

// --- IASIO 本体 ---------------------------------------------------------------
//
// メソッドの宣言順は vtable レイアウトそのものであり、実在のドライバ/ホスト
// 全てがこの順序を前提に COM 呼び出しを行う。1 つでも入れ替えると全メソッド
// 呼び出しが別のスロットにずれて誤動作する。init から outputReady までの
// 21 メソッドの順序は、開発時にローカルの実 SDK ヘッダと突き合わせて確認
// 済み(asio-abi/README.md 参照)。

struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize,
                                    long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                    ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

#pragma pack(pop)
