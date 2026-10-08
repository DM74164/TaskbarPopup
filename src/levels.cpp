// 音量和亮度：迷你任务栏上两个滑块的后端。
//
// 音量：Core Audio 默认播放设备的主音量，在主线程上同步调用，很快。
// 亮度：外接显示器走 DDC/CI（dxva2），笔记本内屏走 WMI（ROOT\WMI 的 WmiMonitorBrightness）。
//       两种都要几十到几百毫秒，所以放在一个后台线程上做；拖动滑块时只执行最新的一次设置。
#include "common.h"

#include <endpointvolume.h>
#include <highlevelmonitorconfigurationapi.h>
#include <mmdeviceapi.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemcli.h>

#include <map>

namespace app {
namespace {

// GUID 自己定义一份，不依赖 uuid / wbemuuid 库里恰好有这些符号
const GUID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const GUID kIidMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const GUID kIidAudioEndpointVolume = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
const GUID kClsidWbemLocator = {0x4590F811, 0x1D3A, 0x11D0, {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
const GUID kIidWbemLocator = {0xDC12A687, 0x737F, 0x11CF, {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ================= 音量 =================
// 这些对象在主线程（STA）上创建，也只能在主线程上用
IMMDeviceEnumerator* s_devices = nullptr;
IAudioEndpointVolume* s_volume = nullptr;
std::wstring s_deviceId;  // s_volume 属于哪个设备；默认设备换了（比如插上耳机）就重建

void DropVolume() {
    SafeRelease(s_volume);
    SafeRelease(s_devices);
    s_deviceId.clear();
}

// recheck：重新问一次默认设备是谁（只读个 ID，很便宜）；否则有缓存就直接用
IAudioEndpointVolume* Endpoint(bool recheck) {
    if (s_volume && !recheck) return s_volume;
    if (!s_devices && FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
                                              kIidMMDeviceEnumerator, reinterpret_cast<void**>(&s_devices)))) {
        s_devices = nullptr;
        return nullptr;
    }
    IMMDevice* device = nullptr;
    if (FAILED(s_devices->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        // 没有可用的播放设备（全拔了或者都禁用了）
        SafeRelease(s_volume);
        s_deviceId.clear();
        return nullptr;
    }
    std::wstring id;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(device->GetId(&raw)) && raw) id = raw;
    CoTaskMemFree(raw);
    if (!s_volume || id.empty() || id != s_deviceId) {
        SafeRelease(s_volume);
        if (FAILED(device->Activate(kIidAudioEndpointVolume, CLSCTX_INPROC_SERVER, nullptr,
                                    reinterpret_cast<void**>(&s_volume))))
            s_volume = nullptr;
        s_deviceId = s_volume ? id : std::wstring();
    }
    device->Release();
    return s_volume;
}

// 对默认设备执行 op。设备被拔掉或禁用后，旧接口会返回 AUDCLNT_E_DEVICE_INVALIDATED 之类的错误：
// 丢掉缓存、重新找默认设备再试一次
template <class Op>
bool WithVolume(bool recheck, Op op) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        IAudioEndpointVolume* volume = Endpoint(recheck || attempt > 0);
        if (!volume) return false;
        if (SUCCEEDED(op(volume))) return true;
        DropVolume();
    }
    return false;
}

// ================= 亮度 =================
constexpr DWORD kStopWaitMs = 2000;
constexpr long kWmiTimeoutMs = 5000;
enum Method { kDdc = 1, kWmi = 2 };

// 主线程和后台线程共用的请求队列，用 lock 保护
struct Requests {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    bool quit = false;
    std::map<HMONITOR, int> sets;  // 每块屏幕只留最新的设置值，拖动时没来得及执行的旧值直接被覆盖
    std::vector<std::pair<HWND, HMONITOR>> queries;
};

// 这两个只在主线程上读写
Requests* s_requests = nullptr;
HANDLE s_thread = nullptr;

// 通过 DCOM 传给 WMI 服务的字符串必须是真正的 BSTR（带长度前缀）
class Bstr {
public:
    explicit Bstr(const wchar_t* s) : m_s(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(m_s); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return m_s; }

private:
    BSTR m_s;
};

bool GetInt(IWbemClassObject* obj, const wchar_t* name, int& value) {
    VARIANT v, i4;
    VariantInit(&v);
    VariantInit(&i4);
    // uint8 回来是 VT_UI1，uint32 是 VT_I4，统一转成 int
    bool ok = SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr)) && SUCCEEDED(VariantChangeType(&i4, &v, 0, VT_I4));
    if (ok) value = i4.lVal;
    VariantClear(&i4);
    VariantClear(&v);
    return ok;
}

