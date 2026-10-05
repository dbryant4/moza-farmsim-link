#include "web_ui.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace bridge {

namespace {

using Clock = std::chrono::steady_clock;

// Limits on what one client may send and how long it may take. A request for this API is a few
// hundred bytes and arrives at once, so anything near these is a mistake or an attack.
constexpr std::size_t kMaxHeaderBytes = 8 * 1024;
constexpr std::size_t kMaxBodyBytes   = 16 * 1024;
constexpr std::size_t kMaxConnections = 32;
constexpr std::size_t kMaxDrainBytes  = 256 * 1024;
constexpr auto        kRequestTimeout = std::chrono::milliseconds(2000);
constexpr auto        kWriteTimeout   = std::chrono::milliseconds(2000);
constexpr auto        kDrainTimeout   = std::chrono::milliseconds(1000);
/** How long the server thread sleeps when nothing happens; bounds how long stop() takes. */
constexpr int kPollMs = 50;

// ---- The part of the socket API that differs between Winsock and POSIX ----

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;

// select() is used on Windows; an fd_set there holds FD_SETSIZE sockets whatever their values.
static_assert(kMaxConnections + 1 <= FD_SETSIZE, "connection limit does not fit an fd_set");

int  lastSocketError() { return WSAGetLastError(); }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
bool interrupted(int error) { return error == WSAEINTR; }
void closeSocket(Socket socket) { closesocket(socket); }
void shutdownSend(Socket socket) { shutdown(socket, SD_SEND); }

bool setNonBlocking(Socket socket)
{
    u_long on = 1;
    return ioctlsocket(socket, FIONBIO, &on) == 0;
}

int receiveSome(Socket socket, char* buffer, std::size_t capacity)
{
    return recv(socket, buffer, static_cast<int>(capacity), 0);
}

int sendSome(Socket socket, const char* data, std::size_t size)
{
    return send(socket, data, static_cast<int>(size), 0);
}
#else
using Socket = int;
const Socket kNoSocket = -1;

int  lastSocketError() { return errno; }
bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool interrupted(int error) { return error == EINTR; }
void closeSocket(Socket socket) { ::close(socket); }
void shutdownSend(Socket socket) { ::shutdown(socket, SHUT_WR); }

bool setNonBlocking(Socket socket)
{
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

int receiveSome(Socket socket, char* buffer, std::size_t capacity)
{
    return static_cast<int>(::recv(socket, buffer, capacity, 0));
}

int sendSome(Socket socket, const char* data, std::size_t size)
{
    // Writing to a connection the client already dropped must fail, not raise SIGPIPE.
#ifdef MSG_NOSIGNAL
    return static_cast<int>(::send(socket, data, size, MSG_NOSIGNAL));
#else
    return static_cast<int>(::send(socket, data, size, 0));
#endif
}
#endif

void setOption(Socket socket, int level, int name, int value)
{
    setsockopt(socket, level, name, reinterpret_cast<const char*>(&value), sizeof value);
}

std::string socketErrorText(int error)
{
    return std::error_code(error, std::system_category()).message();
}

struct Watch
{
    Socket socket   = kNoSocket;
    bool   forWrite = false;
    bool   ready    = false;
};

/** Blocks until a watched socket can be read (or written, or has failed), or the timeout passes. */
void waitFor(std::vector<Watch>& watches, int timeoutMs)
{
#ifdef _WIN32
    fd_set readable;
    fd_set writable;
    fd_set failed;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    for (const Watch& watch : watches)
    {
        FD_SET(watch.socket, watch.forWrite ? &writable : &readable);
        FD_SET(watch.socket, &failed);
    }
    timeval timeout;
    timeout.tv_sec  = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(0, &readable, &writable, &failed, &timeout) < 0)
    {
        // Nothing sensible to do about a failed wait except not to spin on it.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }
    for (Watch& watch : watches)
    {
        watch.ready = FD_ISSET(watch.socket, &readable) || FD_ISSET(watch.socket, &writable) || FD_ISSET(watch.socket, &failed);
    }
#else
    std::vector<pollfd> entries(watches.size());
    for (std::size_t i = 0; i < watches.size(); ++i)
    {
        entries[i].fd      = watches[i].socket;
        entries[i].events  = watches[i].forWrite ? POLLOUT : POLLIN;
        entries[i].revents = 0;
    }
    if (::poll(entries.data(), static_cast<nfds_t>(entries.size()), timeoutMs) < 0)
    {
        if (!interrupted(errno))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return;
    }
    for (std::size_t i = 0; i < watches.size(); ++i)
    {
        watches[i].ready = entries[i].revents != 0;
    }
#endif
}

// ---- Text helpers ----

std::string lowered(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

std::string_view trim(std::string_view text)
{
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && blank(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

bool allDigits(std::string_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string fileText(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

void appendJsonString(std::string& out, std::string_view text)
{
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20)
            {
                out += "\\u00";
                out += kHex[byte >> 4];
                out += kHex[byte & 0x0F];
            }
            else
            {
                // UTF-8 passes through as it is; the response is declared as UTF-8.
                out += c;
            }
        }
    }
    out += '"';
}

void appendJsonNumber(std::string& out, double value)
{
    // JSON has no way to write infinity or NaN.
    out += std::isfinite(value) ? formatNumber(value) : "null";
}

void appendJsonField(std::string& out, std::string_view name, std::string_view text)
{
    appendJsonString(out, name);
    out += ':';
    appendJsonString(out, text);
    out += ',';
}

void appendJsonField(std::string& out, std::string_view name, double value)
{
    appendJsonString(out, name);
    out += ':';
    appendJsonNumber(out, value);
    out += ',';
}

/** Replaces the trailing comma left by the appendJsonField calls with the closing bracket. */
void closeJson(std::string& out, char bracket)
{
    if (!out.empty() && out.back() == ',')
    {
        out.back() = bracket;
    }
    else
    {
        out += bracket;
    }
}

std::string jsonStringList(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (const std::string& item : items)
    {
        appendJsonString(out, item);
        out += ',';
    }
    closeJson(out, ']');
    return out;
}

// ---- HTTP ----

const char* reasonPhrase(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    default: return "Internal Server Error";
    }
}

std::string makeResponse(int status, std::string_view contentType, std::string_view body, std::string_view extraHeaders = {})
{
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) + "\r\n";
    out += "Content-Type: ";
    out += contentType;
    out += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    // No Access-Control-* headers on purpose: other origins get no access to this server.
    out += "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    out += extraHeaders;
    out += "\r\n";
    out += body;
    return out;
}

std::string plainResponse(int status, std::string_view message, std::string_view extraHeaders = {})
{
    std::string body(message);
    body += '\n';
    return makeResponse(status, "text/plain; charset=utf-8", body, extraHeaders);
}

struct Request
{
    std::string method;
    std::string path;
    std::string host;
    std::string origin;
    std::string body;
};

struct ParseOutcome
{
    enum class State
    {
        NeedMore,
        Ready,
        Reject,
    };

    State       state  = State::NeedMore;
    int         status = 0;
    std::string reason;

    static ParseOutcome reject(int status, std::string reason) { return {State::Reject, status, std::move(reason)}; }
};

/** Method, target and version, or a rejection. @p line has no line ending. */
ParseOutcome parseRequestLine(std::string_view line, Request& request)
{
    const auto firstSpace = line.find(' ');
    const auto lastSpace  = line.rfind(' ');
    if (firstSpace == std::string_view::npos || lastSpace == firstSpace)
    {
        return ParseOutcome::reject(400, "malformed request line");
    }
    const std::string_view method  = line.substr(0, firstSpace);
    const std::string_view target  = line.substr(firstSpace + 1, lastSpace - firstSpace - 1);
    const std::string_view version = line.substr(lastSpace + 1);

    const bool methodOk = !method.empty() && method.size() <= 16 &&
                          std::all_of(method.begin(), method.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
    if (!methodOk || target.empty() || target.front() != '/' || target.find(' ') != std::string_view::npos ||
        (version != "HTTP/1.1" && version != "HTTP/1.0"))
    {
        return ParseOutcome::reject(400, "malformed request line");
    }
    request.method = std::string(method);
    request.path   = std::string(target.substr(0, target.find_first_of("?#")));
    return {ParseOutcome::State::Ready, 0, {}};
}

/**
 * Looks at everything received so far on a connection. Called again from the start each time more
 * arrives, which is cheap at these sizes and keeps no parser state between calls.
 */
ParseOutcome parseRequest(std::string_view in, Request& request)
{
    // Anything that is not printable text before the first line ends is not HTTP (a TLS hello,
    // a port scanner, noise): say so at once instead of waiting for a header that will not come.
    const auto firstLineEnd = in.find('\n');
    const auto firstLine    = in.substr(0, firstLineEnd);
    for (const char c : firstLine)
    {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte < 0x20 && c != '\r') || byte >= 0x7F)
        {
            return ParseOutcome::reject(400, "not an HTTP request");
        }
    }
    if (firstLineEnd != std::string_view::npos)
    {
        std::string_view line = firstLine;
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        const ParseOutcome outcome = parseRequestLine(line, request);
        if (outcome.state == ParseOutcome::State::Reject)
        {
            return outcome;
        }
    }

    std::size_t headerEnd = std::string_view::npos;
    std::size_t bodyStart = 0;
    for (std::size_t i = firstLineEnd; i != std::string_view::npos; i = in.find('\n', i + 1))
    {
        // A blank line ends the header, with or without carriage returns.
        if (in.substr(i + 1, 1) == "\n")
        {
            headerEnd = i;
            bodyStart = i + 2;
            break;
        }
        if (in.substr(i + 1, 2) == "\r\n")
        {
            headerEnd = i;
            bodyStart = i + 3;
            break;
        }
    }
    if (headerEnd == std::string_view::npos)
    {
        return in.size() > kMaxHeaderBytes ? ParseOutcome::reject(431, "request header too large") : ParseOutcome{};
    }
    if (bodyStart > kMaxHeaderBytes)
    {
        return ParseOutcome::reject(431, "request header too large");
    }

    bool        haveHost      = false;
    bool        haveLength    = false;
    std::size_t contentLength = 0;
    std::size_t lineStart     = firstLineEnd + 1;
    while (lineStart <= headerEnd)
    {
        const std::size_t lineEnd = in.find('\n', lineStart);
        std::string_view  line    = in.substr(lineStart, lineEnd - lineStart);
        lineStart                 = lineEnd + 1;
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 || line.substr(0, colon).find_first_of(" \t") != std::string_view::npos)
        {
            return ParseOutcome::reject(400, "malformed header line");
        }
        const std::string      name  = lowered(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));
        if (name == "host")
        {
            if (haveHost)
            {
                return ParseOutcome::reject(400, "more than one Host header");
            }
            haveHost     = true;
            request.host = std::string(value);
        }
        else if (name == "content-length")
        {
            // Nine digits cannot overflow, and are far past the limit anyway.
            if (haveLength || !allDigits(value) || value.size() > 9)
            {
                return ParseOutcome::reject(400, "bad Content-Length");
            }
            haveLength    = true;
            contentLength = static_cast<std::size_t>(std::stoul(std::string(value)));
        }
        else if (name == "transfer-encoding")
        {
            return ParseOutcome::reject(411, "chunked bodies are not supported, send Content-Length");
        }
        else if (name == "origin")
        {
            request.origin = std::string(value);
        }
    }
    if (!haveHost)
    {
        return ParseOutcome::reject(400, "missing Host header");
    }
    if (contentLength > kMaxBodyBytes)
    {
        return ParseOutcome::reject(413, "request body too large");
    }
    if (in.size() - bodyStart < contentLength)
    {
        return {};
    }
    request.body = std::string(in.substr(bodyStart, contentLength));
    return {ParseOutcome::State::Ready, 0, {}};
}

