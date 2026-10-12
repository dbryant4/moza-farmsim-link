#pragma once

#include "settings.hpp"
#include "status_board.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace bridge {

/**
 * Serves the tuning page and its small HTTP API on 127.0.0.1, from one background thread.
 *
 * Everything is HTTP/1.1 with one request per connection. A request must carry
 * `Host: 127.0.0.1[:port]` or `Host: localhost[:port]`, and a POST that names an `Origin` must
 * name this server; anything else gets 403. No CORS headers are sent, so pages from other sites
 * can neither read the API nor post to it.
 *
 *   GET  /             The page (text/html), self-contained.
 *   GET  /api/state    The state document below (application/json).
 *   POST /api/set      Body: one `key=value` per line. Values are clamped to each setting's range.
 *                      All or nothing: an unknown key or unreadable value fails the whole request.
 *   POST /api/save     Writes the settings file.
 *   POST /api/revert   Reloads the settings file, discarding unsaved changes.
 *   POST /api/reset    Puts every setting back to its default (not saved until /api/save).
 *   GET  /api/export   The current values as a settings file (text/plain, sent as a download).
 *   POST /api/import   Body: a settings file as /api/export gives. Applies the values it holds and
 *                      leaves the rest alone (not saved until /api/save). 400 if it holds none.
 *   POST /api/forces   Body: `on` or `off`. Sets StatusBoard::setForcesEnabled().
 *   POST /api/release  Panic button: StatusBoard::requestRelease(), and forces enabled goes off
 *                      so nothing is re-applied until the user switches forces back on.
 *
 * A successful POST answers 200 with the state document plus a "result" member:
 *   "result": {"message": "Saved to moza_fs_bridge.ini", "warnings": []}
 * Every failure answers with a one-line text/plain reason: 400 malformed request or body,
 * 403 wrong Host or Origin, 404 unknown path, 405 wrong method (with Allow), 408 request not
 * finished within two seconds, 411 chunked body, 413 body over 64 KiB, 431 header over 8 KiB,
 * 500 settings file could not be written or read.
 *
 * The state document:
 *   {
 *     "revision": 12,                 Settings::revision()
 *     "dirty": true,                  Settings::dirty()
 *     "forcesEnabled": true,
 *     "releaseRequests": 0,
 *     "settingsFile": "moza_fs_bridge.ini",
 *     "settings": [
 *       {"key": "roll.spring", "group": "Steering", "label": "Centering", "description": "...",
 *        "unit": "%", "type": "number", "min": 0, "max": 100, "step": 1, "default": 30,
 *        "value": 42, "saved": 30},
 *       {"key": "rumble.on", ..., "type": "toggle", "min": 0, "max": 1, "step": 1, "default": 1,
 *        "value": 1, "saved": 1}
 *     ],
 *     "status": {
 *       "age": 0.08,                  seconds since the main loop last published; null if never
 *       "items": [
 *         {"key": "speed", "group": "Vehicle", "label": "Speed", "unit": "km/h", "decimals": 1,
 *          "min": 0, "max": 60, "value": 23.4},          min/max only for ranged items
 *         {"key": "telemetry", "group": "Vehicle", "label": "Telemetry", "unit": "", "decimals": 2,
 *          "text": "connected"}                          neither value nor text: nothing published yet
 *       ]
 *     }
 *   }
 */
class WebUi
{
public:
    /** Both objects must outlive this one. @p settingsFile is what Save writes and Revert reloads. */
    WebUi(Settings& settings, StatusBoard& status, std::filesystem::path settingsFile);
    ~WebUi();

    WebUi(const WebUi&)            = delete;
    WebUi& operator=(const WebUi&) = delete;

    /**
     * Listens on 127.0.0.1:@p port and starts the server thread. Port 0 asks the system for a free
     * one; port() says which. Returns false with the reason in @p error if the port could not be
     * bound (typically because it is in use) or the server is already running.
     */
    bool start(std::uint16_t port, std::string* error = nullptr);

    /** Closes every connection and joins the thread, within about a tenth of a second. Safe to repeat. */
    void stop();

    bool running() const;

    /** The port being listened on, or 0 when not running. */
    std::uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/** The embedded page, as served for GET /. */
const std::string& webUiPage();

} // namespace bridge
