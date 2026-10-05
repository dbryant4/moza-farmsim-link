#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace bridge {

/**
 * Tells the FS25 mod that the bridge is running, by rewriting `bridge.xml` beside the telemetry
 * file about twice a second (format in docs/telemetry.md). The mod turns off the game's own
 * steering force feedback only while it sees that file changing, so machines without the bridge,
 * such as the other players in a multiplayer game, keep the game's. XML, because that is the one
 * kind of file the game's Lua can read back.
 */
class PresenceFile
{
public:
    using Clock = std::chrono::steady_clock;

    /** @p telemetryPath is the telemetry file; an empty path makes every call a no-op. */
    explicit PresenceFile(const std::filesystem::path& telemetryPath)
        : m_path(telemetryPath.empty() ? std::filesystem::path() : telemetryPath.parent_path() / "bridge.xml")
    {
    }

    ~PresenceFile() { remove(); }

    PresenceFile(const PresenceFile&)            = delete;
    PresenceFile& operator=(const PresenceFile&) = delete;

    const std::filesystem::path& path() const { return m_path; }

    /**
     * Writes the next beat if half a second has passed since the last one. Does nothing while the
     * folder is missing: the mod creates it, and until the mod has run there is nobody to tell.
     */
    void tick(Clock::time_point now)
    {
        if (m_path.empty() || (m_written && now - m_lastWrite < kInterval))
        {
            return;
        }
        m_lastWrite = now;
        m_written   = true;

        std::error_code error;
        if (!std::filesystem::is_directory(m_path.parent_path(), error))
        {
            return;
        }
        // Written beside the target and renamed over it, so the game never parses half a file.
        std::filesystem::path temporary = m_path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                return;
            }
            file << "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"no\"?>\n<bridge version=\"1\" beat=\"" << m_beat + 1 << "\"/>\n";
        }
        std::filesystem::rename(temporary, m_path, error);
        if (error)
        {
            // The game has the file open for its once-a-second read. Skip this beat; the next one lands.
            std::filesystem::remove(temporary, error);
            return;
        }
        ++m_beat;
        m_created = true;
    }

    /** Deletes the file, so the mod notices at its next read rather than after its timeout. */
    void remove()
    {
        if (m_created)
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
            m_created = false;
        }
    }

private:
    static constexpr std::chrono::milliseconds kInterval{500};

    std::filesystem::path m_path;
    Clock::time_point     m_lastWrite{};
    std::uint64_t         m_beat    = 0;
    bool                  m_written = false;
    bool                  m_created = false;
};

} // namespace bridge