/** True for `127.0.0.1` or `localhost`, optionally followed by a port. Expects lower case. */
bool isLocalHost(std::string_view host)
{
    const auto colon = host.rfind(':');
    if (colon != std::string_view::npos)
    {
        const std::string_view port = host.substr(colon + 1);
        if (!allDigits(port) || port.size() > 5)
        {
            return false;
        }
        host = host.substr(0, colon);
    }
    return host == "127.0.0.1" || host == "localhost";
}

struct Route
{
    std::string_view path;
    std::string_view method;
};

constexpr Route kRoutes[] = {
    {"/", "GET"},
    {"/index.html", "GET"},
    {"/api/state", "GET"},
    {"/api/set", "POST"},
    {"/api/save", "POST"},
    {"/api/revert", "POST"},
    {"/api/reset", "POST"},
    {"/api/forces", "POST"},
    {"/api/release", "POST"},
};

// The page needs nothing from anywhere else, and this makes the browser hold it to that. It also
// may not be framed, so another site cannot overlay it to trick a click onto a control.
constexpr std::string_view kPageHeaders =
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
    "connect-src 'self'; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: no-referrer\r\n";

struct Connection
{
    enum class State
    {
        Reading,
        Writing,
        /** Response sent; discarding whatever the client still sends until it closes. */
        Draining,
        Closed,
    };