bool GetString(IWbemClassObject* obj, const wchar_t* name, std::wstring& value) {
    VARIANT v;
    VariantInit(&v);
    bool ok = SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal;
    if (ok) value.assign(v.bstrVal, SysStringLen(v.bstrVal));
    VariantClear(&v);
    return ok;
}

bool PutInt(IWbemClassObject* obj, const wchar_t* name, VARTYPE type, int value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = type;
    if (type == VT_UI1)
        v.bVal = static_cast<BYTE>(value);
    else
        v.lVal = value;
    bool ok = SUCCEEDED(obj->Put(name, 0, &v, 0));
    VariantClear(&v);
    return ok;
}

// ---- WMI 实例和 HMONITOR 的对应 ----
// WMI 的 InstanceName 形如 DISPLAY\BOE0867\4&2d7d3e4b&0&UID8388688_0，
// 显示器的设备接口名形如 \\?\DISPLAY#BOE0867#4&2d7d3e4b&0&UID8388688#{e6f07b5f-...}，
// 把前者的 \ 换成 #、去掉结尾的 _0，就能在后者里找到
std::wstring InstanceKey(const std::wstring& instance) {
    std::wstring key = ToLower(instance);
    size_t us = key.find_last_of(L'_');
    if (us != std::wstring::npos && us + 1 < key.size() &&
        std::all_of(key.begin() + us + 1, key.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        key.resize(us);
    std::replace(key.begin(), key.end(), L'\\', L'#');
    return key + L'#';
}

std::vector<std::wstring> MonitorDeviceIds(HMONITOR monitor) {
    std::vector<std::wstring> ids;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return ids;
    // 复制模式下一个 HMONITOR 可能对应好几台显示器
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd = {};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(mi.szDevice, i, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) break;
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) ids.push_back(ToLower(dd.DeviceID));
    }
    return ids;
}

bool IdsContain(const std::vector<std::wstring>& ids, const std::wstring& key) {
    return std::any_of(ids.begin(), ids.end(), [&](const std::wstring& id) { return id.find(key) != std::wstring::npos; });
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    reinterpret_cast<std::vector<HMONITOR>*>(param)->push_back(monitor);
    return TRUE;
}

// 在 WMI 实例里找属于这块屏幕的那个，找不到返回 -1
int PickInstance(HMONITOR monitor, const std::vector<std::wstring>& names) {
    if (names.empty()) return -1;
    std::vector<std::wstring> keys;
    for (const auto& n : names) keys.push_back(InstanceKey(n));
    std::vector<std::wstring> ids = MonitorDeviceIds(monitor);
    for (size_t i = 0; i < keys.size(); ++i)
        if (IdsContain(ids, keys[i])) return static_cast<int>(i);

    // 名字对不上时退一步：只有一个实例、而且它也对不上任何一块屏幕（名字格式和上面预想的不同），
    // 就当它属于这块 DDC/CI 不通的屏幕。笔记本外接屏多半能走 DDC/CI，一般不会弄错；
    // 但如果外接屏也不支持 DDC/CI，这时拖它的滑块会改到内屏
    if (names.size() != 1) return -1;
    std::vector<HMONITOR> all;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&all));
    for (HMONITOR m : all)
        if (IdsContain(MonitorDeviceIds(m), keys[0])) return -1;
    return 0;
}

