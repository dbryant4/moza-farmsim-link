#include "mod_sync.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace bridge {

namespace {

bool sameContent(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code error;
    const auto      sizeA = std::filesystem::file_size(a, error);
    if (error)
    {
        return false;
    }
    const auto sizeB = std::filesystem::file_size(b, error);
    if (error || sizeA != sizeB)
    {
        return false;
    }
    std::ifstream first(a, std::ios::binary);
    std::ifstream second(b, std::ios::binary);
    return first && second &&
           std::equal(std::istreambuf_iterator<char>(first), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(second));
}

} // namespace

ModSync syncMod(const std::filesystem::path& shipped, const std::filesystem::path& gameProfile, bool gameRunning)
{
    std::error_code error;
    if (shipped.empty() || !std::filesystem::is_regular_file(shipped, error))
    {
        return ModSync::NoSource;
    }
    if (gameProfile.empty() || !std::filesystem::is_directory(gameProfile, error))
    {
        return ModSync::NoGameFolder;
    }

    const std::filesystem::path mods      = gameProfile / "mods";
    const std::filesystem::path installed = mods / shipped.filename();
    if (std::filesystem::exists(installed, error) && sameContent(shipped, installed))
    {
        return ModSync::UpToDate;
    }
    if (gameRunning)
    {
        return ModSync::GameRunning;
    }

    std::filesystem::create_directories(mods, error);
    // Copied beside the target and renamed over it, so the game never finds half a mod there.
    std::filesystem::path incoming = installed;
    incoming += ".new";
    std::filesystem::copy_file(shipped, incoming, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
    {
        return ModSync::Failed;
    }
    std::filesystem::rename(incoming, installed, error);
    if (error)
    {
        std::error_code ignored;
        std::filesystem::remove(incoming, ignored);
        return ModSync::Failed;
    }
    return ModSync::Updated;
}

const char* describe(ModSync result)
{
    switch (result)
    {
    case ModSync::NoSource: return "no mod file beside the program";
    case ModSync::NoGameFolder: return "Farming Simulator 25's folder was not found";
    case ModSync::UpToDate: return "up to date";
    case ModSync::Updated: return "installed the mod shipped with this version";
    case ModSync::GameRunning: return "out of date, but Farming Simulator is running; it will be replaced once the game has closed";
    default: return "could not be replaced";
    }
}

} // namespace bridge
