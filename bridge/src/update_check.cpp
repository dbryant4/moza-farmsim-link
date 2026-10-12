#include "update_check.hpp"

#include <cctype>
#include <charconv>
#include <memory>
#include <utility>
#include <vector>

namespace bridge {

namespace {

/** Just enough of JSON to walk GitHub's answer: every value kind is parsed, numbers are kept as text. */
struct Json
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };
    Kind                                       kind = Kind::Null;
    bool                                       flag = false;
    std::string                                text;
    std::vector<Json>                          items;
    std::vector<std::pair<std::string, Json>>  members;

    const Json* find(std::string_view key) const
    {
        for (const auto& [name, value] : members)
        {
            if (name == key)
            {
                return &value;
            }
        }
        return nullptr;
    }
    std::string string(std::string_view key) const
    {
        const Json* value = find(key);
        return value != nullptr && value->kind == Kind::String ? value->text : std::string();
    }
    bool boolean(std::string_view key) const
    {
        const Json* value = find(key);
        return value != nullptr && value->kind == Kind::Bool && value->flag;
    }
};

class Parser
{
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    bool parse(Json& out)
    {
        return value(out, 0) && (skipSpace(), m_at == m_text.size());
    }

private:
    // GitHub's documents nest a handful of levels; anything deeper is not one of them.
    static constexpr int kDeepest = 32;

    void skipSpace()
    {
        while (m_at < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_at])))
        {
            ++m_at;
        }
    }
    bool eat(char c)
    {
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == c)
        {
            ++m_at;
            return true;
        }
        return false;
    }
    bool word(std::string_view expected)
    {
        if (m_text.substr(m_at, expected.size()) == expected)
        {
            m_at += expected.size();
            return true;
        }
        return false;
    }

    bool string(std::string& out)
    {
        if (!eat('"'))
        {
            return false;
        }
        out.clear();
        while (m_at < m_text.size())
        {
            const char c = m_text[m_at++];
            if (c == '"')
            {
                return true;
            }
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (m_at >= m_text.size())
            {
                return false;
            }
            const char escaped = m_text[m_at++];
            switch (escaped)
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
                // Nothing this code looks at (tags, URLs, file names, hashes) is outside ASCII, so
                // an escaped character is only kept recognisable, not decoded.
                if (m_at + 4 > m_text.size())
                {
                    return false;
                }
                m_at += 4;
                out += '?';
                break;
            default: out += escaped; break;
            }
        }
        return false;
    }

    bool value(Json& out, int depth)
    {
        if (depth > kDeepest)
        {
            return false;
        }
        skipSpace();
        if (m_at >= m_text.size())
        {
            return false;
        }
        const char c = m_text[m_at];
        if (c == '"')
        {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        if (c == '{')
        {
            ++m_at;
            out.kind = Json::Kind::Object;
            if (eat('}'))
            {
                return true;
            }
            do
            {
                std::string key;
                Json        member;
                if (!string(key) || !eat(':') || !value(member, depth + 1))
                {
                    return false;
                }
                out.members.emplace_back(std::move(key), std::move(member));
            } while (eat(','));
            return eat('}');
        }
        if (c == '[')
        {
            ++m_at;
            out.kind = Json::Kind::Array;
            if (eat(']'))
            {
                return true;
            }
            do
            {
                Json item;
                if (!value(item, depth + 1))
                {
                    return false;
                }
                out.items.push_back(std::move(item));
            } while (eat(','));
            return eat(']');
        }
        if (word("true"))
        {
            out.kind = Json::Kind::Bool;
            out.flag = true;
            return true;
        }
        if (word("false"))
        {
            out.kind = Json::Kind::Bool;
            return true;
        }
        if (word("null"))
        {
            return true;
        }
        const std::size_t start = m_at;
        while (m_at < m_text.size() && (std::isdigit(static_cast<unsigned char>(m_text[m_at])) || std::string_view("+-.eE").find(m_text[m_at]) != std::string_view::npos))
        {
            ++m_at;
        }
        out.kind = Json::Kind::Number;
        out.text = std::string(m_text.substr(start, m_at - start));
        return m_at > start;
    }

    std::string_view m_text;
    std::size_t      m_at = 0;
};

bool endsWith(std::string_view text, std::string_view ending)
{
    return text.size() >= ending.size() && text.substr(text.size() - ending.size()) == ending;
}

} // namespace

std::optional<Version> parseVersion(std::string_view text)
{
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
    {
        text.remove_prefix(1);
    }
    Version version;
    int*    parts[] = {&version.major, &version.minor, &version.patch};
    for (int i = 0; i < 3; ++i)
    {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *parts[i]);
        if (error != std::errc() || end == text.data() || *parts[i] < 0)
        {
            return std::nullopt;
        }
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (i < 2)
        {
            if (text.empty() || text.front() != '.')
            {
                return std::nullopt;
            }
            text.remove_prefix(1);
        }
    }
    return text.empty() ? std::optional<Version>(version) : std::nullopt;
}

std::optional<ReleaseInfo> parseLatestRelease(std::string_view json)
{
    Json root;
    if (!Parser(json).parse(root) || root.kind != Json::Kind::Object || root.boolean("draft") || root.boolean("prerelease"))
    {
        return std::nullopt;
    }
    const auto version = parseVersion(root.string("tag_name"));
    if (!version)
    {
        return std::nullopt;
    }

    ReleaseInfo info;
    info.version = *version;
    info.pageUrl = root.string("html_url");
    if (const Json* assets = root.find("assets"); assets != nullptr && assets->kind == Json::Kind::Array)
    {
        for (const Json& asset : assets->items)
        {
            const std::string name = asset.string("name");
            const std::string url  = asset.string("browser_download_url");
            // Only ever a file from GitHub itself, over HTTPS, named like the installer this project publishes.
            if (!endsWith(name, "-setup.exe") || url.rfind("https://github.com/", 0) != 0)
            {
                continue;
            }
            info.installerName = name;
            info.installerUrl  = url;
            const std::string digest = asset.string("digest");
            if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64)
            {
                info.installerSha256 = digest.substr(7);
            }
            break;
        }
    }
    return info;
}

bool isReleaseDownload(std::string_view url, std::string_view repository)
{
    const std::string prefix = "https://github.com/" + std::string(repository) + "/releases/download/";
    return !repository.empty() && url.size() > prefix.size() && url.substr(0, prefix.size()) == prefix &&
           url.find_first_of(" \t\r\n\"'<>\\") == std::string_view::npos && url.find("..") == std::string_view::npos;
}

std::string safeFileName(std::string_view name)
{
    if (name.empty() || name.size() > 120 || name.front() == '.')
    {
        return {};
    }
    for (const char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
        {
            return {};
        }
    }
    return std::string(name);
}

} // namespace bridge
