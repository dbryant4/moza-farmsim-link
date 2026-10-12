#pragma once

// In-process stand-in for MOZA_SDK.dll: implements the C API from moza/moza.h against one
// imaginary flight base and records every command, so the bridge can be built and tested
// without Windows, the MOZA device service or hardware.

#include "moza/moza.h"

#include <map>
#include <utility>
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
    /** When true a yoke handle (a stick device, id 2) is listed after the base. */
    bool                     handlePresent        = false;
    /** Every parameter write as {id, value}, in order. A list write is recorded by its first value here. */
    std::vector<std::pair<int, long long>> parameterWrites;
    /** Parameters whose value is a list, such as an r,g,b colour; these win over `parameters`. */
    std::map<int, std::vector<long long>> listParameters;
    /** Every list write as {id, values}, in order. */
    std::vector<std::pair<int, std::vector<long long>>> listWrites;
    int                      openHandles          = 0;
    bool                     initialized          = false;
};

State& state();

/** Restores the defaults above (FFB enabled, DirectInput mode, base present immediately). */
void reset();

} // namespace fake_moza
