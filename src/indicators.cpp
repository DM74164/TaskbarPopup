// 迷你任务栏右边的电量和输入法状态
#include "common.h"

#include <imm.h>

#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_SETCONVERSIONMODE
#define IMC_SETCONVERSIONMODE 0x0002
#endif
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS 0x0005
#endif
#ifndef IMC_SETOPENSTATUS
#define IMC_SETOPENSTATUS 0x0006
#endif

namespace app {
namespace {

constexpr UINT kImeTimeoutMs = 100;  // 问别的程序的输入法窗口最多等这么久（它卡住时别跟着卡）

// 发给 window 那个线程的默认输入法窗口
bool ImeControl(HWND ime, WPARAM command, LPARAM value, DWORD_PTR& result) {
    result = 0;
    return SendMessageTimeoutW(ime, WM_IME_CONTROL, command, value, SMTO_ABORTIFHUNG | SMTO_BLOCK, kImeTimeoutMs,
                               &result) != 0;
}

// 输入法开着、是中文（日文、韩文）模式
bool ReadNative(HWND window, bool& native) {
    HWND ime = window ? ImmGetDefaultIMEWnd(window) : nullptr;
    DWORD_PTR open = 0, mode = 0;
    if (!ime || !ImeControl(ime, IMC_GETOPENSTATUS, 0, open) || !ImeControl(ime, IMC_GETCONVERSIONMODE, 0, mode))
        return false;
    native = open && (mode & IME_CMODE_NATIVE);
    return true;
}

}  // namespace

BatteryInfo Battery_Get() {
    BatteryInfo info;
    SYSTEM_POWER_STATUS ps = {};
    if (!GetSystemPowerStatus(&ps) || ps.BatteryFlag == 128 || ps.BatteryFlag == 255) return info;  // 没有电池、不知道
    info.present = true;
    info.percent = ps.BatteryLifePercent <= 100 ? ps.BatteryLifePercent : -1;
    info.charging = (ps.BatteryFlag & 8) != 0;
    info.plugged = ps.ACLineStatus == 1;
    info.secondsLeft = ps.BatteryLifeTime != static_cast<DWORD>(-1) ? static_cast<int>(ps.BatteryLifeTime) : -1;
    return info;
}

// 中文输入法显示“中 / 英”（和输入法自己的一样），日文“あ / A”，韩文“가 / A”；
// 别的键盘布局、读不到中英文（比如那个程序权限比本程序高）时显示语言的缩写，如 ENG
ImeInfo Ime_Get(HWND window) {
    ImeInfo info;
    DWORD thread = window ? GetWindowThreadProcessId(window, nullptr) : 0;
    LANGID lang = LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(thread)));
    WORD primary = PRIMARYLANGID(lang);
    bool native = false;
    if ((primary == LANG_CHINESE || primary == LANG_JAPANESE || primary == LANG_KOREAN) && ReadNative(window, native)) {
        info.canToggle = true;
        const wchar_t* nativeText = L"中文输入（点一下切换）";
        if (primary == LANG_CHINESE) {
            info.label = native ? L"中" : L"英";
        } else if (primary == LANG_JAPANESE) {
            info.label = native ? L"あ" : L"A";
            nativeText = L"日文输入（点一下切换）";
        } else {
            info.label = native ? L"가" : L"A";
            nativeText = L"韩文输入（点一下切换）";
        }
        info.text = Tr(native ? nativeText : L"英文输入（点一下切换）");
        return info;
    }
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {}, abbr[16] = {}, name[128] = {};
    if (LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0)) {
        GetLocaleInfoEx(locale, LOCALE_SISO639LANGNAME2, abbr, ARRAYSIZE(abbr));
        GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, name, ARRAYSIZE(name));
    }
    CharUpperW(abbr);
    info.label = primary == LANG_CHINESE ? L"中" : abbr;
    if (info.label.empty()) info.label = L"?";
    info.text = name[0] ? name : info.label;
    return info;
}

bool Ime_Toggle(HWND window) {
    HWND ime = window ? ImmGetDefaultIMEWnd(window) : nullptr;
    DWORD_PTR open = 0, mode = 0, done = 0;
    if (!ime || !ImeControl(ime, IMC_GETOPENSTATUS, 0, open) || !ImeControl(ime, IMC_GETCONVERSIONMODE, 0, mode))
        return false;
    bool native = open && (mode & IME_CMODE_NATIVE);
    if (!open && !ImeControl(ime, IMC_SETOPENSTATUS, 1, done)) return false;
    mode = native ? (mode & ~static_cast<DWORD_PTR>(IME_CMODE_NATIVE)) : (mode | IME_CMODE_NATIVE);
    if (!ImeControl(ime, IMC_SETCONVERSIONMODE, static_cast<LPARAM>(mode), done)) return false;
    bool now = false;
    return ReadNative(window, now) && now != native;
}

}  // namespace app
