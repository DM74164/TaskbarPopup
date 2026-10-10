// 快捷键：弹出迷你任务栏、固定（或取消固定）小窗口。可以是组合键、单个键或鼠标键，单按或长按触发。
// 单按的键盘按键用 RegisterHotKey 挂在主窗口上，管理员权限的程序在前台时也收得到；
// 鼠标键、长按、单独的 Ctrl / Shift / Alt 和锁定键 RegisterHotKey 做不了，交给键盘钩子线程（hook.cpp）。
// 在设置窗口里录，存进设置文件时写成 Ctrl+Alt+Z、MouseMiddle 这样的文字
#include "common.h"

#include <cwchar>

namespace app {
namespace {

constexpr int kHotkeyIdBase = 1;  // RegisterHotKey 的编号 = kHotkeyIdBase + HotkeyId

struct NamedKey {
    UINT vk;
    const wchar_t* name;     // 写进设置文件的
    const wchar_t* display;  // 给人看的，空 = 和 name 一样
};
const NamedKey kNamedKeys[] = {
    {VK_SPACE, L"Space"},     {VK_RETURN, L"Enter"},       {VK_TAB, L"Tab"},         {VK_BACK, L"Backspace"},
    {VK_DELETE, L"Delete"},   {VK_INSERT, L"Insert"},      {VK_HOME, L"Home"},       {VK_END, L"End"},
    {VK_PRIOR, L"PageUp"},    {VK_NEXT, L"PageDown"},      {VK_LEFT, L"Left"},       {VK_RIGHT, L"Right"},
    {VK_UP, L"Up"},           {VK_DOWN, L"Down"},          {VK_ESCAPE, L"Esc"},      {VK_PAUSE, L"Pause"},
    {VK_SCROLL, L"ScrollLock"}, {VK_SNAPSHOT, L"PrintScreen"}, {VK_APPS, L"Menu"},   {VK_CAPITAL, L"CapsLock"},
    {VK_NUMLOCK, L"NumLock"},
    {VK_OEM_1, L";"},         {VK_OEM_PLUS, L"="},         {VK_OEM_COMMA, L","},     {VK_OEM_MINUS, L"-"},
    {VK_OEM_PERIOD, L"."},    {VK_OEM_2, L"/"},            {VK_OEM_3, L"`"},         {VK_OEM_4, L"["},
    {VK_OEM_5, L"\\"},        {VK_OEM_6, L"]"},            {VK_OEM_7, L"'"},
    {VK_MULTIPLY, L"Num*"},   {VK_ADD, L"Num+"},           {VK_SUBTRACT, L"Num-"},   {VK_DECIMAL, L"Num."},
    {VK_DIVIDE, L"Num/"},
    {VK_BROWSER_BACK, L"BrowserBack"},       {VK_BROWSER_FORWARD, L"BrowserForward"},
    {VK_BROWSER_REFRESH, L"BrowserRefresh"}, {VK_BROWSER_SEARCH, L"BrowserSearch"},
    {VK_BROWSER_FAVORITES, L"BrowserFavorites"}, {VK_BROWSER_HOME, L"BrowserHome"},
    {VK_MEDIA_PLAY_PAUSE, L"MediaPlayPause"}, {VK_MEDIA_NEXT_TRACK, L"MediaNext"},
    {VK_MEDIA_PREV_TRACK, L"MediaPrev"},     {VK_MEDIA_STOP, L"MediaStop"},
    {VK_LAUNCH_MAIL, L"LaunchMail"},         {VK_LAUNCH_MEDIA_SELECT, L"LaunchMedia"},
    {VK_LAUNCH_APP1, L"LaunchApp1"},         {VK_LAUNCH_APP2, L"LaunchApp2"},
    {VK_LCONTROL, L"LCtrl", L"左 Ctrl"},     {VK_RCONTROL, L"RCtrl", L"右 Ctrl"},
    {VK_LSHIFT, L"LShift", L"左 Shift"},     {VK_RSHIFT, L"RShift", L"右 Shift"},
    {VK_LMENU, L"LAlt", L"左 Alt"},          {VK_RMENU, L"RAlt", L"右 Alt"},
    {VK_LBUTTON, L"MouseLeft", L"鼠标左键"}, {VK_RBUTTON, L"MouseRight", L"鼠标右键"},
    {VK_MBUTTON, L"MouseMiddle", L"鼠标中键"}, {VK_XBUTTON1, L"MouseBack", L"鼠标后退键"},
    {VK_XBUTTON2, L"MouseForward", L"鼠标前进键"},
};

// 修饰键按显示的顺序
struct NamedMod {
    UINT mod;
    const wchar_t* name;
};
const NamedMod kNamedMods[] = {{MOD_CONTROL, L"Ctrl"}, {MOD_SHIFT, L"Shift"}, {MOD_ALT, L"Alt"}};

UINT s_registered[kHotkeyCount] = {};  // 现在用 RegisterHotKey 注册着的，0 = 没注册
bool s_failed[kHotkeyCount] = {};      // 设的那个注册不上
UINT s_warned[kHotkeyCount] = {};      // 已经弹气泡说过注册不上的
UINT s_logged[kHotkeyCount] = {};      // 日志里记过的（交给钩子线程的那些），免得每次都记
bool s_loggedHold[kHotkeyCount] = {};
bool s_suspended = false;

UINT Wanted(int id) {
    if (id == kHotkeyPin && !g_settings.keepFloatsOnTop) return 0;  // 没打开“小窗口留在上面”时固定了也没用
    return g_settings.hotkey[id];
}

// 不配 Ctrl、Alt 单按的话，打字、平常操作就会被它拿走的键
bool IsTypingKey(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || (vk >= VK_OEM_1 && vk <= VK_OEM_3) ||
        (vk >= VK_OEM_4 && vk <= VK_OEM_7) || (vk >= VK_PRIOR && vk <= VK_DOWN))
        return true;
    switch (vk) {
        case VK_SPACE: case VK_RETURN: case VK_TAB: case VK_BACK: case VK_DELETE: case VK_ESCAPE: return true;
    }
    return false;
}

// 单按的键盘按键用 RegisterHotKey；同一个键另一个快捷键要长按的话，得由钩子来分单按还是长按
bool UsesRegisterHotKey(int id, UINT want) {
    UINT vk = HotkeyVk(want);
    if (g_settings.hotkeyHold[id] || IsMouseVk(vk) || IsModifierVk(vk)) return false;
    if (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) return false;  // 钩子吞掉才不会切换大小写等
    for (int other = 0; other < kHotkeyCount; ++other)
        if (other != id && Wanted(other) == want && g_settings.hotkeyHold[other]) return false;
    return true;
}

const wchar_t* ActionName(int id) { return id == kHotkeyPopup ? L"弹出迷你任务栏" : L"固定小窗口"; }

}  // namespace

