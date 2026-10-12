#pragma once

// Looks for newer releases on GitHub and, when asked, installs one. Windows only; what a release
// document means is decided in update_check.hpp.

#include "update_check.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace bridge {

class Updater
{
public:
    struct Config
    {
        /** "owner/name" of the GitHub repository whose releases are followed. */
        std::string repository;
        Version     current;
        /** Where the running program is installed, and so where a new version goes. */
        std::filesystem::path installDirectory;
        /** Asked before each check the updater makes by itself; one asked for by hand always runs. */
        std::function<bool()> enabled;
        /** A newer release exists. Called from the updater's thread, once per release found. */
        std::function<void(const ReleaseInfo& release, bool installable)> onAvailable;
        /** Something the user should be told: the answer to a check made by hand, progress, or a failure. */
        std::function<void(const std::string& title, const std::string& text)> onMessage;
    };

    explicit Updater(Config config);
    /** Stops, if stop() was not called. */
    ~Updater();

    Updater(const Updater&)            = delete;
    Updater& operator=(const Updater&) = delete;

    /** Begins checking: shortly after this call, then once a day. */
    void start();
    /** Ends the checking and abandons a download in progress. The other members stay safe to call and do nothing. */
    void stop();

    /** Checks now, and says so through onMessage even when there is nothing new. Returns at once. */
    void checkNow();
    /**
     * Downloads the newer release's installer and runs it. The installer asks this program to
     * close, replaces it and starts it again. Returns at once; does nothing if no installable
     * release has been found.
     */
    void installNow();
    /** Opens the newer release's page, or the list of releases if none has been found. */
    void openReleasePage();

    /**
     * True if this copy was put there by the installer, so that the installer can replace it. A
     * copy unpacked from the zip is only told where to download from.
     */
    bool installed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * For --check-update: asks GitHub once and prints what it found. With @p fetch the installer is
 * also downloaded and verified, then deleted; it is never run. Returns a process exit code.
 */
int printUpdateCheck(const std::string& repository, const Version& current, bool fetch);

} // namespace bridge
