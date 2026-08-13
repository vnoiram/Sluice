#pragma once
// format_caps.h : DeviceCaps を人間可読な1行に整形する(main.cpp から
// 分離しているのは、DeviceCaps 自体が Windows 非依存(engine/device/
// iaudio_device.h)であるためテストしやすくするため。tests/ 参照)。

#include <cstdio>
#include <cwchar>
#include <string>

#include "device/iaudio_device.h"

inline std::wstring FormatCaps(const DeviceCaps& caps) {
    wchar_t buf[256];
    swprintf(buf, sizeof(buf) / sizeof(buf[0]),
             L"supports64=%ls  lane=%ls  min=%u  fundamental=%u  default=%u\n",
             caps.supports64 ? L"yes" : L"no", caps.recommendedLane == Lane::RT ? L"RT" : L"Compat",
             caps.minPeriodFrames, caps.fundamentalFrames, caps.defaultPeriodFrames);
    return buf;
}