std::wstring KeyName(UINT vk, bool display) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
    for (const NamedKey& k : kNamedKeys)
        if (k.vk == vk) return display && k.display ? k.display : k.name;
    return {};
}

std::wstring HotkeyText(UINT hotkey, bool display) {
    std::wstring key = KeyName(HotkeyVk(hotkey), display);
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

const wchar_t* HotkeyProblem(UINT hotkey, bool hold) {
    UINT vk = HotkeyVk(hotkey), mods = HotkeyMods(hotkey);
    if (KeyName(vk).empty()) return L"这个键不能用，换一个";
    if (vk == VK_LBUTTON && !mods) return L"鼠标左键要配上 Ctrl、Shift 或 Alt";
    if (hold) return nullptr;
    if (vk == VK_RBUTTON && !mods) return L"单按右键的话右键菜单就打不开了，配上修饰键或改成长按";
    if (IsTypingKey(vk) && !(mods & (MOD_CONTROL | MOD_ALT))) return L"单按会打不了字，配上 Ctrl / Alt 或改成长按";
    if (vk == VK_F12 && !mods) return L"F12 单按留给系统了，配上修饰键或改成长按";
    return nullptr;
}

void Hotkeys_Apply() {
    std::vector<KeyBinding> bindings;
    for (int id = 0; id < kHotkeyCount; ++id) {
        UINT want = s_suspended ? 0 : Wanted(id);
        bool hold = g_settings.hotkeyHold[id];
        bool viaRegister = want && UsesRegisterHotKey(id, want);
        if (want && !viaRegister) {
            // 交给钩子线程；两个快捷键是同一个键（一个单按、一个长按）的合成一条
            auto same = std::find_if(bindings.begin(), bindings.end(), [&](const KeyBinding& b) {
                return b.mods == HotkeyMods(want) && b.vk == HotkeyVk(want);
            });
            if (same == bindings.end()) same = bindings.insert(bindings.end(), {HotkeyMods(want), HotkeyVk(want)});
            (hold ? same->hold : same->tap) = id;
            if (hold) same->holdMs = g_settings.hotkeyHoldMs[id];
            if (!s_suspended && (s_logged[id] != want || s_loggedHold[id] != hold)) {
                if (hold)
                    Log(L"快捷键 %ls（长按 %d 毫秒）：%ls", HotkeyText(want).c_str(), g_settings.hotkeyHoldMs[id], ActionName(id));
                else
                    Log(L"快捷键 %ls（单按）：%ls", HotkeyText(want).c_str(), ActionName(id));
                s_logged[id] = want;
                s_loggedHold[id] = hold;
            }
        } else if (!s_suspended) {
            s_logged[id] = 0;
        }

        UINT reg = viaRegister ? want : 0;
        if (reg == s_registered[id]) continue;
        if (s_registered[id]) UnregisterHotKey(g_mainWnd, kHotkeyIdBase + id);
        s_registered[id] = 0;
        if (!reg) {
            if (!s_suspended) s_failed[id] = false;
            continue;
        }
        if (RegisterHotKey(g_mainWnd, kHotkeyIdBase + id, HotkeyMods(reg) | MOD_NOREPEAT, HotkeyVk(reg))) {
            s_registered[id] = reg;
            s_failed[id] = false;
            Log(L"快捷键 %ls：%ls", HotkeyText(reg).c_str(), ActionName(id));
            continue;
        }
        s_failed[id] = true;
        Log(L"快捷键 %ls 注册不上（错误 %lu），可能被别的程序占用了", HotkeyText(reg).c_str(), GetLastError());
        if (s_warned[id] == reg) continue;
        std::wstring text = L"快捷键 " + HotkeyText(reg, true) + L" 被别的程序占用了，在设置里换一个吧。";
        if (ShowTrayBalloon(L"TaskbarPopup", text.c_str())) s_warned[id] = reg;  // 托盘图标还没加上时下次再说
    }
    Hook_SetBindings(bindings);
}

void Hotkeys_Suspend(bool suspend) {
    if (suspend == s_suspended) return;
    s_suspended = suspend;
    Hotkeys_Apply();
}

bool Hotkeys_Failed(int id) { return id >= 0 && id < kHotkeyCount && s_failed[id]; }

void Hotkeys_Run(int id, HWND target) {
    if (id == kHotkeyPopup) {
        Log(L"按了弹出迷你任务栏的快捷键");
        Popup_Toggle();
    } else if (id == kHotkeyPin) {
        FloatWindows_TogglePin(target);
    }
}

void Hotkeys_OnHotkey(WPARAM id) {
    if (id >= kHotkeyIdBase && id < kHotkeyIdBase + kHotkeyCount) Hotkeys_Run(static_cast<int>(id - kHotkeyIdBase), nullptr);
}

}  // namespace app