    Socket            socket = kNoSocket;
    State             state  = State::Reading;
    std::string       in;
    std::string       out;
    std::size_t       sent    = 0;
    std::size_t       drained = 0;
    Clock::time_point deadline;
};

} // namespace

struct WebUi::Impl
{
    Impl(Settings& settingsIn, StatusBoard& statusIn, std::filesystem::path settingsFileIn)
        : settings(settingsIn), status(statusIn), settingsFile(std::move(settingsFileIn))
    {
    }

    Settings&                  settings;
    StatusBoard&               status;
    std::filesystem::path      settingsFile;
    std::thread                thread;
    std::atomic<bool>          stopRequested{false};
    std::atomic<std::uint16_t> port{0};
    Socket                     listener = kNoSocket;
#ifdef _WIN32
    bool winsockStarted = false;
#endif

    void releaseSockets();

    std::string stateJson(std::string_view message, const std::vector<std::string>* warnings) const;
    std::string okResponse(std::string_view message, const std::vector<std::string>* warnings = nullptr) const;
    std::string respond(const Request& request);
    std::string setSettings(std::string_view body);

    void run();
    void acceptPending(std::vector<Connection>& connections);
    void service(Connection& connection);
    void beginResponse(Connection& connection, std::string response);
    void expire(Connection& connection);
    static void drop(Connection& connection);
};