// ---- 后台线程上的亮度读写，所有成员只在后台线程里用 ----
class Worker {
public:
    explicit Worker(bool comReady) : m_comReady(comReady) {}
    ~Worker() { WmiReset(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // 返回 0~100，不支持返回 -1
    int Read(HMONITOR monitor) {
        int percent = -1;
        for (int pass = 0; pass < 2; ++pass) {
            bool wmi = (pass == 0) == WmiFirst(monitor);
            if (wmi ? WmiRetry([&] { return WmiRead(monitor, percent); }) : DdcRead(monitor, percent)) {
                m_method[monitor] = wmi ? kWmi : kDdc;
                return percent;
            }
        }
        m_method.erase(monitor);
        return -1;
    }

    void Write(HMONITOR monitor, int percent) {
        for (int pass = 0; pass < 2; ++pass) {
            bool wmi = (pass == 0) == WmiFirst(monitor);
            if (wmi ? WmiRetry([&] { return WmiWrite(monitor, percent); }) : DdcWrite(monitor, percent)) {
                m_method[monitor] = wmi ? kWmi : kDdc;
                return;
            }
        }
        m_method.erase(monitor);
    }

private:
    struct Range {
        DWORD min = 0, max = 0;  // max <= min 表示还不知道
    };
    struct WmiTarget {
        std::wstring name;  // InstanceName
        std::wstring path;  // __PATH，ExecMethod 用
    };

    // 上次在这块屏幕上管用的是 WMI 就先试 WMI，省掉一次注定失败的（慢的）DDC/CI；
    // 默认先试 DDC/CI，WMI 只在 DDC/CI 不通时才用
    bool WmiFirst(HMONITOR monitor) const {
        auto it = m_method.find(monitor);
        return it != m_method.end() && it->second == kWmi;
    }

    // ---- DDC/CI ----
    static bool OpenPhysical(HMONITOR monitor, std::vector<PHYSICAL_MONITOR>& list) {
        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) || count == 0) return false;
        list.resize(count);
        if (GetPhysicalMonitorsFromHMONITOR(monitor, count, list.data())) return true;
        list.clear();
        return false;
    }

    std::vector<Range>& Ranges(HMONITOR monitor, size_t count) {
        std::vector<Range>& ranges = m_ddcRanges[monitor];
        if (ranges.size() != count) ranges.assign(count, Range{});
        return ranges;
    }

    bool DdcRead(HMONITOR monitor, int& percent) {
        std::vector<PHYSICAL_MONITOR> list;
        if (!OpenPhysical(monitor, list)) return false;
        std::vector<Range>& ranges = Ranges(monitor, list.size());
        bool ok = false;
        for (size_t i = 0; i < list.size() && !ok; ++i) {  // 用第一台回答的
            DWORD lo = 0, cur = 0, hi = 0;
            if (!GetMonitorBrightness(list[i].hPhysicalMonitor, &lo, &cur, &hi) || hi <= lo) continue;
            ranges[i] = {lo, hi};
            cur = std::min(std::max(cur, lo), hi);
            percent = static_cast<int>(((cur - lo) * 100 + (hi - lo) / 2) / (hi - lo));
            ok = true;
        }
        DestroyPhysicalMonitors(static_cast<DWORD>(list.size()), list.data());
        return ok;
    }

    bool DdcWrite(HMONITOR monitor, int percent) {
        std::vector<PHYSICAL_MONITOR> list;
        if (!OpenPhysical(monitor, list)) return false;
        std::vector<Range>& ranges = Ranges(monitor, list.size());
        bool any = false;
        for (size_t i = 0; i < list.size(); ++i) {  // 复制模式下全都设
            HANDLE h = list[i].hPhysicalMonitor;
            Range& r = ranges[i];
            // 范围记下来，拖动时每次设置就不用先读一遍（DDC/CI 每条命令都要几十毫秒）
            if (r.max <= r.min) {
                DWORD cur = 0;
                if (!GetMonitorBrightness(h, &r.min, &cur, &r.max) || r.max <= r.min) {
                    r = Range{};
                    continue;
                }
            }
            DWORD value = r.min + (static_cast<DWORD>(percent) * (r.max - r.min) + 50) / 100;
            if (SetMonitorBrightness(h, value))
                any = true;
            else
                r = Range{};  // 下次重新读范围
        }
        DestroyPhysicalMonitors(static_cast<DWORD>(list.size()), list.data());
        return any;
    }

