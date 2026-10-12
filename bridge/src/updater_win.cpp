#include "updater.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace bridge {

namespace {

constexpr DWORD       kFirstCheckDelayMs = 30 * 1000;
constexpr DWORD       kCheckIntervalMs   = 24 * 60 * 60 * 1000;
// The installer is under 1 MB; anything near this size is not it.
constexpr std::size_t kLargestInstaller  = 64u * 1024 * 1024;
constexpr std::size_t kLargestDocument   = 2u * 1024 * 1024;

std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int    length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

struct InternetHandle
{
    HINTERNET handle = nullptr;
    ~InternetHandle()
    {
        if (handle != nullptr)
        {
            WinHttpCloseHandle(handle);
        }
    }
    explicit operator bool() const { return handle != nullptr; }
};

/**
 * Fetches @p url over HTTPS, handing the body to @p sink piece by piece. Returns the HTTP status,
 * or 0 with @p error set if the exchange failed, was stopped or @p sink refused a piece.
 */
int httpsGet(const std::wstring& url, const std::wstring& agent, const std::atomic<bool>& stop,
             const std::function<bool(const char* data, std::size_t size)>& sink, std::string& error)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256];
    wchar_t path[2048];
    parts.lpszHostName     = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath      = path;
    parts.dwUrlPathLength  = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
    {
        error = "not an https address";
        return 0;
    }

    const InternetHandle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session)
    {
        error = "WinHTTP is not available";
        return 0;
    }
    WinHttpSetTimeouts(session.handle, 10000, 10000, 15000, 30000);
    const InternetHandle connection{WinHttpConnect(session.handle, host, parts.nPort, 0)};
    if (!connection)
    {
        error = "could not connect";
        return 0;
    }
    // Redirects are followed by WinHTTP itself, and never from https down to http.
    const InternetHandle request{
        WinHttpOpenRequest(connection.handle, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request)
    {
        error = "could not prepare the request";
        return 0;
    }
    const wchar_t headers[] = L"Accept: application/vnd.github+json, application/octet-stream\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.handle, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr))
    {
        error = "no answer (error " + std::to_string(GetLastError()) + ")";
        return 0;
    }

    DWORD status     = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);

    std::vector<char> buffer(64 * 1024);
    for (;;)
    {
        if (stop)
        {
            error = "stopped";
            return 0;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
        {
            error = "the connection broke off (error " + std::to_string(GetLastError()) + ")";
            return 0;
        }
        if (read == 0)
        {
            break;
        }
        if (status == 200 && !sink(buffer.data(), read))
        {
            if (error.empty())
            {
                error = "more data than expected";
            }
            return 0;
        }
    }
    return static_cast<int>(status);
}

/** SHA-256 as it goes along, through Windows' own implementation. */
class Sha256
{
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        {
            m_algorithm = nullptr;
            return;
        }
        if (BCryptCreateHash(m_algorithm, &m_hash, nullptr, 0, nullptr, 0, 0) != 0)
        {
            m_hash = nullptr;
        }
    }
    ~Sha256()
    {
        if (m_hash != nullptr)
        {
            BCryptDestroyHash(m_hash);
        }
        if (m_algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
        }
    }
    void add(const char* data, std::size_t size)
    {
        if (m_hash != nullptr && BCryptHashData(m_hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(size), 0) != 0)
        {
            m_failed = true;
        }
    }
    /** Lower-case hex, or empty if the hash could not be computed. */
    std::string finish()
    {
        unsigned char digest[32];
        if (m_hash == nullptr || m_failed || BCryptFinishHash(m_hash, digest, sizeof(digest), 0) != 0)
        {
            return {};
        }
        std::string text;
        for (const unsigned char byte : digest)
        {
            char pair[3];
            std::snprintf(pair, sizeof(pair), "%02x", byte);
            text += pair;
        }
        return text;
    }