std::string WebUi::Impl::stateJson(std::string_view message, const std::vector<std::string>* warnings) const
{
    std::string out = "{";
    appendJsonField(out, "revision", static_cast<double>(settings.revision()));
    out += std::string("\"dirty\":") + (settings.dirty() ? "true" : "false") + ",";
    out += std::string("\"forcesEnabled\":") + (status.forcesEnabled() ? "true" : "false") + ",";
    appendJsonField(out, "releaseRequests", static_cast<double>(status.releaseRequests()));
    appendJsonField(out, "settingsFile", fileText(settingsFile));

    out += "\"settings\":[";
    for (const SettingState& state : settings.snapshot())
    {
        const SettingInfo& info = state.info;
        out += '{';
        appendJsonField(out, "key", info.key);
        appendJsonField(out, "group", info.group);
        appendJsonField(out, "label", info.label);
        appendJsonField(out, "description", info.description);
        appendJsonField(out, "unit", info.unit);
        appendJsonField(out, "type", info.kind == SettingInfo::Kind::Toggle ? "toggle" : "number");
        appendJsonField(out, "min", info.minimum);
        appendJsonField(out, "max", info.maximum);
        appendJsonField(out, "step", info.step);
        appendJsonField(out, "default", info.defaultValue);
        appendJsonField(out, "value", state.value);
        appendJsonField(out, "saved", state.saved);
        closeJson(out, '}');
        out += ',';
    }
    closeJson(out, ']');

    const StatusSnapshot snapshot = status.snapshot();
    out += ",\"status\":{";
    // A negative age means nothing was ever published, which is written as null.
    appendJsonField(out, "age", snapshot.ageSeconds >= 0.0 ? snapshot.ageSeconds : std::nan(""));
    out += "\"items\":[";
    for (const StatusItem& item : snapshot.items)
    {
        out += '{';
        appendJsonField(out, "key", item.key);
        appendJsonField(out, "group", item.group);
        appendJsonField(out, "label", item.label);
        appendJsonField(out, "unit", item.unit);
        appendJsonField(out, "decimals", static_cast<double>(item.decimals));
        if (item.hasRange)
        {
            appendJsonField(out, "min", item.minimum);
            appendJsonField(out, "max", item.maximum);
        }
        if (item.kind == StatusItem::Kind::Number)
        {
            appendJsonField(out, "value", item.number);
        }
        else if (item.kind == StatusItem::Kind::Text)
        {
            appendJsonField(out, "text", item.text);
        }
        closeJson(out, '}');
        out += ',';
    }
    closeJson(out, ']');
    out += '}';

    if (warnings)
    {
        out += ",\"result\":{";
        appendJsonField(out, "message", message);
        out += "\"warnings\":" + jsonStringList(*warnings) + "}";
    }
    out += "}\n";
    return out;
}

