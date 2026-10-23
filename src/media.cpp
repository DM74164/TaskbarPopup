// 正在播放的媒体：走 WinRT 的 Windows.Media.Control（系统音量弹窗里的媒体控制用的就是它，Win10 1809 起才有）。
// MinGW 没有这个 WinRT 头文件，下面按 ABI 自己声明用到的几个接口；combase 里的函数动态加载。
// 读和发命令都放在后台线程上做，打开着这个功能时每隔几秒读一遍，弹出迷你任务栏时就知道有没有在放。
// 封面换歌时才读一次，在后台解码、裁成正方形缩小好
#include <inspectable.h>

#include "common.h"

namespace app {

#ifndef TP_FAKE_MEDIA
// WinRT 接口（按 Windows SDK 里 windows.media.control.h 的顺序，用不到的方法也要占位）。
// 不能放进匿名命名空间，原因见 radios.cpp
namespace media_abi {

struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

template <typename T>
struct IAsyncOperationAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T* results) = 0;
};

using BoolOperation = IAsyncOperationAbi<unsigned char>;  // WinRT 的 Boolean 是一个字节

// Windows.Storage.Streams.IRandomAccessStreamReference：打开得到 IRandomAccessStreamWithContentType
struct IStreamReferenceAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IAsyncOperationAbi<IInspectable*>** op) = 0;
};

struct IMediaPropertiesAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(INT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(void** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(INT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(void** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IStreamReferenceAbi** value) = 0;
};

struct IPlaybackControlsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayPauseToggleEnabled(unsigned char* value) = 0;
};

struct IPlaybackInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IPlaybackControlsAbi** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int* value) = 0;
};

struct ISessionAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IAsyncOperationAbi<IMediaPropertiesAbi*>** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(void** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IPlaybackInfoAbi** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(BoolOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(BoolOperation** op) = 0;
};

struct ISessionManagerAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(ISessionAbi** session) = 0;
};

struct ISessionManagerStaticsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IAsyncOperationAbi<ISessionManagerAbi*>** op) = 0;
};

}  // namespace media_abi
#endif

namespace {

#ifndef TP_FAKE_MEDIA
using namespace media_abi;
#endif

constexpr DWORD kPollMs = 4000;      // 打开着这个功能时多久读一遍
constexpr DWORD kWaitMs = 3000;      // 等一个异步操作最多等这么久
constexpr DWORD kSettleMs = 250;     // 发完命令等播放器改好状态再读
constexpr DWORD kStopWaitMs = 2000;
constexpr int kCoverPx = 128;        // 封面缩成这么大（迷你任务栏上再按缩放缩小）
constexpr int kCoverTries = 3;       // 有的播放器换歌以后过一会儿才给封面：没读到的再读几遍

#ifndef TP_FAKE_MEDIA
// PlaybackStatus
enum { kStatusClosed, kStatusOpened, kStatusChanging, kStatusStopped, kStatusPlaying, kStatusPaused };
enum { kAsyncStarted, kAsyncCompleted, kAsyncCanceled, kAsyncError };

const IID kIidAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidManagerStatics = {0x2050C4EE, 0x11A0, 0x57DE, {0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45}};
#endif

struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    bool quit = false;
    bool poll = false;     // 每隔 kPollMs 读一遍
    bool query = false;    // 要马上读一遍
    int command = -1;      // 要发的命令（MediaCommand），-1 = 没有
    HWND notify = nullptr;
    MediaInfo info;
};

Shared* s_shared = nullptr;
HANDLE s_thread = nullptr;

bool SameInfo(const MediaInfo& a, const MediaInfo& b) {
    return a.present == b.present && a.playing == b.playing && a.canPrevious == b.canPrevious && a.canNext == b.canNext &&
           a.canPlayPause == b.canPlayPause && a.title == b.title && a.artist == b.artist && a.cover == b.cover;
}

