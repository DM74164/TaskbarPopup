// Wi-Fi 和蓝牙开关：走 WinRT 的 Windows.Devices.Radios（和系统“快速设置”里的开关一样）。
// MinGW 没有这个 WinRT 头文件，下面按 ABI 自己声明用到的几个接口；combase 里的函数动态加载。
// 读和切换都慢（切换要等网卡开关好），放在后台线程上做。
// Wi-Fi 连着的网络名从网络列表管理器里拿（WLAN 接口读 SSID 在新版 Win11 上要定位权限，会弹提示）。
#include <winsock2.h>
#include <iphlpapi.h>
#include <inspectable.h>
#include <netlistmgr.h>

#include "common.h"

namespace app {

#ifndef TP_FAKE_RADIOS
// WinRT 接口（按 Windows SDK 里 windows.devices.radios.h 的顺序）。不能放进匿名命名空间：那样 GCC 认定没有别的类
// 实现它们，会把对它们的虚函数调用直接换成 __cxa_pure_virtual，一调用程序就退出
namespace radio_abi {

struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// IAsyncOperation<T>：不注册完成回调（那要算参数化接口的 IID），后台线程上轮询 IAsyncInfo 的状态
template <typename T>
struct IAsyncOperationAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T* results) = 0;
};

template <typename T>
struct IVectorViewAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, T* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
};