std::string WebUi::Impl::okResponse(std::string_view message, const std::vector<std::string>* warnings) const
{
    static const std::vector<std::string> kNone;
    return makeResponse(200, "application/json; charset=utf-8", stateJson(message, warnings ? warnings : &kNone));
}

std::string WebUi::Impl::setSettings(std::string_view body)
{
    std::vector<std::pair<std::string, double>> changes;
    std::string                                 problems;
    int                                         lineNumber = 0;
    while (!body.empty())
    {
        const auto             lineEnd = body.find('\n');
        const std::string_view line    = trim(body.substr(0, lineEnd));
        body.remove_prefix(lineEnd == std::string_view::npos ? body.size() : lineEnd + 1);
        ++lineNumber;
        if (line.empty())
        {
            continue;
        }
        const std::string where  = "line " + std::to_string(lineNumber) + ": ";
        const auto        equals = line.find('=');
        if (equals == std::string_view::npos)
        {
            problems += where + "expected key=value; ";
            continue;
        }
        const std::string_view key  = trim(line.substr(0, equals));
        const std::string_view text = trim(line.substr(equals + 1));
        if (!settings.contains(key))
        {
            // The key is echoed only as far as it is harmless to show.
            problems += where + "unknown setting; ";
            continue;
        }
        const auto value = settings.parse(key, text);
        if (!value)
        {
            problems += where + "cannot read the value for " + std::string(key) + "; ";
            continue;
        }
        changes.emplace_back(std::string(key), *value);
    }
    if (!problems.empty())
    {
        problems.resize(problems.size() - 2);
        return plainResponse(400, "nothing changed: " + problems);
    }
    if (changes.empty())
    {
        return plainResponse(400, "nothing changed: the body has no key=value lines");
    }
    for (const auto& [key, value] : changes)
    {
        settings.set(key, value);
    }
    return okResponse(changes.size() == 1 ? "1 setting applied" : std::to_string(changes.size()) + " settings applied");
}