private:
    BCRYPT_ALG_HANDLE  m_algorithm = nullptr;
    BCRYPT_HASH_HANDLE m_hash      = nullptr;
    bool               m_failed    = false;
};

std::filesystem::path downloadFolder()
{
    wchar_t     temp[MAX_PATH + 1];
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    if (length == 0 || length >= ARRAYSIZE(temp))
    {
        return {};
    }
    return std::filesystem::path(temp) / "MozaFsLinkUpdate";
}

std::string failureText(int status, const std::string& error)
{
    return status == 0 ? error : "GitHub answered " + std::to_string(status);
}

/** Asks GitHub for the repository's latest release. Returns the HTTP status (0 if there was none) and fills @p body. */
int fetchLatest(const std::string& repository, const std::wstring& agent, const std::atomic<bool>& stop, std::string& body, std::string& error)
{
    return httpsGet(widen("https://api.github.com/repos/" + repository + "/releases/latest"), agent, stop,
                    [&](const char* data, std::size_t size) {
                        body.append(data, size);
                        return body.size() <= kLargestDocument;
                    },
                    error);
}

/**
 * Downloads @p release's installer into this program's own temporary folder, emptied first, and
 * checks it against the SHA-256 GitHub lists for it. Returns the file, or nothing with @p error
 * saying why. Never runs it.
 */
std::optional<std::filesystem::path> downloadInstaller(const ReleaseInfo& release, const std::wstring& agent, const std::atomic<bool>& stop,
                                                       std::string& error)
{
    const auto      folder = downloadFolder();
    std::error_code fileError;
    if (!folder.empty())
    {
        // The folder is this program's own; whatever an earlier update left there goes first.
        std::filesystem::remove_all(folder, fileError);
        std::filesystem::create_directories(folder, fileError);
    }
    if (folder.empty() || fileError)
    {
        error = "There is nowhere to download the installer to.";
        return std::nullopt;
    }
    const std::filesystem::path target = folder / safeFileName(release.installerName);

    Sha256      hash;
    std::size_t received = 0;
    std::string why;
    int         status = 0;
    {
        FILE* const file = _wfopen(target.c_str(), L"wb");
        if (file == nullptr)
        {
            error = "The installer could not be saved.";
            return std::nullopt;
        }
        status = httpsGet(widen(release.installerUrl), agent, stop,
                          [&](const char* data, std::size_t size) {
                              received += size;
                              hash.add(data, size);
                              return received <= kLargestInstaller && std::fwrite(data, 1, size, file) == size;
                          },
                          why);
        std::fclose(file);
    }
    if (status != 200 || received == 0)
    {
        std::filesystem::remove(target, fileError);
        error = "The download did not complete (" + failureText(status, why) + ").";
        return std::nullopt;
    }
    // GitHub publishes each attached file's SHA-256 alongside it; a download that does not match is not kept.
    const std::string digest = hash.finish();
    if (!release.installerSha256.empty() && digest != release.installerSha256)
    {
        std::filesystem::remove(target, fileError);
        error = "The downloaded installer does not match what GitHub lists, so it was not run.";
        return std::nullopt;
    }
    std::printf("Update: downloaded %s (%zu bytes, sha256 %s%s).\n", target.string().c_str(), received, digest.c_str(),
                release.installerSha256.empty() ? ", no checksum from GitHub to compare with" : ", matches GitHub's");
    return target;
}

} // namespace

struct Updater::Impl
{
    Config            config;
    std::wstring      agent;
    std::thread       thread;
    std::atomic<bool> stopping{false};
    bool              started = false;
    // Auto-reset events the thread waits on.
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    std::atomic<bool> wantCheck{false};
    std::atomic<bool> wantInstall{false};

    std::mutex                 mutex;
    std::optional<ReleaseInfo> newer;

    void say(const std::string& title, const std::string& text) const
    {
        std::printf("Update: %s %s\n", title.c_str(), text.c_str());
        if (config.onMessage)
        {
            config.onMessage(title, text);
        }
    }

