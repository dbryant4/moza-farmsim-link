#pragma once

// Working out whether a newer release exists, from what GitHub's API says. No network access here:
// the Windows side fetches the text and this decides what it means, so it can be tested anywhere.

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace bridge {

struct Version
{
    int major = 0;
    int minor = 0;
    int patch = 0;

    auto operator<=>(const Version&) const = default;
    std::string text() const { return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch); }
};

/** Reads "0.1.2" or "v0.1.2". Anything else, including pre-release suffixes, is not a version to update to. */
std::optional<Version> parseVersion(std::string_view text);

struct ReleaseInfo
{
    Version     version;
    /** The release's page on GitHub, for people to read or to download from by hand. */
    std::string pageUrl;
    /** The installer attached to the release; empty if it has none. */
    std::string installerUrl;
    std::string installerName;
    /** Lower-case hex SHA-256 of the installer as GitHub recorded it; empty if GitHub gave none. */
    std::string installerSha256;
};

/**
 * Reads the body of GitHub's "latest release" API answer. Returns nothing if it is not a published
 * release with a version tag (a draft, a pre-release, an error document, or not JSON at all).
 */
std::optional<ReleaseInfo> parseLatestRelease(std::string_view json);

/**
 * True if @p url is a file attached to a release of @p repository ("owner/name") on GitHub. An
 * installer is only ever fetched from there, whatever a release document says.
 */
bool isReleaseDownload(std::string_view url, std::string_view repository);

/** @p name if it is a plain file name safe to create in a folder (no path, nothing odd); empty otherwise. */
std::string safeFileName(std::string_view name);

} // namespace bridge