struct IRadioAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetStateAsync(int state, IAsyncOperationAbi<int>** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_StateChanged(void* handler, INT64* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_StateChanged(INT64 token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(int* state) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING* name) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Kind(int* kind) = 0;
};

using RadioList = IVectorViewAbi<IRadioAbi*>;

struct IRadioStaticsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetRadiosAsync(IAsyncOperationAbi<RadioList*>** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceSelector(HSTRING* selector) = 0;
    virtual HRESULT STDMETHODCALLTYPE FromIdAsync(HSTRING id, void** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(IAsyncOperationAbi<int>** operation) = 0;
};

}  // namespace radio_abi
#endif

namespace {

#ifndef TP_FAKE_RADIOS
using namespace radio_abi;
#endif

constexpr DWORD kQueryWaitMs = 5000;
constexpr DWORD kSwitchWaitMs = 10000;
constexpr DWORD kStopWaitMs = 2000;

// ---- 后台线程和主线程共用的状态 ----

struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    bool quit = false;
    bool query = false;                     // 要重新读一遍
    HWND notify = nullptr;                  // 做完了通知谁
    int target[kRadioCount] = {-1, -1};     // 要切成的样子：-1 = 不切，0 关，1 开；切完、读完才清掉
    bool fresh[kRadioCount] = {};           // target 是新给的，后台线程还没拿走
    RadioInfo info[kRadioCount];            // 最近读到的
};

// 这两个只在主线程上读写
Shared* s_shared = nullptr;
HANDLE s_thread = nullptr;

#ifdef TP_FAKE_RADIOS
// 测试时可以用 -DTP_FAKE_RADIOS 假装有 Wi-Fi 和蓝牙（在没有这些设备的环境里看界面）：
// Wi-Fi 开着、连着 Home，蓝牙关着，切换要等 0.6 秒
class Backend {
public:
    explicit Backend(bool) {}
    void Read(int status[kRadioCount], std::wstring& network) {
        for (int i = 0; i < kRadioCount; ++i) status[i] = m_on[i] ? kRadioOn : kRadioOff;
        network = m_on[kRadioWifi] ? L"Home" : L"";
    }
    bool Switch(int radio, bool on) {
        Sleep(600);
        m_on[radio] = on;
        return true;
    }

private:
    bool m_on[kRadioCount] = {true, false};
};
#else
// ---- WinRT ----

// RadioState / RadioKind / RadioAccessStatus 都是 32 位枚举，这里直接用 int
enum { kStateUnknown, kStateOn, kStateOff, kStateDisabled };
enum { kKindOther, kKindWiFi, kKindMobileBroadband, kKindBluetooth, kKindFM };
enum { kAccessUnspecified, kAccessAllowed, kAccessDeniedByUser, kAccessDeniedBySystem };
enum { kAsyncStarted, kAsyncCompleted, kAsyncCanceled, kAsyncError };  // AsyncStatus

const IID kIidAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidRadioStatics = {0x5FB6A12E, 0x67CB, 0x46AE, {0xAA, 0xE9, 0x65, 0x91, 0x9F, 0x86, 0xEF, 0xF4}};
const CLSID kClsidNetworkListManager = {0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

template <typename T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// 等异步操作做完：做完了返回 true，出错、被取消或者超时返回 false
bool Await(IInspectable* operation, DWORD timeoutMs) {
    IAsyncInfoAbi* info = nullptr;
    if (FAILED(operation->QueryInterface(kIidAsyncInfo, reinterpret_cast<void**>(&info)))) return false;
    DWORD start = GetTickCount();
    int status = kAsyncStarted;
    while (SUCCEEDED(info->get_Status(&status)) && status == kAsyncStarted && GetTickCount() - start < timeoutMs)
        Sleep(15);
    if (status == kAsyncStarted) info->Cancel();
    info->Release();
    return status == kAsyncCompleted;
}

int KindOf(int radio) { return radio == kRadioWifi ? kKindWiFi : kKindBluetooth; }

class Backend {
public:
    explicit Backend(bool comReady) {
        if (!comReady) return;
        HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!combase) return;
        using GetFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
        using CreateStringFn = HRESULT(WINAPI*)(LPCWSTR, UINT32, HSTRING*);
        using DeleteStringFn = HRESULT(WINAPI*)(HSTRING);
        auto getFactory = reinterpret_cast<GetFactoryFn>(
            reinterpret_cast<void*>(GetProcAddress(combase, "RoGetActivationFactory")));
        auto createString = reinterpret_cast<CreateStringFn>(
            reinterpret_cast<void*>(GetProcAddress(combase, "WindowsCreateString")));
        auto deleteString = reinterpret_cast<DeleteStringFn>(
            reinterpret_cast<void*>(GetProcAddress(combase, "WindowsDeleteString")));
        if (!getFactory || !createString || !deleteString) return;
        const wchar_t kClass[] = L"Windows.Devices.Radios.Radio";
        HSTRING name = nullptr;
        if (FAILED(createString(kClass, static_cast<UINT32>(wcslen(kClass)), &name))) return;
        HRESULT hr = getFactory(name, kIidRadioStatics, reinterpret_cast<void**>(&m_statics));
        deleteString(name);
        if (FAILED(hr)) {
            m_statics = nullptr;
            Log(L"Wi-Fi 和蓝牙：拿不到 Windows.Devices.Radios（0x%08X）", static_cast<unsigned>(hr));
        }
    }
    ~Backend() {
        for (IRadioAbi* r : m_radios) r->Release();
        SafeRelease(m_statics);
    }
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // 每个开关：有开着的就算开着，有关着的就算关着，都被禁用（硬件开关、设备管理器）算不可用
    void Read(int status[kRadioCount], std::wstring& network) {
        network.clear();
        if (!Enumerate()) {
            for (int i = 0; i < kRadioCount; ++i) status[i] = kRadioUnavailable;
            return;
        }
        for (int i = 0; i < kRadioCount; ++i) {
            bool any = false, on = false, off = false;
            for (IRadioAbi* r : m_radios) {
                int kind = kKindOther, state = kStateUnknown;
                if (FAILED(r->get_Kind(&kind)) || kind != KindOf(i)) continue;
                any = true;
                if (FAILED(r->get_State(&state))) continue;
                on = on || state == kStateOn;
                off = off || state == kStateOff;
            }
            status[i] = !any ? kRadioMissing : on ? kRadioOn : off ? kRadioOff : kRadioUnavailable;
        }
        if (status[kRadioWifi] == kRadioOn) network = WifiNetwork();
    }

    // 把这一类的设备都切过去（被禁用的不管），都成功了返回 true
    bool Switch(int radio, bool on) {
        if (!RequestAccess() || !Enumerate()) return false;
        bool any = false, ok = true;
        for (IRadioAbi* r : m_radios) {
            int kind = kKindOther, state = kStateUnknown;
            if (FAILED(r->get_Kind(&kind)) || kind != KindOf(radio)) continue;
            if (SUCCEEDED(r->get_State(&state)) && state == kStateDisabled) continue;
            any = true;
            IAsyncOperationAbi<int>* op = nullptr;
            int access = kAccessUnspecified;
            HRESULT hr = r->SetStateAsync(on ? kStateOn : kStateOff, &op);
            if (SUCCEEDED(hr) && op && Await(op, kSwitchWaitMs)) hr = op->GetResults(&access);
            else if (SUCCEEDED(hr)) hr = E_FAIL;
            SafeRelease(op);
            if (FAILED(hr) || access != kAccessAllowed) {
                Log(L"Wi-Fi 和蓝牙：切换%ls失败（0x%08X，access %d）", radio == kRadioWifi ? L" Wi-Fi " : L"蓝牙",
                    static_cast<unsigned>(hr), access);
                ok = false;
            }
        }
        return any && ok;
    }

private:
    // 重新列一遍设备（插拔 USB 网卡、蓝牙适配器以后会变）
    bool Enumerate() {
        if (!m_statics) return false;
        IAsyncOperationAbi<RadioList*>* op = nullptr;
        RadioList* list = nullptr;
        HRESULT hr = m_statics->GetRadiosAsync(&op);
        if (SUCCEEDED(hr) && op && Await(op, kQueryWaitMs)) hr = op->GetResults(&list);
        else if (SUCCEEDED(hr)) hr = E_FAIL;
        SafeRelease(op);
        if (FAILED(hr) || !list) {
            Log(L"Wi-Fi 和蓝牙：列设备失败（0x%08X）", static_cast<unsigned>(hr));
            SafeRelease(list);
            return false;
        }
        for (IRadioAbi* r : m_radios) r->Release();
        m_radios.clear();
        UINT32 count = 0;
        list->get_Size(&count);
        for (UINT32 i = 0; i < count; ++i) {
            IRadioAbi* r = nullptr;
            if (SUCCEEDED(list->GetAt(i, &r)) && r) m_radios.push_back(r);
        }
        list->Release();
        return true;
    }

    // 桌面程序一般直接就是允许的；问过一次允许了就不再问
    bool RequestAccess() {
        if (m_allowed) return true;
        if (!m_statics) return false;
        IAsyncOperationAbi<int>* op = nullptr;
        int access = kAccessUnspecified;
        HRESULT hr = m_statics->RequestAccessAsync(&op);
        if (SUCCEEDED(hr) && op && Await(op, kQueryWaitMs)) hr = op->GetResults(&access);
        SafeRelease(op);
        m_allowed = SUCCEEDED(hr) && access == kAccessAllowed;
        if (!m_allowed) Log(L"Wi-Fi 和蓝牙：系统不允许切换（0x%08X，access %d）", static_cast<unsigned>(hr), access);
        return m_allowed;
    }

    // Wi-Fi 连着的网络名：网络列表管理器里连着的、走无线网卡的那个连接所在的网络
    static std::wstring WifiNetwork() {
        std::vector<GUID> adapters;
        std::vector<BYTE> buffer;
        ULONG size = 16 * 1024;
        ULONG result = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; ++tries) {
            buffer.resize(size);
            result = GetAdaptersAddresses(AF_UNSPEC,
                                          GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }
        if (result != ERROR_SUCCESS) return L"";
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
            if (a->IfType != IF_TYPE_IEEE80211 || a->OperStatus != IfOperStatusUp || !a->AdapterName) continue;
            wchar_t text[64];
            GUID id;
            if (MultiByteToWideChar(CP_ACP, 0, a->AdapterName, -1, text, 64) && SUCCEEDED(CLSIDFromString(text, &id)))
                adapters.push_back(id);
        }
        if (adapters.empty()) return L"";

        std::wstring name;
        INetworkListManager* nlm = nullptr;
        IEnumNetworkConnections* connections = nullptr;
        if (SUCCEEDED(CoCreateInstance(kClsidNetworkListManager, nullptr, CLSCTX_ALL, __uuidof(INetworkListManager),
                                       reinterpret_cast<void**>(&nlm))) &&
            SUCCEEDED(nlm->GetNetworkConnections(&connections)) && connections) {
            INetworkConnection* c = nullptr;
            ULONG fetched = 0;
            while (name.empty() && connections->Next(1, &c, &fetched) == S_OK && c) {
                VARIANT_BOOL connected = VARIANT_FALSE;
                GUID adapter;
                INetwork* network = nullptr;
                BSTR text = nullptr;
                if (SUCCEEDED(c->get_IsConnected(&connected)) && connected && SUCCEEDED(c->GetAdapterId(&adapter)) &&
                    std::find(adapters.begin(), adapters.end(), adapter) != adapters.end() &&
                    SUCCEEDED(c->GetNetwork(&network)) && network && SUCCEEDED(network->GetName(&text)) && text)
                    name = text;
                SysFreeString(text);
                SafeRelease(network);
                SafeRelease(c);
            }
        }
        SafeRelease(connections);
        SafeRelease(nlm);
        return name;
    }

    IRadioStaticsAbi* m_statics = nullptr;
    std::vector<IRadioAbi*> m_radios;
    bool m_allowed = false;
};
#endif

