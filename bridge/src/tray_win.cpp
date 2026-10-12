#include "tray.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <mutex>
#include <thread>

namespace bridge {

namespace {

constexpr UINT kTrayMessage   = WM_APP + 1;
constexpr UINT kUpdateMessage = WM_APP + 2;
constexpr UINT kIconId        = 1;

enum MenuItem : UINT
{
    kOpenSettings = 1,
    kOpenLog,
    kCheckForUpdates,
    kInstallUpdate,
    kUpdateNotes,
    kExit,
};

std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

/** Copies as much of @p text as fits, always terminated. */
template <std::size_t N>
void copyInto(wchar_t (&target)[N], const std::wstring& text)
{
    const std::size_t length = std::min(text.size(), N - 1);
    text.copy(target, length);
    target[length] = L'\0';
}

} // namespace

struct Tray::Impl
{
    Config      config;
    std::thread thread;
    HWND        window          = nullptr;
    HANDLE      ready           = nullptr;
    HANDLE      quitEvent       = nullptr;
    UINT        taskbarCreated  = 0;
    HICON       icon            = nullptr;

    // Written by any thread, read by the tray thread when it handles kUpdateMessage.
    std::mutex   mutex;
    std::wstring status = L"MOZA FS25 link";
    std::wstring settingsUrl;
    std::wstring updateVersion;
    bool         updateInstallable = false;
    std::wstring balloonTitle;
    std::wstring balloonText;
    bool         balloonPending = false;

