// 快捷键：弹出迷你任务栏、固定（或取消固定）前台的小窗口。用 RegisterHotKey 挂在主窗口上，
// 管理员权限的程序在前台时也收得到。在设置窗口里录，存进设置文件时写成 Ctrl+Alt+Z 这样的文字
#include "common.h"

#include <cwchar>

namespace app {
namespace {

constexpr int kHotkeyIdBase = 1;  // RegisterHotKey 的编号 = kHotkeyIdBase + HotkeyId

struct NamedKey {
    UINT vk;
    const wchar_t* name;
};
const NamedKey kNamedKeys[] = {
    {VK_SPACE, L"Space"},     {VK_RETURN, L"Enter"},       {VK_TAB, L"Tab"},         {VK_BACK, L"Backspace"},
    {VK_DELETE, L"Delete"},   {VK_INSERT, L"Insert"},      {VK_HOME, L"Home"},       {VK_END, L"End"},
    {VK_PRIOR, L"PageUp"},    {VK_NEXT, L"PageDown"},      {VK_LEFT, L"Left"},       {VK_RIGHT, L"Right"},
    {VK_UP, L"Up"},           {VK_DOWN, L"Down"},          {VK_ESCAPE, L"Esc"},      {VK_PAUSE, L"Pause"},
    {VK_SCROLL, L"ScrollLock"}, {VK_SNAPSHOT, L"PrintScreen"}, {VK_APPS, L"Menu"},
    {VK_OEM_1, L";"},         {VK_OEM_PLUS, L"="},         {VK_OEM_COMMA, L","},     {VK_OEM_MINUS, L"-"},
    {VK_OEM_PERIOD, L"."},    {VK_OEM_2, L"/"},            {VK_OEM_3, L"`"},         {VK_OEM_4, L"["},
    {VK_OEM_5, L"\\"},        {VK_OEM_6, L"]"},            {VK_OEM_7, L"'"},
    {VK_MULTIPLY, L"Num*"},   {VK_ADD, L"Num+"},           {VK_SUBTRACT, L"Num-"},   {VK_DECIMAL, L"Num."},
    {VK_DIVIDE, L"Num/"},
};

// 修饰键按显示的顺序
struct NamedMod {
    UINT mod;
    const wchar_t* name;
};
const NamedMod kNamedMods[] = {{MOD_CONTROL, L"Ctrl"}, {MOD_SHIFT, L"Shift"}, {MOD_ALT, L"Alt"}};

UINT s_registered[kHotkeyCount] = {};  // 现在注册着的，0 = 没注册
bool s_failed[kHotkeyCount] = {};      // 设的那个注册不上
UINT s_warned[kHotkeyCount] = {};      // 已经弹气泡说过注册不上的
bool s_suspended = false;

UINT Wanted(int id) {
    if (id == kHotkeyPopup) return g_settings.popupHotkey;
    return g_settings.keepFloatsOnTop ? g_settings.pinHotkey : 0;  // 没打开“小窗口留在上面”时固定了也没用
}

}  // namespace

std::wstring KeyName(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
    for (const NamedKey& k : kNamedKeys)
        if (k.vk == vk) return k.name;
    return {};
}

std::wstring HotkeyText(UINT hotkey) {
    std::wstring key = KeyName(HotkeyVk(hotkey));
    if (!hotkey || key.empty()) return {};
    std::wstring text;
    for (const NamedMod& m : kNamedMods)
        if (HotkeyMods(hotkey) & m.mod) text += std::wstring(m.name) + L"+";
    return text + key;
}

UINT ParseHotkey(const std::wstring& text) {
    // 修饰键从前往后一个个去掉，剩下的是键名（键名本身可能带“+”，比如 Num+）
    std::wstring rest = text;
    UINT mods = 0;
    for (bool found = true; found;) {
        found = false;
        for (const NamedMod& m : kNamedMods) {
            size_t n = wcslen(m.name);
            if (rest.size() > n + 1 && rest[n] == L'+' && _wcsnicmp(rest.c_str(), m.name, n) == 0) {
                mods |= m.mod;
                rest.erase(0, n + 1);
                found = true;
            }
        }
    }
    for (UINT vk = 1; vk < 256; ++vk) {
        std::wstring name = KeyName(vk);
        if (!name.empty() && _wcsicmp(name.c_str(), rest.c_str()) == 0) return MakeHotkey(mods, vk);
    }
    return 0;
}

void Hotkeys_Apply() {
    for (int id = 0; id < kHotkeyCount; ++id) {
        UINT want = s_suspended ? 0 : Wanted(id);
        if (want == s_registered[id]) continue;
        if (s_registered[id]) UnregisterHotKey(g_mainWnd, kHotkeyIdBase + id);
        s_registered[id] = 0;
        if (!want) {
            if (!s_suspended) s_failed[id] = false;
            continue;
        }
        if (RegisterHotKey(g_mainWnd, kHotkeyIdBase + id, HotkeyMods(want) | MOD_NOREPEAT, HotkeyVk(want))) {
            s_registered[id] = want;
            s_failed[id] = false;
            Log(L"快捷键 %ls：%ls", HotkeyText(want).c_str(), id == kHotkeyPopup ? L"弹出迷你任务栏" : L"固定小窗口");
            continue;
        }
        s_failed[id] = true;
        Log(L"快捷键 %ls 注册不上（错误 %lu），可能被别的程序占用了", HotkeyText(want).c_str(), GetLastError());
        if (s_warned[id] == want) continue;
        std::wstring text = L"快捷键 " + HotkeyText(want) + L" 被别的程序占用了，在设置里换一个吧。";
        if (ShowTrayBalloon(L"TaskbarPopup", text.c_str())) s_warned[id] = want;  // 托盘图标还没加上时下次再说
    }
}

void Hotkeys_Suspend(bool suspend) {
    if (suspend == s_suspended) return;
    s_suspended = suspend;
    Hotkeys_Apply();
}

bool Hotkeys_Failed(int id) { return id >= 0 && id < kHotkeyCount && s_failed[id]; }

void Hotkeys_OnHotkey(WPARAM id) {
    if (id == kHotkeyIdBase + kHotkeyPopup) {
        Log(L"按了弹出迷你任务栏的快捷键");
        Popup_Toggle();
    } else if (id == kHotkeyIdBase + kHotkeyPin) {
        FloatWindows_TogglePin();
    }
}

}  // namespace app