DWORD WINAPI RadiosThread(LPVOID param) {
    auto* shared = static_cast<Shared*>(param);
    // 用自己的 MTA：WinRT 的异步操作在这里轮询，不卡主线程
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Backend backend(SUCCEEDED(com));
        for (;;) {
            AcquireSRWLockExclusive(&shared->lock);
            auto pending = [&] {
                for (int i = 0; i < kRadioCount; ++i)
                    if (shared->fresh[i]) return i;
                return -1;
            };
            while (!shared->quit && !shared->query && pending() < 0)
                SleepConditionVariableSRW(&shared->wake, &shared->lock, INFINITE, 0);
            if (shared->quit) {
                ReleaseSRWLockExclusive(&shared->lock);
                break;
            }
            // 先切换后读：切完马上读，报回去的是切换以后的样子
            int radio = pending();
            int target = radio >= 0 ? shared->target[radio] : -1;
            if (radio >= 0) shared->fresh[radio] = false;
            shared->query = false;  // 切完也会读一遍
            ReleaseSRWLockExclusive(&shared->lock);

            bool failed = radio >= 0 && !backend.Switch(radio, target == 1);
            int status[kRadioCount];
            std::wstring network;
            backend.Read(status, network);

            AcquireSRWLockExclusive(&shared->lock);
            for (int i = 0; i < kRadioCount; ++i) {
                shared->info[i].status = status[i];
                shared->info[i].network = i == kRadioWifi ? network : L"";
            }
            // 切的时候又点了一下（target 又是新的）：先别清，接着切
            if (radio >= 0 && !shared->fresh[radio]) shared->target[radio] = -1;
            HWND notify = shared->notify;
            ReleaseSRWLockExclusive(&shared->lock);
            if (notify && IsWindow(notify))
                PostMessageW(notify, WM_APP_RADIOS, failed ? (1u << radio) : 0, 0);
        }
    }  // Backend 在 CoUninitialize 之前释放 WinRT 对象
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

