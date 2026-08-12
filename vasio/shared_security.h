#pragma once
// shared_security.h : 名前付きカーネルオブジェクト(CreateFileMappingW /
// CreateEventW / CreateNamedPipeW)へ「現在のユーザーのみアクセス可」の
// DACL を付与する共有ヘルパ。
//
// 罠: engine と vasio.dll(DAW プロセス内)はどちらも CreateFileMappingW /
// CreateEventW を呼び、先に呼んだ方だけが実際に作成し、後発は
// ERROR_ALREADY_EXISTS で既存を開く(shared_protocol.h 参照)。
// lpSecurityAttributes は「実際に新規作成したプロセス」の指定のみが有効に
// なるため、両プロセスがこのヘルパで「現在ユーザーの SID」から DACL を
// 構築する限り、どちらが先に作成しても同じ効果(同一ユーザーのみアクセス
// 可)になる。
//
// Windows API 依存のため shared_protocol.h(プラットフォーム非依存)には
// 含めない。

#include <windows.h>
#include <accctrl.h>
#include <aclapi.h>

#include <memory>
#include <vector>

namespace vasio {

class CurrentUserOnlySecurityAttributes {
public:
    CurrentUserOnlySecurityAttributes() { valid_ = Build(); }

    // 構築失敗時は nullptr を返す。呼び出し側は既定 DACL にフォールバック
    // してよい(可用性を優先し、致命的エラーにはしない)。
    SECURITY_ATTRIBUTES* attributes() { return valid_ ? &sa_ : nullptr; }
    bool valid() const { return valid_; }

private:
    bool Build() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        if (needed == 0) {
            CloseHandle(token);
            return false;
        }
        tokenUserBuf_.resize(needed);
        const bool gotUser = GetTokenInformation(token, TokenUser, tokenUserBuf_.data(), needed,
                                                  &needed) != 0;
        CloseHandle(token);
        if (!gotUser) return false;

        PSID userSid = reinterpret_cast<PTOKEN_USER>(tokenUserBuf_.data())->User.Sid;

        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = GENERIC_ALL;
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(userSid);

        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS || !acl) return false;
        acl_.reset(acl);

        if (!InitializeSecurityDescriptor(&sd_, SECURITY_DESCRIPTOR_REVISION)) return false;
        if (!SetSecurityDescriptorDacl(&sd_, TRUE, acl_.get(), FALSE)) return false;

        sa_.nLength = sizeof(sa_);
        sa_.lpSecurityDescriptor = &sd_;
        sa_.bInheritHandle = FALSE;
        return true;
    }

    struct LocalFreeDeleter {
        void operator()(ACL* p) const {
            if (p) LocalFree(p);
        }
    };

    std::vector<uint8_t> tokenUserBuf_;
    std::unique_ptr<ACL, LocalFreeDeleter> acl_;
    SECURITY_DESCRIPTOR sd_{};
    SECURITY_ATTRIBUTES sa_{};
    bool valid_ = false;
};

}  // namespace vasio
