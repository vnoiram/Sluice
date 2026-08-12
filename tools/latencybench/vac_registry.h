#pragma once
// vac_registry.h : VAC (Virtual Audio Cable) の内部設定("ms per int")を
// レジストリ経由で読み書きする(docs/audio-router-implementation-gap.md
// gap 9 の一部: VAC 内部設定の自動スイープ)。
//
// 一次資料(公式マニュアル):
//   https://vac.muzychenko.net/en/manual/configure.htm
//     「Configuration data in the Registry」節。ケーブルごとの設定は
//     HKLM\SOFTWARE\EuMus Design\Virtual Audio Cable\4\Cable <N> 以下に
//     格納され、「値名は(スペースも含めて)文字どおり、大文字小文字は
//     区別しない」。"ms per int" に対応する値名は
//     "Milliseconds per interrupt"(範囲 1..20、DWORD)。
//   https://vac.muzychenko.net/en/manual/troubles.htm
//     レジストリ変更後は VAC ドライバの再起動が必要、との記載。例として
//     挙がっているサービスキー名は
//     "VirtualAudioCable_83ed7f0e-2028-4956-b0b4-39c76fdaef1d"。
//
// 未検証事項(実機の VAC インストールがないと確認できない。実装ガイド
// gap 9 が元々見送られた理由と同じ制約):
//   - "Cable <N>" の N が VAC Control Panel 上のケーブル番号(vac.h の
//     DetectVac() が返す lineNumber、"Line N" の N)と一致するかどうか。
//     ここでは一致する前提で実装している。
//   - サービス名の "VirtualAudioCable_" 接頭辞・GUID 部分がバージョン間で
//     安定しているかどうか(トラブルシューティングページの例 1 件のみが
//     根拠)。ここでは前方一致で検索する。実機での初回検証時、この前方
//     一致に一致するサービスが実際に見つかるかどうかも含めて未確認
//     (見つからなければ ERROR_SERVICE_DOES_NOT_EXIST 相当のエラーになる
//     はずだが、ControlService/StartServiceW 自体のエラーコードで判別する
//     設計にしてある)。
//   - サービスの Stop/Start が、マニュアルが案内する VAC Control Panel の
//     "Restart Driver" 機能と同等に新しい値を反映するかどうか。追加調査で、
//     マニュアル本文には「"Restarting System Audio Service"(Windows Audio
//     サービス自体の再起動)」という表現が Control Panel の操作として
//     出てくることが分かった ── これが事実なら、VAC 自身のドライバサービス
//     ではなく OS の "Audiosrv"(Windows Audio)サービスを再起動するのが
//     正しい経路である可能性がある(未確定。ただし影響範囲が PC 全体の
//     オーディオに広がるため、実装を変更する前に実機での実際のエラーを
//     見て判断すること)。
//   - VAC には Lite 版があり(複数ケーブルの本数制限など機能が絞られる)、
//     レジストリレイアウト・サービス名がフル版と同一かどうかは未確認。
//
// そのため、この機能は latencybench 側で明示的にフラグを指定した場合のみ
// 使う実験的機能として扱う。失敗時(レジストリアクセス拒否・サービスが
// 見つからない・再起動確認タイムアウト等)は古い設定のまま測定を続行せず、
// 呼び出し側がその設定値のスイープ全体をスキップし、ユーザーに VAC Control
// Panel からの手動再起動を促すこと。

#include <windows.h>

#include <string>
#include <vector>

namespace vac_registry {

inline std::wstring CableKeyPath(int lineNumber) {
    return L"SOFTWARE\\EuMus Design\\Virtual Audio Cable\\4\\Cable " + std::to_wstring(lineNumber);
}

inline bool ReadMsPerInt(int lineNumber, DWORD* outValue, std::wstring* err) {
    HKEY hKey;
    const std::wstring path = CableKeyPath(lineNumber);
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE, &hKey);
    if (rc != ERROR_SUCCESS) {
        if (err) *err = L"RegOpenKeyExW(" + path + L") failed: " + std::to_wstring(rc);
        return false;
    }
    DWORD value = 0, size = sizeof(value), type = 0;
    rc = RegQueryValueExW(hKey, L"Milliseconds per interrupt", nullptr, &type,
                          reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        if (err) *err = L"RegQueryValueExW(\"Milliseconds per interrupt\") failed: rc=" +
                        std::to_wstring(rc) + L" type=" + std::to_wstring(type);
        return false;
    }
    *outValue = value;
    return true;
}

inline bool WriteMsPerInt(int lineNumber, DWORD value, std::wstring* err) {
    if (value < 1 || value > 20) {
        if (err) *err = L"value out of documented range (1..20)";
        return false;
    }
    HKEY hKey;
    const std::wstring path = CableKeyPath(lineNumber);
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                              KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        if (err) {
            *err = L"RegCreateKeyExW(" + path + L") failed: " + std::to_wstring(rc);
            if (rc == ERROR_ACCESS_DENIED) *err += L" (管理者として実行してください)";
        }
        return false;
    }
    rc = RegSetValueExW(hKey, L"Milliseconds per interrupt", 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        if (err) *err = L"RegSetValueExW failed: " + std::to_wstring(rc);
        return false;
    }
    return true;
}