    static Impl* self(HWND window) { return reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA)); }

    NOTIFYICONDATAW iconData() const
    {
        NOTIFYICONDATAW data{};
        data.cbSize           = sizeof(data);
        data.hWnd             = window;
        data.uID              = kIconId;
        data.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon            = icon;
        return data;
    }

    void addIcon()
    {
        NOTIFYICONDATAW data = iconData();
        {
            std::scoped_lock lock(mutex);
            copyInto(data.szTip, status);
        }
        Shell_NotifyIconW(NIM_ADD, &data);
    }

    void refresh()
    {
        NOTIFYICONDATAW data = iconData();
        std::scoped_lock lock(mutex);
        copyInto(data.szTip, status);
        if (balloonPending)
        {
            data.uFlags |= NIF_INFO;
            data.dwInfoFlags = NIIF_INFO;
            copyInto(data.szInfoTitle, balloonTitle);
            copyInto(data.szInfo, balloonText);
            balloonPending = false;
        }
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void openSettings()
    {
        std::wstring url;
        {
            std::scoped_lock lock(mutex);
            url = settingsUrl;
        }
        if (!url.empty())
        {
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void showMenu()
    {
        bool         haveSettings;
        std::wstring newer;
        bool         installable;
        {
            std::scoped_lock lock(mutex);
            haveSettings = !settingsUrl.empty();
            newer        = updateVersion;
            installable  = updateInstallable;
        }
        const HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (haveSettings ? 0 : MF_GRAYED), kOpenSettings, L"Open settings");
        AppendMenuW(menu, MF_STRING, kOpenLog, L"Open log file");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (config.checkForUpdates)
        {
            if (!newer.empty() && config.installUpdate)
            {
                // The entry says what picking it does, so that nothing further needs asking.
                const std::wstring offer = installable ? L"Install " + newer + L" (the link restarts)"
                                                       : L"Download " + newer + L" (opens GitHub)";
                AppendMenuW(menu, MF_STRING, kInstallUpdate, offer.c_str());
                if (installable && config.showUpdateNotes)
                {
                    AppendMenuW(menu, MF_STRING, kUpdateNotes, (L"What's new in " + newer).c_str());
                }
            }
            AppendMenuW(menu, MF_STRING, kCheckForUpdates, L"Check for updates");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu, MF_STRING, kExit, L"Exit (releases the yoke)");
        SetMenuDefaultItem(menu, kOpenSettings, FALSE);

        POINT cursor;
        GetCursorPos(&cursor);
        // Without this the menu does not close when the user clicks elsewhere.
        SetForegroundWindow(window);
        const UINT picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(menu);

        switch (picked)
        {
        case kOpenSettings: openSettings(); break;
        case kOpenLog: ShellExecuteW(nullptr, L"open", config.logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case kCheckForUpdates: config.checkForUpdates(); break;
        case kInstallUpdate: config.installUpdate(); break;
        case kUpdateNotes: config.showUpdateNotes(); break;
        case kExit:
            if (config.requestExit)
            {
                config.requestExit();
            }
            break;
        default: break;
        }
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            return DefWindowProcW(window, message, wParam, lParam);
        }
        Impl* const impl = self(window);
        if (impl == nullptr)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }

        if (message == impl->taskbarCreated && message != 0)
        {
            // Explorer restarted and forgot every icon.
            impl->addIcon();
            return 0;
        }
        switch (message)
        {
        case kTrayMessage:
            switch (LOWORD(lParam))
            {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK: impl->openSettings(); break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU: impl->showMenu(); break;
            default: break;
            }
            return 0;
        case kUpdateMessage:
            impl->refresh();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            // Windows is shutting down or the user is logging off: let go of the yoke before the process is ended.
            if (wParam != FALSE && impl->config.requestExit)
            {
                impl->config.requestExit();
                if (impl->config.waitForCleanup)
                {
                    impl->config.waitForCleanup();
                }
            }
            return 0;
        case WM_DESTROY:
        {
            NOTIFYICONDATAW data = impl->iconData();
            Shell_NotifyIconW(NIM_DELETE, &data);
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    void run()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc   = &Impl::windowProc;
        windowClass.hInstance     = instance;
        windowClass.lpszClassName = L"MozaFsLinkTray";
        RegisterClassW(&windowClass);

        // The icon compiled into the exe; the stock one if it is missing.
        icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (icon == nullptr)
        {
            icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
        }

        // An ordinary window that is never shown, not a message-only one: only top-level windows are
        // told when the taskbar is recreated or the session ends.
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        window         = CreateWindowExW(0, windowClass.lpszClassName, L"MOZA FS25 link", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (window != nullptr)
        {
            addIcon();
        }
        SetEvent(ready);
        if (window == nullptr)
        {
            return;
        }

        bool quitSeen = false;
        for (;;)
        {
            const DWORD handles = quitEvent != nullptr && !quitSeen ? 1 : 0;
            const DWORD result  = MsgWaitForMultipleObjects(handles, &quitEvent, FALSE, INFINITE, QS_ALLINPUT);
            if (handles == 1 && result == WAIT_OBJECT_0)
            {
                // Another instance was started with --quit.
                quitSeen = true;
                if (config.requestExit)
                {
                    config.requestExit();
                }
                continue;
            }
            MSG  message;
            bool quit = false;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    quit = true;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (quit)
            {
                break;
            }
        }
    }
};

Tray::Tray(Config config) : m_impl(std::make_unique<Impl>())
{
    m_impl->settingsUrl = widen(config.settingsUrl);
    m_impl->config      = std::move(config);
    m_impl->ready       = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->quitEvent   = CreateEventW(nullptr, TRUE, FALSE, quitEventName());
    m_impl->thread      = std::thread([impl = m_impl.get()] { impl->run(); });
    WaitForSingleObject(m_impl->ready, 5000);
}

Tray::~Tray()
{
    if (m_impl->window != nullptr)
    {
        PostMessageW(m_impl->window, WM_CLOSE, 0, 0);
    }
    if (m_impl->thread.joinable())
    {
        m_impl->thread.join();
    }
    if (m_impl->ready != nullptr)
    {
        CloseHandle(m_impl->ready);
    }
    if (m_impl->quitEvent != nullptr)
    {
        CloseHandle(m_impl->quitEvent);
    }
}

void Tray::setStatus(const std::string& text)
{
    {
        std::scoped_lock lock(m_impl->mutex);
        m_impl->status = widen(text);
    }
    if (m_impl->window != nullptr)
    {
        PostMessageW(m_impl->window, kUpdateMessage, 0, 0);
    }
}

void Tray::setSettingsUrl(const std::string& url)
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->settingsUrl = widen(url);
}

void Tray::setUpdate(const std::string& name, bool installable)
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->updateVersion     = widen(name);
    m_impl->updateInstallable = installable;
}

void Tray::notify(const std::string& title, const std::string& text)
{
    {
        std::scoped_lock lock(m_impl->mutex);
        m_impl->balloonTitle   = widen(title);
        m_impl->balloonText    = widen(text);
        m_impl->balloonPending = true;
    }
    if (m_impl->window != nullptr)
    {
        PostMessageW(m_impl->window, kUpdateMessage, 0, 0);
    }
}

const wchar_t* Tray::quitEventName()
{
    return L"Local\\MozaFsLinkQuit";
}

void Tray::openInBrowser(const std::string& url)
{
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace bridge
