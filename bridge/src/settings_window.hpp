#pragma once

// The settings window of the tray program: every tunable value with a slider or a tick box, the
// live status, and saving, reverting, importing and exporting. Windows only.

#include "settings.hpp"
#include "status_board.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace bridge {

class SettingsWindow
{
public:
    /** What the window shows and changes. The objects must stay valid until detach() has returned. */
    struct Source
    {
        Settings*             settings = nullptr;
        StatusBoard*          status   = nullptr;
        std::filesystem::path settingsFile;
        std::string           version;
    };

    SettingsWindow();
    /** Detaches. The window itself belongs to the thread that showed it and goes when that thread's message loop ends. */
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&)            = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    /** Gives the window something to show. Safe from any thread. */
    void attach(const Source& source);
    /**
     * Takes it away again: once this returns the window no longer touches the objects, and it
     * closes shortly after. Safe from any thread, and never waits for the window's thread.
     */
    void detach();
    bool attached() const;

    /**
     * Opens the window, or brings it to the front. Only from the thread whose message loop will
     * serve it, which is the thread that must also call translate(). Does nothing while detached.
     */
    void show();

    /**
     * Lets the window have a message first, for Tab, Enter and Esc. Returns true if that was all
     * the message needed. @p message is a `MSG*`.
     */
    bool translate(void* message);

    /**
     * Draws the window with page @p page selected into a .bmp file without ever showing it, so
     * that its layout can be looked at from somewhere else. Returns false if it could not.
     */
    bool renderToFile(const std::filesystem::path& file, int page);

    /**
     * Builds the window without showing it and works its controls the way a person would (drag a
     * slider, type a number, tick a box, press the buttons), checking each time that the settings
     * and the window agree. Returns one line per check, starting "ok" or "FAIL". Changes the
     * attached settings and saves them to the attached file, so attach throwaway ones.
     */
    std::string selfTest();

    /** Name of the event that asks a running instance to open this window; a second start of the program sets it. */
    static const wchar_t* showEventName();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bridge