std::string WebUi::Impl::respond(const Request& request)
{
    // Requests reaching this socket under any other name were steered here by a DNS trick.
    const std::string host = lowered(request.host);
    if (!isLocalHost(host))
    {
        return plainResponse(403, "this server only answers to 127.0.0.1 or localhost");
    }

    const Route* route = nullptr;
    for (const Route& candidate : kRoutes)
    {
        if (candidate.path == request.path)
        {
            route = &candidate;
        }
    }
    if (!route)
    {
        return plainResponse(404, "no such page");
    }
    if (route->method != request.method)
    {
        return plainResponse(405, "use " + std::string(route->method) + " for this path", "Allow: " + std::string(route->method) + "\r\n");
    }
    // A browser names the page a POST came from. Only this server's own page may change anything.
    if (request.method == "POST" && !request.origin.empty() && lowered(request.origin) != "http://" + host)
    {
        return plainResponse(403, "cross-origin requests are not accepted");
    }

    if (request.path == "/" || request.path == "/index.html")
    {
        return makeResponse(200, "text/html; charset=utf-8", webUiPage(), kPageHeaders);
    }
    if (request.path == "/api/state")
    {
        return makeResponse(200, "application/json; charset=utf-8", stateJson({}, nullptr));
    }
    if (request.path == "/api/set")
    {
        return setSettings(request.body);
    }
    if (request.path == "/api/save")
    {
        std::string error;
        if (!settings.save(settingsFile, &error))
        {
            return plainResponse(500, "could not save settings: " + error);
        }
        return okResponse("Saved to " + fileText(settingsFile));
    }
    if (request.path == "/api/revert")
    {
        const Settings::LoadResult loaded = settings.load(settingsFile);
        if (!loaded.ok)
        {
            return plainResponse(500, "could not reload settings: " + (loaded.warnings.empty() ? "unknown error" : loaded.warnings.front()));
        }
        return okResponse(loaded.fileFound ? "Reverted to " + fileText(settingsFile) : "No saved settings yet, so everything is at its default",
                          &loaded.warnings);
    }
    if (request.path == "/api/reset")
    {
        settings.resetToDefaults();
        return okResponse("Everything is back at its default");
    }
    if (request.path == "/api/forces")
    {
        const std::string word = lowered(trim(request.body));
        if (word == "on" || word == "true" || word == "1")
        {
            status.setForcesEnabled(true);
            return okResponse("Forces on");
        }
        if (word == "off" || word == "false" || word == "0")
        {
            status.setForcesEnabled(false);
            return okResponse("Forces off");
        }
        return plainResponse(400, "the body must be on or off");
    }
    // /api/release. Forces also go off, or the main loop would put them straight back.
    status.setForcesEnabled(false);
    status.requestRelease();
    return okResponse("Forces released and switched off");
}

void WebUi::Impl::drop(Connection& connection)
{
    if (connection.socket != kNoSocket)
    {
        closeSocket(connection.socket);
        connection.socket = kNoSocket;
    }
    connection.state = Connection::State::Closed;
}

void WebUi::Impl::beginResponse(Connection& connection, std::string response)
{
    connection.out      = std::move(response);
    connection.sent     = 0;
    connection.state    = Connection::State::Writing;
    connection.deadline = Clock::now() + kWriteTimeout;
    connection.in.clear();
    connection.in.shrink_to_fit();
}