// 封面裁成正方形（宽屏的视频截图取中间）、缩成 kCoverPx 见方，自己持有像素（从流解出来的位图要一直拿着流）
std::shared_ptr<Gdiplus::Bitmap> CoverFrom(Gdiplus::Bitmap& decoded) {
    UINT w = decoded.GetWidth(), h = decoded.GetHeight();
    if (decoded.GetLastStatus() != Gdiplus::Ok || !w || !h) return nullptr;
    UINT side = std::min(w, h);
    auto cover = std::make_shared<Gdiplus::Bitmap>(kCoverPx, kCoverPx, PixelFormat32bppPARGB);
    if (cover->GetLastStatus() != Gdiplus::Ok) return nullptr;
    Gdiplus::Graphics g(cover.get());
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::ImageAttributes edges;
    edges.SetWrapMode(Gdiplus::WrapModeTileFlipXY);  // 缩小时边上不混进透明
    g.DrawImage(&decoded, Gdiplus::Rect(0, 0, kCoverPx, kCoverPx), static_cast<INT>((w - side) / 2),
                static_cast<INT>((h - side) / 2), static_cast<INT>(side), static_cast<INT>(side), Gdiplus::UnitPixel,
                &edges);
    return cover;
}

#ifndef TP_FAKE_MEDIA
template <typename T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool Await(IInspectable* operation, DWORD timeoutMs) {
    IAsyncInfoAbi* info = nullptr;
    if (FAILED(operation->QueryInterface(kIidAsyncInfo, reinterpret_cast<void**>(&info)))) return false;
    DWORD start = GetTickCount();
    int status = kAsyncStarted;
    while (SUCCEEDED(info->get_Status(&status)) && status == kAsyncStarted && GetTickCount() - start < timeoutMs) Sleep(10);
    if (status == kAsyncStarted) info->Cancel();
    info->Release();
    return status == kAsyncCompleted;
}
#endif