    // ---- WMI ----
    // WMI 一般只有笔记本内屏（也可能是一体机）有亮度实例，外接显示器得靠 DDC/CI

    // 缓存的连接失效（WMI 服务重启过）时调用会失败并丢掉连接：重新连上再试一次，
    // 不然这块屏幕会被当成不支持调亮度
    template <class Fn>
    bool WmiRetry(Fn fn) {
        bool had = m_wmi != nullptr;
        if (fn()) return true;
        return had && !m_wmi && fn();
    }

    bool WmiConnect() {
        if (m_wmi) return true;
        if (!m_comReady) return false;
        IWbemLocator* locator = nullptr;
        if (FAILED(CoCreateInstance(kClsidWbemLocator, nullptr, CLSCTX_INPROC_SERVER, kIidWbemLocator,
                                    reinterpret_cast<void**>(&locator))))
            return false;
        Bstr ns(L"ROOT\\WMI");
        HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &m_wmi);
        locator->Release();
        if (FAILED(hr)) {
            m_wmi = nullptr;
            return false;
        }
        hr = CoSetProxyBlanket(m_wmi, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                               RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        if (FAILED(hr)) {
            WmiReset();
            return false;
        }
        return true;
    }

    void WmiResetSet() {
        SafeRelease(m_setParams);
        m_targets.clear();
    }

    // 调用失败多半是 WMI 服务重启或屏幕变了：全部丢掉，下次重新连接
    void WmiReset() {
        WmiResetSet();
        SafeRelease(m_wmi);
    }

    // 执行查询，把每个结果交给 fn
    template <class Fn>
    bool WmiQuery(const wchar_t* query, Fn fn) {
        Bstr language(L"WQL"), text(query);
        IEnumWbemClassObject* results = nullptr;
        HRESULT hr = m_wmi->ExecQuery(language, text, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                                      &results);
        if (FAILED(hr) || !results) {
            // WBEM_E_xxx（台式机上没有这个类之类）说明连接本身没问题；RPC 断开等其他错误才重新连接
            if (HRESULT_FACILITY(hr) != FACILITY_ITF) WmiReset();
            return false;
        }
        for (;;) {
            IWbemClassObject* obj = nullptr;
            ULONG got = 0;
            // 半同步查询，上面那种 WBEM_E_NOT_SUPPORTED 也可能到这里才返回
            hr = results->Next(kWmiTimeoutMs, 1, &obj, &got);
            if (FAILED(hr) || got == 0 || !obj) break;
            fn(obj);
            obj->Release();
        }
        results->Release();
        return true;
    }

    bool WmiRead(HMONITOR monitor, int& percent) {
        if (!WmiConnect()) return false;
        std::vector<std::wstring> names;
        std::vector<int> values;
        bool ok = WmiQuery(L"SELECT CurrentBrightness, InstanceName FROM WmiMonitorBrightness WHERE Active=TRUE",
                           [&](IWbemClassObject* obj) {
                               std::wstring name;
                               int value = 0;
                               if (GetString(obj, L"InstanceName", name) && GetInt(obj, L"CurrentBrightness", value)) {
                                   names.push_back(name);
                                   values.push_back(value);
                               }
                           });
        if (!ok) return false;
        int i = PickInstance(monitor, names);
        if (i < 0) return false;
        percent = std::clamp(values[i], 0, 100);
        return true;
    }