void WebUi::Impl::service(Connection& connection)
{
    char buffer[4096];

    if (connection.state == Connection::State::Reading)
    {
        bool finished = false;
        while (connection.in.size() <= kMaxHeaderBytes + kMaxBodyBytes)
        {
            const int received = receiveSome(connection.socket, buffer, sizeof buffer);
            if (received > 0)
            {
                connection.in.append(buffer, static_cast<std::size_t>(received));
                continue;
            }
            const int error = received < 0 ? lastSocketError() : 0;
            if (received < 0 && interrupted(error))
            {
                continue;
            }
            if (received < 0 && wouldBlock(error))
            {
                break;
            }
            if (received < 0)
            {
                drop(connection);
                return;
            }
            // The client has finished sending. It may still be waiting for the answer to a
            // request that arrived whole, so that is looked at before giving up on it.
            finished = true;
            break;
        }

        Request            request;
        const ParseOutcome outcome = parseRequest(connection.in, request);
        if (outcome.state == ParseOutcome::State::NeedMore)
        {
            if (finished)
            {
                drop(connection);
            }
            return;
        }
        beginResponse(connection, outcome.state == ParseOutcome::State::Ready ? respond(request)
                                                                              : plainResponse(outcome.status, outcome.reason));
    }

    if (connection.state == Connection::State::Writing)
    {
        while (connection.sent < connection.out.size())
        {
            const int written = sendSome(connection.socket, connection.out.data() + connection.sent, connection.out.size() - connection.sent);
            if (written > 0)
            {
                connection.sent += static_cast<std::size_t>(written);
                continue;
            }
            const int error = lastSocketError();
            if (written < 0 && interrupted(error))
            {
                continue;
            }
            if (written < 0 && wouldBlock(error))
            {
                return;
            }
            drop(connection);
            return;
        }
        // Closing now could reset the connection if the client is still sending (an oversized
        // upload, say) and lose it the response. So finish our side and read until it hangs up.
        shutdownSend(connection.socket);
        connection.state    = Connection::State::Draining;
        connection.deadline = Clock::now() + kDrainTimeout;
        return;
    }

    if (connection.state == Connection::State::Draining)
    {
        while (connection.drained <= kMaxDrainBytes)
        {
            const int received = receiveSome(connection.socket, buffer, sizeof buffer);
            if (received > 0)
            {
                connection.drained += static_cast<std::size_t>(received);
                continue;
            }
            const int error = received < 0 ? lastSocketError() : 0;
            if (received < 0 && interrupted(error))
            {
                continue;
            }
            if (received < 0 && wouldBlock(error))
            {
                return;
            }
            break;
        }
        drop(connection);
    }
}

void WebUi::Impl::expire(Connection& connection)
{
    // A client that started a request and stalled is told so. One that never sent a byte (browsers
    // open spare connections like that) is just dropped, as is one that will not take its response.
    if (connection.state == Connection::State::Reading && !connection.in.empty())
    {
        beginResponse(connection, plainResponse(408, "request not completed in time"));
        service(connection);
    }
    else
    {
        drop(connection);
    }
}