    bool installedCopy() const
    {
        std::error_code error;
        return std::filesystem::is_regular_file(config.installDirectory / "uninstall.exe", error);
    }

    bool installable(const ReleaseInfo& release) const
    {
        return installedCopy() && isReleaseDownload(release.installerUrl, config.repository) && !safeFileName(release.installerName).empty();
    }

    void check(bool byHand)
    {
        std::string body;
        std::string error;
        const int   status = fetchLatest(config.repository, agent, stopping, body, error);
        if (stopping)
        {
            return;
        }
        if (status != 200)
        {
            const std::string why = failureText(status, error);
            std::printf("Update: the check for a new version did not get through (%s).\n", why.c_str());
            if (byHand && config.onMessage)
            {
                config.onMessage("Could not check for updates", "GitHub could not be reached (" + why + ").");
            }
            return;
        }

        const auto release = parseLatestRelease(body);
        if (!release || !(release->version > config.current))
        {
            std::printf("Update: version %s is the latest.\n", config.current.text().c_str());
            if (byHand && config.onMessage)
            {
                config.onMessage("No update", "Version " + config.current.text() + " is the latest.");
            }
            return;
        }

        bool fresh;
        {
            std::scoped_lock lock(mutex);
            fresh = !newer || newer->version != release->version;
            newer = *release;
        }
        std::printf("Update: version %s is available (%s).\n", release->version.text().c_str(), release->pageUrl.c_str());
        if ((fresh || byHand) && config.onAvailable)
        {
            config.onAvailable(*release, installable(*release));
        }
    }

    void install()
    {
        std::optional<ReleaseInfo> release;
        {
            std::scoped_lock lock(mutex);
            release = newer;
        }
        if (!release || !installable(*release))
        {
            return;
        }
        const std::string version = release->version.text();
        say("Downloading version " + version, "The link restarts by itself when the update is ready.");
        std::string error;
        const auto  downloaded = downloadInstaller(*release, agent, stopping, error);
        if (stopping)
        {
            return;
        }
        if (!downloaded)
        {
            say("Update failed", error);
            return;
        }
        const std::filesystem::path target = *downloaded;
        const std::filesystem::path folder = target.parent_path();

        // /D= has to come last and takes the rest of the line unquoted, spaces and all.
        std::wstring command = L"\"" + target.wstring() + L"\" /S /RESTART /D=" + config.installDirectory.wstring();
        STARTUPINFOW        startup{};
        PROCESS_INFORMATION process{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &process))
        {
            say("Update failed", "The installer could not be started (error " + std::to_string(GetLastError()) + ").");
            return;
        }
        CloseHandle(process.hThread);
        std::puts("Update: the installer is running; it will ask this program to close.");

        // Normally this program is closed by the installer long before the installer finishes. If
        // the installer ends first, it did not get as far as replacing anything.
        const HANDLE waitOn[] = {process.hProcess, wake};
        while (!stopping)
        {
            if (WaitForMultipleObjects(2, waitOn, FALSE, 1000) == WAIT_OBJECT_0)
            {
                DWORD code = 0;
                GetExitCodeProcess(process.hProcess, &code);
                if (!stopping && code != 0)
                {
                    say("Update failed", "The installer stopped with code " + std::to_string(code) + ". Version " + config.current.text() +
                                             " is still running.");
                }
                break;
            }
        }
        CloseHandle(process.hProcess);
    }

    void run()
    {
        DWORD wait = kFirstCheckDelayMs;
        while (!stopping)
        {
            const bool timedOut = WaitForSingleObject(wake, wait) == WAIT_TIMEOUT;
            if (stopping)
            {
                break;
            }
            if (wantInstall.exchange(false))
            {
                install();
                continue;
            }
            const bool byHand = wantCheck.exchange(false);
            if (byHand || (timedOut && (!config.enabled || config.enabled())))
            {
                check(byHand);
            }
            if (timedOut)
            {
                wait = kCheckIntervalMs;
            }
        }
    }
};