    // WmiSetBrightness 的参数定义和各个实例的路径，拖动时每次设置都要用，缓存起来
    bool WmiPrepareSet() {
        if (m_setParams && !m_targets.empty()) return true;
        WmiResetSet();
        IWbemClassObject* cls = nullptr;
        Bstr className(L"WmiMonitorBrightnessMethods");
        HRESULT got = m_wmi->GetObject(className, 0, nullptr, &cls, nullptr);
        if (FAILED(got) || !cls) {
            if (FAILED(got) && HRESULT_FACILITY(got) != FACILITY_ITF) WmiReset();  // 同 WmiQuery
            return false;
        }
        HRESULT hr = cls->GetMethod(L"WmiSetBrightness", 0, &m_setParams, nullptr);
        cls->Release();
        if (FAILED(hr) || !m_setParams) {
            m_setParams = nullptr;
            return false;
        }
        bool ok = WmiQuery(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE", [&](IWbemClassObject* obj) {
            WmiTarget t;
            if (GetString(obj, L"InstanceName", t.name) && GetString(obj, L"__PATH", t.path)) m_targets.push_back(t);
        });
        if (!ok || m_targets.empty()) {
            WmiResetSet();
            return false;
        }
        return true;
    }

    bool WmiWrite(HMONITOR monitor, int percent) {
        if (!WmiConnect() || !WmiPrepareSet()) return false;
        std::vector<std::wstring> names;
        for (const auto& t : m_targets) names.push_back(t.name);
        int i = PickInstance(monitor, names);
        if (i < 0) {
            WmiResetSet();  // 屏幕可能刚接上 / 拔掉，下次重新枚举实例
            return false;
        }
        IWbemClassObject* in = nullptr;
        if (FAILED(m_setParams->SpawnInstance(0, &in)) || !in) {
            WmiReset();
            return false;
        }
        // Timeout 是 uint32，按惯例用 VT_I4；Brightness 是 uint8，先按 VT_UI1 放，不接受再试 VT_I4
        bool ok = PutInt(in, L"Timeout", VT_I4, 1) &&
                  (PutInt(in, L"Brightness", VT_UI1, percent) || PutInt(in, L"Brightness", VT_I4, percent));
        if (ok) {
            Bstr path(m_targets[i].path.c_str()), method(L"WmiSetBrightness");
            ok = SUCCEEDED(m_wmi->ExecMethod(path, method, 0, nullptr, in, nullptr, nullptr));
            if (!ok) WmiReset();
        }
        in->Release();
        return ok;
    }

    bool m_comReady;
    std::map<HMONITOR, int> m_method;  // 每块屏幕上哪种方式管用，失败了就删掉重新试
    std::map<HMONITOR, std::vector<Range>> m_ddcRanges;
    IWbemServices* m_wmi = nullptr;
    IWbemClassObject* m_setParams = nullptr;
    std::vector<WmiTarget> m_targets;
};

DWORD WINAPI BrightnessThread(LPVOID param) {
    auto* requests = static_cast<Requests*>(param);
    // 用自己的 MTA，不和主线程的 STA 抢；DDC/CI 不需要 COM，COM 起不来也照样能用
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Worker worker(SUCCEEDED(com));
        for (;;) {
            AcquireSRWLockExclusive(&requests->lock);
            while (!requests->quit && requests->sets.empty() && requests->queries.empty())
                SleepConditionVariableSRW(&requests->wake, &requests->lock, INFINITE, 0);
            if (requests->quit) {
                ReleaseSRWLockExclusive(&requests->lock);
                break;
            }
            // 先设置后查询：拖动完马上查询时，报回去的是刚设置的新值
            if (!requests->sets.empty()) {
                auto it = requests->sets.begin();
                HMONITOR monitor = it->first;
                int percent = it->second;
                requests->sets.erase(it);
                ReleaseSRWLockExclusive(&requests->lock);
                worker.Write(monitor, percent);
                continue;
            }
            auto query = requests->queries.front();
            requests->queries.erase(requests->queries.begin());
            ReleaseSRWLockExclusive(&requests->lock);
            int percent = worker.Read(query.second);
            // 读的时候用户又调过了：读到的是旧值，别拿它把调节条拨回去（排着的设置马上就会生效）
            AcquireSRWLockExclusive(&requests->lock);
            bool stale = percent >= 0 && requests->sets.count(query.second) != 0;
            ReleaseSRWLockExclusive(&requests->lock);
            if (!stale && IsWindow(query.first))
                PostMessageW(query.first, WM_APP_BRIGHTNESS, static_cast<WPARAM>(static_cast<INT_PTR>(percent)),
                             reinterpret_cast<LPARAM>(query.second));
        }
    }  // Worker 在 CoUninitialize 之前释放 WMI 对象
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

