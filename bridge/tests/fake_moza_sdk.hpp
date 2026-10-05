#pragma once

// In-process stand-in for MOZA_SDK.dll: implements the C API from moza/moza.h against one
// imaginary flight base and records every command, so the bridge can be built and tested
// without Windows, the MOZA device service or hardware.

#include "moza/moza.h"

#include <map>
#include <vector>

namespace fake_moza {

struct Command
{
    int                    id = 0;
    /** Integer arguments in send order, with nested lists flattened. */
    std::vector<long long> args;
    bool                   sync = false;

    bool operator==(const Command& other) const { return id == other.id && args == other.args; }
};

struct State
{
    std::vector<Command>     commands;
    /** Readable parameters by id; anything absent reads as MOZA_ERR_NOT_SUPPORTED. */
    std::map<int, long long> parameters;
    /** What a synchronous MainCtrl_FfbControl send returns. */
    MOZA_ERRNO               controlSyncResult = MOZA_ERR_NO_ERROR;
    /** When false, MainCtrl_FfbNewEffect answers 0 (registration refused). */
    bool                     acceptEffects = true;
    /** Number of Moza_DeviceGetList calls that see no devices before the base appears. */
    int                      emptyDeviceListCalls = 0;
    int                      openHandles          = 0;
    bool                     initialized          = false;
};

State& state();

/** Restores the defaults above (FFB enabled, DirectInput mode, base present immediately). */
void reset();

} // namespace fake_moza