Updater::Updater(Config config) : m_impl(std::make_unique<Impl>())
{
    m_impl->agent  = widen("moza-farmsim-link/" + config.current.text());
    m_impl->config = std::move(config);
}

Updater::~Updater()
{
    stop();
    if (m_impl->wake != nullptr)
    {
        CloseHandle(m_impl->wake);
    }
}

void Updater::start()
{
    if (!m_impl->started && !m_impl->stopping)
    {
        m_impl->started = true;
        m_impl->thread  = std::thread([impl = m_impl.get()] { impl->run(); });
    }
}

void Updater::stop()
{
    m_impl->stopping = true;
    SetEvent(m_impl->wake);
    if (m_impl->thread.joinable())
    {
        m_impl->thread.join();
    }
}

void Updater::checkNow()
{
    m_impl->wantCheck = true;
    SetEvent(m_impl->wake);
}

void Updater::installNow()
{
    m_impl->wantInstall = true;
    SetEvent(m_impl->wake);
}

void Updater::openReleasePage()
{
    std::string url;
    {
        std::scoped_lock lock(m_impl->mutex);
        if (m_impl->newer)
        {
            url = m_impl->newer->pageUrl;
        }
    }
    const std::string releases = "https://github.com/" + m_impl->config.repository + "/releases";
    // Only ever a page of this project on GitHub, whatever the release document said.
    if (url.rfind(releases, 0) != 0 || url.find_first_of(" \t\r\n\"'<>\\") != std::string::npos)
    {
        url = releases + "/latest";
    }
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool Updater::installed() const
{
    return m_impl->installedCopy();
}

int printUpdateCheck(const std::string& repository, const Version& current, bool fetch)
{
    // The hashing is Windows' own; one known answer shows it is being driven correctly.
    Sha256 known;
    known.add("abc", 3);
    if (known.finish() != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    {
        std::puts("SHA-256 is not working on this system, so a download could not be verified.");
        return 1;
    }

    const std::atomic<bool> stop{false};
    const std::wstring      agent = widen("moza-farmsim-link/" + current.text());
    std::string             body;
    std::string             error;
    const int               status = fetchLatest(repository, agent, stop, body, error);
    if (status != 200)
    {
        std::printf("GitHub could not be asked about %s (%s).\n", repository.c_str(), failureText(status, error).c_str());
        return 1;
    }
    const auto release = parseLatestRelease(body);
    if (!release)
    {
        std::puts("GitHub's answer does not describe a published release with a version number.");
        return 1;
    }
    const bool newer = release->version > current;
    std::printf("This is version %s; the latest release is %s (%s).\n", current.text().c_str(), release->version.text().c_str(),
                newer ? "newer" : "not newer");
    std::printf("  page:      %s\n", release->pageUrl.c_str());
    if (release->installerUrl.empty())
    {
        std::puts("  installer: none attached, so this release can only be downloaded by hand");
        return 0;
    }
    std::printf("  installer: %s\n", release->installerUrl.c_str());
    std::printf("  sha256:    %s\n", release->installerSha256.empty() ? "(GitHub lists none)" : release->installerSha256.c_str());
    if (!isReleaseDownload(release->installerUrl, repository) || safeFileName(release->installerName).empty())
    {
        std::puts("  That is not a file of this project's releases, so it would not be installed.");
        return 1;
    }
    if (!fetch)
    {
        return 0;
    }
    const auto downloaded = downloadInstaller(*release, agent, stop, error);
    if (!downloaded)
    {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::error_code ignored;
    std::filesystem::remove_all(downloaded->parent_path(), ignored);
    std::puts("The installer downloaded and verified. It was not run, and has been deleted again.");
    return 0;
}

} // namespace bridge