Requests* EnsureThread() {
    if (s_thread) return s_requests;
    auto* requests = new Requests;
    HANDLE thread = CreateThread(nullptr, 0, BrightnessThread, requests, 0, nullptr);
    if (!thread) {
        delete requests;
        return nullptr;
    }
    s_thread = thread;
    s_requests = requests;
    return requests;
}

}  // namespace

bool Volume_Get(float& level, bool& muted) {
    float value = 0;
    BOOL mute = FALSE;
    bool ok = WithVolume(true, [&](IAudioEndpointVolume* v) {
        HRESULT hr = v->GetMasterVolumeLevelScalar(&value);
        if (SUCCEEDED(hr)) hr = v->GetMute(&mute);
        return hr;
    });
    if (!ok) return false;
    level = std::clamp(value, 0.0f, 1.0f);
    muted = mute != FALSE;
    return true;
}

void Volume_Set(float level) {
    if (!(level >= 0.0f)) level = 0.0f;  // 顺便挡住 NaN
    if (level > 1.0f) level = 1.0f;
    WithVolume(false, [&](IAudioEndpointVolume* v) {
        HRESULT hr = v->SetMasterVolumeLevelScalar(level, nullptr);
        BOOL mute = FALSE;
        if (SUCCEEDED(hr) && SUCCEEDED(v->GetMute(&mute)) && mute) hr = v->SetMute(FALSE, nullptr);
        return hr;
    });
}

void Volume_SetMute(bool muted) {
    WithVolume(false, [&](IAudioEndpointVolume* v) { return v->SetMute(muted ? TRUE : FALSE, nullptr); });
}

void Volume_Release() { DropVolume(); }

void Brightness_Query(HWND notify, HMONITOR monitor) {
    Requests* r = EnsureThread();
    if (!r) {
        if (IsWindow(notify))
            PostMessageW(notify, WM_APP_BRIGHTNESS, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(monitor));
        return;
    }
    auto query = std::make_pair(notify, monitor);
    AcquireSRWLockExclusive(&r->lock);
    if (std::find(r->queries.begin(), r->queries.end(), query) == r->queries.end()) r->queries.push_back(query);
    ReleaseSRWLockExclusive(&r->lock);
    WakeConditionVariable(&r->wake);
}

void Brightness_Set(HMONITOR monitor, int percent) {
    Requests* r = EnsureThread();
    if (!r) return;
    AcquireSRWLockExclusive(&r->lock);
    r->sets[monitor] = std::clamp(percent, 0, 100);
    ReleaseSRWLockExclusive(&r->lock);
    WakeConditionVariable(&r->wake);
}

void Brightness_Stop() {
    if (!s_thread) return;
    AcquireSRWLockExclusive(&s_requests->lock);
    s_requests->quit = true;  // 还没执行的设置和查询都不要了
    ReleaseSRWLockExclusive(&s_requests->lock);
    WakeConditionVariable(&s_requests->wake);
    // 后台线程可能正卡在一次很慢的 DDC/CI 或 WMI 调用里。最多等 2 秒，等不到就不管它了：
    // 它做完手上这次就会退出，或者随进程一起被结束。这时它还会用 s_requests，所以不能释放
    // （故意漏掉这一小块）；也不能 TerminateThread，那可能留下没释放的锁
    if (WaitForSingleObject(s_thread, kStopWaitMs) == WAIT_OBJECT_0) delete s_requests;
    CloseHandle(s_thread);
    s_thread = nullptr;
    s_requests = nullptr;
}

}  // namespace app
