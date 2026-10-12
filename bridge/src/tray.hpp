#pragma once

// The icon by the clock that the windowless build of the bridge lives behind. Windows only.

#include <functional>
#include <memory>
#include <string>

namespace bridge {

class Tray
{
public:
    struct Config
    {
        /** Called, from the tray's own thread, by a click on the icon and by "Open settings". */
        std::function<void()> openSettings;
        /**
         * Called on the tray's thread with each message (a `MSG*`) before it is dispatched; true
         * means it has been dealt with. For a window that lives on this thread.
         */
        std::function<bool(void* message)> translate;
        /** An event that, when set from anywhere, has the same effect as a click on the icon. May be null. */
        const wchar_t* showEventName = nullptr;
        /** Opened by "Open log file". */
        std::wstring logPath;
        /** Called, from the tray's own thread, when the user picks Exit or Windows is shutting down. */
        std::function<void()> requestExit;
        /** Called after requestExit on shutdown or logoff: must return once forces are released, or after a few seconds. */
        std::function<void()> waitForCleanup;
        /** "Check for updates". Without it the menu has no update entries at all. Must return at once. */
        std::function<void()> checkForUpdates;
        /** The entry offering a newer version, once setUpdate() has named one. Must return at once. */
        std::function<void()> installUpdate;
        /** "What's new in ...". Must return at once. */
        std::function<void()> showUpdateNotes;
    };

    explicit Tray(Config config);
    /** Removes the icon. */
    ~Tray();

    Tray(const Tray&)            = delete;
    Tray& operator=(const Tray&) = delete;

    /** Text shown when the pointer rests on the icon. Safe from any thread. */
    void setStatus(const std::string& text);
    /** Makes "Open settings" work, once there are settings to open. Safe from any thread. */
    void setSettingsAvailable(bool available);
    /**
     * Puts a newer release in the menu under @p name, which reads on after "Install", for example
     * "version 0.1.3" or "pre-release 0.1.4"; empty takes it out again. @p installable says whether
     * picking it installs it or only leads to the download page. Safe from any thread.
     */
    void setUpdate(const std::string& name, bool installable);
    /** A notification bubble from the icon. Safe from any thread. */
    void notify(const std::string& title, const std::string& text);

    /** Name of the event that tells a running instance to exit; `--quit` sets it. */
    static const wchar_t* quitEventName();
    /** Opens @p url in the default browser. */
    static void openInBrowser(const std::string& url);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bridge
