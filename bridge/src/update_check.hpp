#pragma once

// Working out whether a newer release exists, from what GitHub's API says. No network access here:
// the Windows side fetches the text and this decides what it means, so it can be tested anywhere.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bridge {

struct Version
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    /** 0 for the finished version; N for its Nth pre-release, written "-preN". */
    int pre = 0;

    bool operator==(const Version&) const = default;
    /** By number, and within one number the pre-releases in order and then the finished version: 0.1.3-pre1 < 0.1.3-pre2 < 0.1.3. */
    std::strong_ordering operator<=>(const Version& other) const;
    std::string          text() const;
};

/**
 * Reads "0.1.2" or "0.1.3-pre1", with or without a leading "v". Anything else, including other
 * suffixes, is not a version to update to.
 */
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
    /** A "-preN" version, or marked "pre-release" on GitHub: only for those who asked for pre-releases. */
    bool prerelease = false;
    /** When it was published, seconds since 1970 UTC; 0 if GitHub did not say. */
    std::int64_t publishedAt = 0;
};

/**
 * Reads the body of GitHub's "latest release" API answer. Returns nothing if it is not a published
 * release with a version tag (a draft, a pre-release, an error document, or not JSON at all).
 */
std::optional<ReleaseInfo> parseLatestRelease(std::string_view json);

/**
 * Reads the body of GitHub's "list releases" API answer and returns the release with the highest
 * version, or nothing if there is none to offer. Drafts never count; releases marked pre-release
 * count only with @p includePrereleases. Entries without a version tag are passed over.
 */
std::optional<ReleaseInfo> newestRelease(std::string_view json, bool includePrereleases);

/** Reads a time as GitHub writes it, "2026-10-11T22:20:05Z", into seconds since 1970 UTC. */
std::optional<std::int64_t> parseTimestamp(std::string_view text);

/**
 * How long after publishing a release is left alone, seconds. A release is made in steps (created,
 * marked as a pre-release, files attached), and for a moment it can look like something it is
 * not: a normal release that is about to be marked pre-release, or one with no installer yet.
 */
constexpr std::int64_t kReleaseSettleSeconds = 15 * 60;

/**
 * 0 once @p release has been out long enough to act on (or GitHub did not say when it came out);
 * otherwise the number of seconds still to wait.
 */
std::int64_t secondsUntilSettled(const ReleaseInfo& release, std::int64_t nowSeconds);

/**
 * True if @p url is a file attached to a release of @p repository ("owner/name") on GitHub. An
 * installer is only ever fetched from there, whatever a release document says.
 */
bool isReleaseDownload(std::string_view url, std::string_view repository);

/** @p name if it is a plain file name safe to create in a folder (no path, nothing odd); empty otherwise. */
std::string safeFileName(std::string_view name);

} // namespace bridge