// 没有这套接口，或者发命令失败时：按键盘上的媒体键
void SendMediaKey(int command) {
    WORD vk = command == kMediaPrevious ? VK_MEDIA_PREV_TRACK : command == kMediaNext ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PLAY_PAUSE;
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

#ifdef TP_FAKE_MEDIA
// 测试时可以用 -DTP_FAKE_MEDIA 假装在放歌（在没有这套接口的环境里看界面）：
// 上一首、下一首换一首，播放 / 暂停跟着切换，第一首没有上一首
class Backend {
public:
    explicit Backend(bool) {}
    bool Ready() const { return true; }
    MediaInfo Read() {
        MediaInfo info;
        info.present = true;
        info.playing = m_playing;
        info.canPrevious = m_song > 1;
        info.canNext = info.canPlayPause = true;
        info.title = L"测试歌曲 " + std::to_wstring(m_song);
        info.artist = L"测试歌手";
        // 封面是 C:\coverN.png（没有就没有封面）
        if (m_coverSong != m_song) {
            m_coverSong = m_song;
            Gdiplus::Bitmap file((L"C:\\cover" + std::to_wstring(m_song) + L".png").c_str());
            m_cover = CoverFrom(file);
        }
        info.cover = m_cover;
        return info;
    }
    bool Command(int command, bool playing) {
        if (command == kMediaPrevious && m_song > 1) --m_song;
        else if (command == kMediaNext) ++m_song;
        else if (command == kMediaPlayPause) m_playing = !playing;
        return true;
    }

private:
    int m_song = 1;
    bool m_playing = true;
    int m_coverSong = 0;
    std::shared_ptr<Gdiplus::Bitmap> m_cover;
};
#else
class Backend {
public:
    explicit Backend(bool comReady) {
        if (!comReady) return;
        HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!combase) return;
        using GetFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
        using CreateStringFn = HRESULT(WINAPI*)(LPCWSTR, UINT32, HSTRING*);
        auto getFactory = reinterpret_cast<GetFactoryFn>(
            reinterpret_cast<void*>(GetProcAddress(combase, "RoGetActivationFactory")));
        auto createString = reinterpret_cast<CreateStringFn>(
            reinterpret_cast<void*>(GetProcAddress(combase, "WindowsCreateString")));
        m_deleteString = reinterpret_cast<DeleteStringFn>(
            reinterpret_cast<void*>(GetProcAddress(combase, "WindowsDeleteString")));
        m_rawBuffer = reinterpret_cast<RawBufferFn>(
            reinterpret_cast<void*>(GetProcAddress(combase, "WindowsGetStringRawBuffer")));
        if (!getFactory || !createString || !m_deleteString || !m_rawBuffer) return;
        // WinRT 的流转成 IStream 给 GDI+ 解封面（Win8 起才有）
        if (HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            m_streamOver = reinterpret_cast<StreamOverFn>(
                reinterpret_cast<void*>(GetProcAddress(shcore, "CreateStreamOverRandomAccessStream")));
        const wchar_t kClass[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        HSTRING name = nullptr;
        if (FAILED(createString(kClass, static_cast<UINT32>(wcslen(kClass)), &name))) return;
        ISessionManagerStaticsAbi* statics = nullptr;
        HRESULT hr = getFactory(name, kIidManagerStatics, reinterpret_cast<void**>(&statics));
        m_deleteString(name);
        if (FAILED(hr) || !statics) {
            Log(L"媒体：拿不到 Windows.Media.Control（0x%08X）", static_cast<unsigned>(hr));
            return;
        }
        IAsyncOperationAbi<ISessionManagerAbi*>* op = nullptr;
        hr = statics->RequestAsync(&op);
        if (SUCCEEDED(hr) && op && Await(op, kWaitMs)) hr = op->GetResults(&m_manager);
        else if (SUCCEEDED(hr)) hr = E_FAIL;
        SafeRelease(op);
        statics->Release();
        if (FAILED(hr) || !m_manager) {
            m_manager = nullptr;
            Log(L"媒体：拿不到媒体会话管理器（0x%08X）", static_cast<unsigned>(hr));
        }
    }
    ~Backend() { SafeRelease(m_manager); }
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    bool Ready() const { return m_manager != nullptr; }

    MediaInfo Read() {
        MediaInfo info;
        ISessionAbi* session = Current();
        if (!session) return info;
        IPlaybackInfoAbi* playback = nullptr;
        IPlaybackControlsAbi* controls = nullptr;
        int status = kStatusClosed;
        if (SUCCEEDED(session->GetPlaybackInfo(&playback)) && playback) {
            playback->get_PlaybackStatus(&status);
            if (SUCCEEDED(playback->get_Controls(&controls)) && controls) {
                unsigned char prev = 0, next = 0, toggle = 0, play = 0, pause = 0;
                controls->get_IsPreviousEnabled(&prev);
                controls->get_IsNextEnabled(&next);
                controls->get_IsPlayPauseToggleEnabled(&toggle);
                controls->get_IsPlayEnabled(&play);
                controls->get_IsPauseEnabled(&pause);
                info.canPrevious = prev != 0;
                info.canNext = next != 0;
                info.canPlayPause = toggle || play || pause;
            }
        }
        IAsyncOperationAbi<IMediaPropertiesAbi*>* op = nullptr;
        IMediaPropertiesAbi* props = nullptr;
        if (SUCCEEDED(session->TryGetMediaPropertiesAsync(&op)) && op && Await(op, kWaitMs) &&
            SUCCEEDED(op->GetResults(&props)) && props) {
            HSTRING text = nullptr;
            std::wstring album, app;
            if (SUCCEEDED(props->get_Title(&text))) info.title = Take(text);
            if (SUCCEEDED(props->get_Artist(&text))) info.artist = Take(text);
            if (SUCCEEDED(props->get_AlbumTitle(&text))) album = Take(text);
            if (SUCCEEDED(session->get_SourceAppUserModelId(&text))) app = Take(text);
            // 换了歌才重新读封面；没读到的再试几遍
            std::wstring key = app + L"\n" + info.title + L"\n" + info.artist + L"\n" + album;
            if (key != m_coverKey) {
                m_coverKey = key;
                m_cover.reset();
                m_coverTries = 0;
            }
            if (!m_cover && m_coverTries < kCoverTries) {
                ++m_coverTries;
                m_cover = ReadCover(props);
            }
            info.cover = m_cover;
        }
        SafeRelease(props);
        SafeRelease(op);
        SafeRelease(controls);
        SafeRelease(playback);
        session->Release();
        info.playing = status == kStatusPlaying;
        // 关掉了的、停下来又没有标题的（网页里的视频放完了）不算
        info.present = status == kStatusPlaying || status == kStatusPaused || status == kStatusChanging ||
                       (status != kStatusClosed && !info.title.empty());
        return info;
    }

    bool Command(int command, bool playing) {
        ISessionAbi* session = Current();
        if (!session) return false;
        BoolOperation* op = nullptr;
        HRESULT hr = E_FAIL;
        if (command == kMediaPrevious) {
            hr = session->TrySkipPreviousAsync(&op);
        } else if (command == kMediaNext) {
            hr = session->TrySkipNextAsync(&op);
        } else {
            hr = session->TryTogglePlayPauseAsync(&op);
            unsigned char done = 0;
            // 有的播放器不支持“切换”，那就按现在的状态播放或暂停
            if (SUCCEEDED(hr) && op && Await(op, kWaitMs) && SUCCEEDED(op->GetResults(&done)) && !done) {
                SafeRelease(op);
                hr = playing ? session->TryPauseAsync(&op) : session->TryPlayAsync(&op);
            }
        }
        unsigned char done = 0;
        bool ok = SUCCEEDED(hr) && op && Await(op, kWaitMs) && SUCCEEDED(op->GetResults(&done)) && done;
        SafeRelease(op);
        session->Release();
        if (!ok) Log(L"媒体：命令 %d 没成功（0x%08X），改按媒体键", command, static_cast<unsigned>(hr));
        return ok;
    }

private:
    using DeleteStringFn = HRESULT(WINAPI*)(HSTRING);
    using RawBufferFn = PCWSTR(WINAPI*)(HSTRING, UINT32*);
    using StreamOverFn = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);

    std::shared_ptr<Gdiplus::Bitmap> ReadCover(IMediaPropertiesAbi* props) {
        IStreamReferenceAbi* ref = nullptr;
        if (!m_streamOver || FAILED(props->get_Thumbnail(&ref)) || !ref) return nullptr;
        IAsyncOperationAbi<IInspectable*>* op = nullptr;
        IInspectable* random = nullptr;
        IStream* stream = nullptr;
        std::shared_ptr<Gdiplus::Bitmap> cover;
        if (SUCCEEDED(ref->OpenReadAsync(&op)) && op && Await(op, kWaitMs) && SUCCEEDED(op->GetResults(&random)) &&
            random && SUCCEEDED(m_streamOver(random, IID_IStream, reinterpret_cast<void**>(&stream))) && stream) {
            Gdiplus::Bitmap decoded(stream);
            cover = CoverFrom(decoded);
        }
        if (!cover) Log(L"媒体：有封面但读不出来");
        SafeRelease(stream);
        SafeRelease(random);
        SafeRelease(op);
        ref->Release();
        return cover;
    }

    ISessionAbi* Current() {
        ISessionAbi* session = nullptr;
        if (!m_manager || FAILED(m_manager->GetCurrentSession(&session))) return nullptr;
        return session;
    }

    std::wstring Take(HSTRING text) {
        if (!text) return L"";
        UINT32 length = 0;
        PCWSTR raw = m_rawBuffer(text, &length);
        std::wstring s(raw ? raw : L"", raw ? length : 0);
        m_deleteString(text);
        return s;
    }

    ISessionManagerAbi* m_manager = nullptr;
    DeleteStringFn m_deleteString = nullptr;
    RawBufferFn m_rawBuffer = nullptr;
    StreamOverFn m_streamOver = nullptr;
    std::wstring m_coverKey;  // 封面是哪首歌的
    std::shared_ptr<Gdiplus::Bitmap> m_cover;
    int m_coverTries = 0;
};
#endif

DWORD WINAPI MediaThread(LPVOID param) {
    auto* shared = static_cast<Shared*>(param);
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Backend backend(SUCCEEDED(com));
        for (;;) {
            AcquireSRWLockExclusive(&shared->lock);
            while (!shared->quit && !shared->query && shared->command < 0) {
                if (!SleepConditionVariableSRW(&shared->wake, &shared->lock, shared->poll ? kPollMs : INFINITE, 0) &&
                    shared->poll)
                    break;  // 到时间了，读一遍
            }
            if (shared->quit) {
                ReleaseSRWLockExclusive(&shared->lock);
                break;
            }
            bool asked = shared->query || shared->command >= 0;
            int command = shared->command;
            bool playing = shared->info.playing;
            shared->query = false;
            shared->command = -1;
            ReleaseSRWLockExclusive(&shared->lock);

            if (command >= 0) {
                if (!backend.Ready() || !backend.Command(command, !playing)) SendMediaKey(command);
                Sleep(kSettleMs);
            }
            MediaInfo info = backend.Ready() ? backend.Read() : MediaInfo();

            AcquireSRWLockExclusive(&shared->lock);
            bool changed = !SameInfo(info, shared->info);
            if (shared->command < 0) shared->info = info;  // 又按了一下：等那次做完再更新
            HWND notify = shared->notify;
            ReleaseSRWLockExclusive(&shared->lock);
            if ((changed || asked) && notify && IsWindow(notify)) PostMessageW(notify, WM_APP_MEDIA, 0, 0);
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

Shared* EnsureThread() {
    if (s_thread) return s_shared;
    auto* shared = new Shared;
    HANDLE thread = CreateThread(nullptr, 0, MediaThread, shared, 0, nullptr);
    if (!thread) {
        delete shared;
        return nullptr;
    }
    s_thread = thread;
    s_shared = shared;
    return shared;
}

}  // namespace

void Media_Configure(bool enabled, HWND notify) {
    if (!enabled && !s_thread) return;
    Shared* s = EnsureThread();
    if (!s) return;
    AcquireSRWLockExclusive(&s->lock);
    bool start = enabled && !s->poll;
    s->poll = enabled;
    s->notify = notify;
    if (start) s->query = true;
    if (!enabled) s->info = MediaInfo();
    ReleaseSRWLockExclusive(&s->lock);
    WakeConditionVariable(&s->wake);
}

void Media_Query() {
    if (!s_shared) return;
    AcquireSRWLockExclusive(&s_shared->lock);
    s_shared->query = true;
    ReleaseSRWLockExclusive(&s_shared->lock);
    WakeConditionVariable(&s_shared->wake);
}

// 播放 / 暂停先把按钮换过去，读回来再按真实的样子显示
void Media_Command(int command) {
    if (!s_shared) return;
    AcquireSRWLockExclusive(&s_shared->lock);
    if (command == kMediaPlayPause && s_shared->command < 0) s_shared->info.playing = !s_shared->info.playing;
    s_shared->command = command;
    ReleaseSRWLockExclusive(&s_shared->lock);
    WakeConditionVariable(&s_shared->wake);
}

MediaInfo Media_Get() {
    MediaInfo info;
    if (!s_shared) return info;
    AcquireSRWLockShared(&s_shared->lock);
    info = s_shared->info;
    ReleaseSRWLockShared(&s_shared->lock);
    return info;
}

void Media_Stop() {
    if (!s_thread) return;
    AcquireSRWLockExclusive(&s_shared->lock);
    s_shared->quit = true;
    ReleaseSRWLockExclusive(&s_shared->lock);
    WakeConditionVariable(&s_shared->wake);
    // 卡在系统调用里的话不等了：进程马上就结束
    if (WaitForSingleObject(s_thread, kStopWaitMs) == WAIT_OBJECT_0) delete s_shared;
    CloseHandle(s_thread);
    s_thread = nullptr;
    s_shared = nullptr;
}

}  // namespace app