Shared* EnsureThread() {
    if (s_thread) return s_shared;
    auto* shared = new Shared;
    HANDLE thread = CreateThread(nullptr, 0, RadiosThread, shared, 0, nullptr);
    if (!thread) {
        delete shared;
        return nullptr;
    }
    s_thread = thread;
    s_shared = shared;
    return shared;
}

}  // namespace

void Radios_Query(HWND notify) {
    Shared* s = EnsureThread();
    if (!s) return;
    AcquireSRWLockExclusive(&s->lock);
    s->notify = notify;
    s->query = true;
    ReleaseSRWLockExclusive(&s->lock);
    WakeConditionVariable(&s->wake);
}

void Radios_Set(int radio, bool on, HWND notify) {
    if (radio < 0 || radio >= kRadioCount) return;
    Shared* s = EnsureThread();
    if (!s) return;
    AcquireSRWLockExclusive(&s->lock);
    s->notify = notify;
    s->target[radio] = on ? 1 : 0;
    s->fresh[radio] = true;
    ReleaseSRWLockExclusive(&s->lock);
    WakeConditionVariable(&s->wake);
}

RadioInfo Radios_Get(int radio) {
    RadioInfo info;
    if (radio < 0 || radio >= kRadioCount || !s_shared) return info;
    AcquireSRWLockShared(&s_shared->lock);
    info = s_shared->info[radio];
    if (s_shared->target[radio] >= 0) {
        info.status = s_shared->target[radio] ? kRadioOn : kRadioOff;
        info.switching = true;
    }
    ReleaseSRWLockShared(&s_shared->lock);
    return info;
}

void Radios_Stop() {
    if (!s_thread) return;
    AcquireSRWLockExclusive(&s_shared->lock);
    s_shared->quit = true;
    ReleaseSRWLockExclusive(&s_shared->lock);
    WakeConditionVariable(&s_shared->wake);
    // 后台线程可能正在等网卡开关。最多等 2 秒，等不到就不管它了（和亮度一样，故意漏掉 s_shared 这一小块）
    if (WaitForSingleObject(s_thread, kStopWaitMs) == WAIT_OBJECT_0) delete s_shared;
    CloseHandle(s_thread);
    s_thread = nullptr;
    s_shared = nullptr;
}

}  // namespace app
