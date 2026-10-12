#pragma once

#include <filesystem>

namespace bridge {

enum class ModSync
{
    /** There is no mod file beside the program to install (a copy run from a build folder, say). */
    NoSource,
    /** Farming Simulator's profile folder does not exist: the game is not installed for this user. */
    NoGameFolder,
    UpToDate,
    Updated,
    /** The game is running and has the installed mod open; try again once it has closed. */
    GameRunning,
    Failed,
};

/**
 * Makes the mod in Farming Simulator's mods folder the same as the copy shipped beside the program.
 *
 * An update replaces the program at once but cannot always replace the mod, because the game keeps
 * the mod's file open while it runs. So the program does it itself, whenever it finds the two
 * differ and the game is not running.
 */
ModSync syncMod(const std::filesystem::path& shipped, const std::filesystem::path& gameProfile, bool gameRunning);

/** For logs. */
const char* describe(ModSync result);

} // namespace bridge