void WebUi::Impl::acceptPending(std::vector<Connection>& connections)
{
    for (int i = 0; i < 16; ++i)
    {
        const Socket socket = ::accept(listener, nullptr, nullptr);
        if (socket == kNoSocket)
        {
            const int error = lastSocketError();
            if (!wouldBlock(error) && !interrupted(error))
            {
                // Out of descriptors, most likely. The listener stays readable, so without a pause
                // this loop would spin until the shortage passes.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return;
        }
        if (connections.size() >= kMaxConnections)
        {
            // Full of clients that are saying nothing: the one waiting longest makes room.
            const auto idle = std::find_if(connections.begin(), connections.end(),
                                           [](const Connection& c) { return c.state == Connection::State::Reading; });
            if (idle == connections.end())
            {
                closeSocket(socket);
                continue;
            }
            drop(*idle);
            connections.erase(idle);
        }
        if (!setNonBlocking(socket))
        {
            closeSocket(socket);
            continue;
        }
#ifdef SO_NOSIGPIPE
        setOption(socket, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
        Connection connection;
        connection.socket   = socket;
        connection.deadline = Clock::now() + kRequestTimeout;
        connections.push_back(std::move(connection));
    }
}

void WebUi::Impl::run()
{
    // Every connection is served from this one thread without ever blocking on any of them, so a
    // client that connects and says nothing delays neither the others nor stop().
    std::vector<Connection> connections;
    std::vector<Watch>      watches;
    while (!stopRequested.load(std::memory_order_acquire))
    {
        watches.clear();
        watches.push_back({listener, false, false});
        for (const Connection& connection : connections)
        {
            watches.push_back({connection.socket, connection.state == Connection::State::Writing, false});
        }
        waitFor(watches, kPollMs);
        if (stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        for (std::size_t i = 0; i < connections.size(); ++i)
        {
            Connection& connection = connections[i];
            if (watches[i + 1].ready)
            {
                service(connection);
            }
            if (connection.state != Connection::State::Closed && Clock::now() >= connection.deadline)
            {
                expire(connection);
            }
        }
        connections.erase(std::remove_if(connections.begin(), connections.end(),
                                         [](const Connection& c) { return c.state == Connection::State::Closed; }),
                          connections.end());
        if (watches[0].ready)
        {
            acceptPending(connections);
        }
    }
    for (Connection& connection : connections)
    {
        // Stopping: reset rather than close politely, so nothing lingers in the system holding
        // the port after this returns.
        linger reset;
        reset.l_onoff  = 1;
        reset.l_linger = 0;
        setsockopt(connection.socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&reset), sizeof reset);
        drop(connection);
    }
}

void WebUi::Impl::releaseSockets()
{
    if (listener != kNoSocket)
    {
        closeSocket(listener);
        listener = kNoSocket;
    }
#ifdef _WIN32
    if (winsockStarted)
    {
        WSACleanup();
        winsockStarted = false;
    }
#endif
    port.store(0, std::memory_order_release);
}

WebUi::WebUi(Settings& settings, StatusBoard& status, std::filesystem::path settingsFile)
    : m_impl(std::make_unique<Impl>(settings, status, std::move(settingsFile)))
{
}

WebUi::~WebUi()
{
    stop();
}

bool WebUi::start(std::uint16_t port, std::string* error)
{
    Impl&      impl = *m_impl;
    const auto fail = [&](const std::string& reason) {
        impl.releaseSockets();
        if (error)
        {
            *error = reason;
        }
        return false;
    };

    if (impl.thread.joinable())
    {
        if (error)
        {
            *error = "already running on port " + std::to_string(impl.port.load());
        }
        return false;
    }

#ifdef _WIN32
    WSADATA   data;
    const int startup = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup != 0)
    {
        return fail("Winsock would not start: " + socketErrorText(startup));
    }
    impl.winsockStarted = true;
#endif

    impl.listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl.listener == kNoSocket)
    {
        return fail("cannot create a socket: " + socketErrorText(lastSocketError()));
    }
#ifdef _WIN32
    // On Windows SO_REUSEADDR would let a second program bind the same port and steal requests.
    setOption(impl.listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    // Lets a restarted bridge take its port back while old connections are still winding down.
    setOption(impl.listener, SOL_SOCKET, SO_REUSEADDR, 1);
#endif

    // Loopback only: this page moves motors and must not be reachable from the network.
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(impl.listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
    {
        return fail("cannot listen on 127.0.0.1:" + std::to_string(port) + ": " + socketErrorText(lastSocketError()));
    }
    if (::listen(impl.listener, 32) != 0)
    {
        return fail("cannot listen on 127.0.0.1:" + std::to_string(port) + ": " + socketErrorText(lastSocketError()));
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int boundSize = sizeof bound;
#else
    socklen_t boundSize = sizeof bound;
#endif
    if (::getsockname(impl.listener, reinterpret_cast<sockaddr*>(&bound), &boundSize) != 0)
    {
        return fail("cannot read back the listening port: " + socketErrorText(lastSocketError()));
    }
    if (!setNonBlocking(impl.listener))
    {
        return fail("cannot make the listening socket non-blocking: " + socketErrorText(lastSocketError()));
    }

    impl.port.store(ntohs(bound.sin_port), std::memory_order_release);
    impl.stopRequested.store(false, std::memory_order_release);
    try
    {
        impl.thread = std::thread([&impl] { impl.run(); });
    }
    catch (const std::system_error& failure)
    {
        return fail(std::string("cannot start the server thread: ") + failure.what());
    }
    return true;
}

void WebUi::stop()
{
    Impl& impl = *m_impl;
    if (impl.thread.joinable())
    {
        impl.stopRequested.store(true, std::memory_order_release);
        impl.thread.join();
    }
    impl.releaseSockets();
}

bool WebUi::running() const
{
    return m_impl->thread.joinable();
}

std::uint16_t WebUi::port() const
{
    return m_impl->port.load(std::memory_order_acquire);
}

} // namespace bridge
