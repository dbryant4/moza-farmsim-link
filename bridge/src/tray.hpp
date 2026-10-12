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
        /** Opened by a click on the icon. Empty until the settings page is known to be up. */
        std::string settingsUrl;
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
    /** Makes "Open settings" work. Safe from any thread. */
    void setSettingsUrl(const std::string& url);
    /**
     * Puts a newer version in the menu. @p installable says whether picking it installs it or only
     * leads to the download page. Safe from any thread.
     */
    void setUpdate(const std::string& version, bool installable);
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