// VAC のカーネルドライバサービスを Stop→Start して設定変更を反映させる。
// 成功を返しても、実際に新しい "Milliseconds per interrupt" が反映された
// ことまでは保証しない(マニュアル上グラウンディングされているのはあくまで
// VAC Control Panel の "Restart Driver" 機能であり、サービス経由の再起動が
// 等価であることは実機未検証)。呼び出し側は、可能なら測定後に
// ReadMsPerInt で書き込んだ値が読み戻せることを確認し、xrun 増加など
// 不審な挙動があればその測定行を信頼しないこと。
inline bool RestartDriverService(std::wstring* err) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) {
        if (err) *err = L"OpenSCManagerW failed: " + std::to_wstring(GetLastError());
        return false;
    }

    DWORD bytesNeeded = 0, serviceCount = 0, resumeHandle = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL, nullptr, 0,
                         &bytesNeeded, &serviceCount, &resumeHandle, nullptr);

    std::wstring serviceName;
    if (bytesNeeded > 0) {
        std::vector<BYTE> buffer(bytesNeeded);
        DWORD returned = 0;
        resumeHandle = 0;
        if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL,
                                  buffer.data(), (DWORD)buffer.size(), &bytesNeeded, &returned,
                                  &resumeHandle, nullptr)) {
            auto* entries = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
            for (DWORD i = 0; i < returned; ++i) {
                const wchar_t* name = entries[i].lpServiceName;
                if (name && std::wstring(name).find(L"VirtualAudioCable_") == 0) {
                    serviceName = name;
                    break;
                }
            }
        }
    }

    if (serviceName.empty()) {
        CloseServiceHandle(scm);
        if (err)
            *err = L"VirtualAudioCable_* サービスが見つからない"
                  L"(未インストール、またはサービス名の前提が誤っている可能性)";
        return false;
    }

    SC_HANDLE svc =
        OpenServiceW(scm, serviceName.c_str(), SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        if (err) {
            *err = L"OpenServiceW(" + serviceName + L") failed: " + std::to_wstring(GetLastError());
            if (GetLastError() == ERROR_ACCESS_DENIED) *err += L" (管理者として実行してください)";
        }
        return false;
    }

    SERVICE_STATUS status{};
    if (!QueryServiceStatus(svc, &status)) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (err)
            *err = L"QueryServiceStatus(" + serviceName +
                  L") failed before stop: " + std::to_wstring(GetLastError());
        return false;
    }

    bool stopped = (status.dwCurrentState == SERVICE_STOPPED);
    if (!stopped) {
        // ControlService の戻り値を必ず見る。WDM/PnP 系のドライバサービスは
        // 通常の Win32 サービスと違い、そもそも SERVICE_CONTROL_STOP を
        // 受理しない(ERROR_INVALID_SERVICE_CONTROL)、依存関係で拒否される
        // (ERROR_DEPENDENT_SERVICES_RUNNING)等の理由で、単なるタイムアウト
        // 待ちでは原因が分からない(vac_registry.h 冒頭のコメント参照:
        // VAC Control Panel の "Restart Driver" と同等かどうかは未検証)。
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
            const DWORD stopErr = GetLastError();
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            if (err) {
                *err = L"ControlService(" + serviceName +
                      L", SERVICE_CONTROL_STOP) failed: " + std::to_wstring(stopErr);
                if (stopErr == ERROR_ACCESS_DENIED) *err += L" (管理者として実行してください)";
                if (stopErr == ERROR_DEPENDENT_SERVICES_RUNNING)
                    *err += L" (このドライバに依存する何かが起動中。オーディオを"
                           L"使っているアプリを全て閉じてから再試行を)";
                if (stopErr == ERROR_INVALID_SERVICE_CONTROL)
                    *err += L" (このドライバは STOP 制御コード自体を受理しない設計の"
                           L"可能性が高い。SCM 経由の再起動は使えず、VAC Control Panel の"
                           L"\"Restart Driver\" を手動で使うしかないかもしれない)";
            }
            return false;
        }
        for (int i = 0; i < 50; ++i) {
            if (!QueryServiceStatus(svc, &status)) break;
            if (status.dwCurrentState == SERVICE_STOPPED) {
                stopped = true;
                break;
            }
            Sleep(100);
        }
    }
    if (!stopped) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (err)
            *err = L"service " + serviceName +
                  L" did not reach SERVICE_STOPPED within timeout (last state=" +
                  std::to_wstring(status.dwCurrentState) + L")";
        return false;
    }

    if (!StartServiceW(svc, 0, nullptr)) {
        const DWORD startErr = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (err)
            *err = L"StartServiceW(" + serviceName + L") failed: " + std::to_wstring(startErr);
        return false;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    // ドライバ再初期化・デバイス列挙の再構築を待つための猶予(未検証の
    // 経験的な値。実機で足りなければ呼び出し側で追加のリトライを検討)。
    Sleep(300);
    return true;
}

}  // namespace vac_registry
