#include "fake_moza_sdk.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fake_moza {

namespace {

constexpr MOZA_DEVICE_ID kDeviceId = 1;

constexpr int kFfbNewEffectCommandId = 603;
constexpr int kFfbControlCommandId   = 610;

void flatten(const MOZA_VALUE* value, std::vector<long long>& out)
{
    if (!value)
    {
        return;
    }
    switch (value->type)
    {
    case MOZA_VT_INT: out.push_back(value->data.intValue); break;
    case MOZA_VT_UINT: out.push_back(static_cast<long long>(value->data.uintValue)); break;
    case MOZA_VT_BOOL: out.push_back(value->data.boolValue); break;
    case MOZA_VT_LIST:
        for (MOZA_SIZE i = 0; i < value->data.listValue.size; ++i)
        {
            flatten(&value->data.listValue.items[i], out);
        }
        break;
    default: break;
    }
}

void setInt(MOZA_VALUE* value, bool isUnsigned, long long number)
{
    if (!value)
    {
        return;
    }
    Moza_ValueDestroy(value);
    value->type          = isUnsigned ? MOZA_VT_UINT : MOZA_VT_INT;
    value->data.intValue = number;
}

MOZA_DEVICE_INFO deviceInfo()
{
    MOZA_DEVICE_INFO info{};
    info.id = kDeviceId;
    std::snprintf(info.name, sizeof(info.name), "Fake Yoke Base");
    info.vendorId       = 0x346E;
    info.category       = MOZA_DC_AS;
    info.type           = MOZA_DT_AS_WHEELBASE;
    info.state          = MOZA_DS_NORMAL;
    info.hidDeviceCount = 1;
    return info;
}

constexpr MOZA_DEVICE_ID kHandleDeviceId = 2;

MOZA_DEVICE_INFO handleInfo()
{
    MOZA_DEVICE_INFO info{};
    info.id = kHandleDeviceId;
    std::snprintf(info.name, sizeof(info.name), "Fake Yoke Handle");
    info.vendorId = 0x346E;
    info.category = MOZA_DC_AS;
    info.type     = MOZA_DT_AS_STICK;
    info.state    = MOZA_DS_NORMAL;
    return info;
}

MOZA_ERRNO checkHandle(MOZA_DEVICE_HANDLE handle)
{
    if (!state().initialized)
    {
        return MOZA_ERR_NOT_INITIALIZED;
    }
    return handle == MOZA_INVALID_DEVICE_HANDLE ? MOZA_ERR_INVALID_HANDLE : MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO readParameter(MOZA_DEVICE_HANDLE handle, MOZA_PARAMETER_ID id, MOZA_VALUE* value)
{
    if (const MOZA_ERRNO err = checkHandle(handle); err != MOZA_ERR_NO_ERROR)
    {
        return err;
    }
    if (const auto list = state().listParameters.find(id); list != state().listParameters.end())
    {
        if (value != nullptr && Moza_ValueCreate(MOZA_VT_LIST, value, static_cast<MOZA_SIZE>(list->second.size())))
        {
            for (std::size_t i = 0; i < list->second.size(); ++i)
            {
                value->data.listValue.items[i].type           = MOZA_VT_UINT;
                value->data.listValue.items[i].data.uintValue = static_cast<uint64_t>(list->second[i]);
            }
        }
        return MOZA_ERR_NO_ERROR;
    }
    const auto found = state().parameters.find(id);
    if (found == state().parameters.end())
    {
        return MOZA_ERR_NOT_SUPPORTED;
    }
    setInt(value, false, found->second);
    return MOZA_ERR_NO_ERROR;
}

template <typename Info>
MOZA_ERRNO emptyList(MOZA_DEVICE_HANDLE handle, Info*, MOZA_SIZE* size)
{
    if (const MOZA_ERRNO err = checkHandle(handle); err != MOZA_ERR_NO_ERROR)
    {
        return err;
    }
    if (!size)
    {
        return MOZA_ERR_INVALID_ARGUMENT;
    }
    *size = 0;
    return MOZA_ERR_NO_ERROR;
}

} // namespace

State& state()
{
    static State instance;
    return instance;
}

void reset()
{
    state()            = State{};
    state().parameters = {
        {312, 1}, // Main_CtrlFfbEnable
        {53, 1},  // Main_CtrlModeSet: DirectInput FFB
        {95, 0},  // Main_Steer_TorqueOutX
        {96, 0},  // Main_Steer_TorqueOutY
    };
}

} // namespace fake_moza

using fake_moza::state;

extern "C" {

MOZA_ERRNO Moza_Initialize()
{
    if (state().initialized)
    {
        return MOZA_ERR_ALREADY_INITIALIZED;
    }
    if (state().parameters.empty())
    {
        fake_moza::reset();
    }
    state().initialized = true;
    return MOZA_ERR_NO_ERROR;
}

uint32_t Moza_GetSdkVersionNumber()
{
    return 0;
}

const char* Moza_GetSdkVersionString()
{
    return "fake";
}

void Moza_Shutdown()
{
    state().initialized = false;
}

MOZA_ERRNO Moza_SetEventCallback(MOZA_EVENT_CALLBACK, void*)
{
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_SetLogCallback(MOZA_LOG_CALLBACK, void*)
{
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_SetHidAxisEventMinInterval(MOZA_SIZE)
{
    return MOZA_ERR_NO_ERROR;
}

MOZA_BOOL Moza_ValueCreate(MOZA_VALUE_TYPE type, MOZA_VALUE* value, MOZA_SIZE size)
{
    if (!value)
    {
        return MOZA_FALSE;
    }
    Moza_ValueDestroy(value);

    switch (type)
    {
    case MOZA_VT_STRING:
        value->data.stringValue.data = static_cast<char*>(std::calloc(size + 1, 1));
        value->data.stringValue.size = size;
        break;
    case MOZA_VT_BINARY:
        value->data.binaryValue.data = static_cast<uint8_t*>(std::calloc(size ? size : 1, 1));
        value->data.binaryValue.size = size;
        break;
    case MOZA_VT_LIST:
        value->data.listValue.items = static_cast<MOZA_VALUE*>(std::calloc(size ? size : 1, sizeof(MOZA_VALUE)));
        value->data.listValue.size  = size;
        break;
    default: return MOZA_FALSE;
    }
    value->type = type;
    return MOZA_TRUE;
}

MOZA_BOOL Moza_ValueCopy(const MOZA_VALUE* source, MOZA_VALUE* destination)
{
    if (!source || !destination)
    {
        return MOZA_FALSE;
    }
    if (source == destination)
    {
        return MOZA_TRUE;
    }

    switch (source->type)
    {
    case MOZA_VT_STRING:
        if (!Moza_ValueCreate(MOZA_VT_STRING, destination, source->data.stringValue.size))
        {
            return MOZA_FALSE;
        }
        std::memcpy(destination->data.stringValue.data, source->data.stringValue.data, source->data.stringValue.size);
        break;
    case MOZA_VT_BINARY:
        if (!Moza_ValueCreate(MOZA_VT_BINARY, destination, source->data.binaryValue.size))
        {
            return MOZA_FALSE;
        }
        std::memcpy(destination->data.binaryValue.data, source->data.binaryValue.data, source->data.binaryValue.size);
        break;
    case MOZA_VT_LIST:
        if (!Moza_ValueCreate(MOZA_VT_LIST, destination, source->data.listValue.size))
        {
            return MOZA_FALSE;
        }
        for (MOZA_SIZE i = 0; i < source->data.listValue.size; ++i)
        {
            if (!Moza_ValueCopy(&source->data.listValue.items[i], &destination->data.listValue.items[i]))
            {
                return MOZA_FALSE;
            }
        }
        break;
    default:
        Moza_ValueDestroy(destination);
        std::memcpy(destination, source, sizeof(MOZA_VALUE));
        break;
    }
    return MOZA_TRUE;
}

void Moza_ValueDestroy(MOZA_VALUE* value)
{
    if (!value)
    {
        return;
    }
    switch (value->type)
    {
    case MOZA_VT_STRING: std::free(value->data.stringValue.data); break;
    case MOZA_VT_BINARY: std::free(value->data.binaryValue.data); break;
    case MOZA_VT_LIST:
        for (MOZA_SIZE i = 0; i < value->data.listValue.size; ++i)
        {
            Moza_ValueDestroy(&value->data.listValue.items[i]);
        }
        std::free(value->data.listValue.items);
        break;
    default: break;
    }
    std::memset(value, 0, sizeof(MOZA_VALUE));
}

MOZA_ERRNO Moza_DeviceGetList(MOZA_DEVICE_INFO infos[], MOZA_SIZE* infosSize)
{
    if (!state().initialized)
    {
        return MOZA_ERR_NOT_INITIALIZED;
    }
    if (!infosSize)
    {
        return MOZA_ERR_INVALID_ARGUMENT;
    }

    // The count query and the fill that follows it must agree, so only the query consumes a delay.
    if (!infos && state().emptyDeviceListCalls > 0)
    {
        --state().emptyDeviceListCalls;
        *infosSize = 0;
        return MOZA_ERR_NO_ERROR;
    }

    const MOZA_SIZE capacity = *infosSize;
    const MOZA_SIZE count    = state().handlePresent ? 2 : 1;
    *infosSize               = count;
    if (!infos)
    {
        return MOZA_ERR_NO_ERROR;
    }
    if (capacity < count)
    {
        return MOZA_ERR_BUFFER_TOO_SMALL;
    }
    infos[0] = fake_moza::deviceInfo();
    if (count > 1)
    {
        infos[1] = fake_moza::handleInfo();
    }
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_DeviceGetInfoById(MOZA_DEVICE_ID deviceId, MOZA_DEVICE_INFO* info)
{
    if (deviceId != fake_moza::kDeviceId || !info)
    {
        return MOZA_ERR_DEVICE_NOT_FOUND;
    }
    *info = fake_moza::deviceInfo();
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_DeviceOpen(MOZA_DEVICE_ID deviceId, MOZA_DEVICE_HANDLE* handle)
{
    if (!state().initialized)
    {
        return MOZA_ERR_NOT_INITIALIZED;
    }
    const bool known = deviceId == fake_moza::kDeviceId || (deviceId == fake_moza::kHandleDeviceId && state().handlePresent);
    if (!known || !handle)
    {
        return MOZA_ERR_DEVICE_NOT_FOUND;
    }
    *handle = reinterpret_cast<MOZA_DEVICE_HANDLE>(static_cast<uintptr_t>(0x1000 + ++state().openHandles));
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_DeviceClose(MOZA_DEVICE_HANDLE handle)
{
    if (handle == MOZA_INVALID_DEVICE_HANDLE)
    {
        return MOZA_ERR_INVALID_HANDLE;
    }
    --state().openHandles;
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_DeviceParameterGetIdByName(MOZA_DEVICE_HANDLE, const char*, MOZA_PARAMETER_ID*)
{
    return MOZA_ERR_NOT_SUPPORTED;
}

MOZA_ERRNO Moza_DeviceParameterGetInfoById(MOZA_DEVICE_HANDLE, MOZA_PARAMETER_ID, MOZA_PARAMETER_INFO*)
{
    return MOZA_ERR_NOT_SUPPORTED;
}

MOZA_ERRNO Moza_DeviceParameterGetInfoByName(MOZA_DEVICE_HANDLE, const char*, MOZA_PARAMETER_INFO*)
{
    return MOZA_ERR_NOT_SUPPORTED;
}

MOZA_ERRNO Moza_DeviceParameterGetList(MOZA_DEVICE_HANDLE handle, MOZA_PARAMETER_INFO infos[], MOZA_SIZE* infosSize)
{
    return fake_moza::emptyList(handle, infos, infosSize);
}

MOZA_ERRNO Moza_DeviceParameterFetch(MOZA_DEVICE_HANDLE handle, MOZA_PARAMETER_ID)
{
    return fake_moza::checkHandle(handle);
}

MOZA_ERRNO Moza_DeviceParameterGetValue(MOZA_DEVICE_HANDLE handle, MOZA_PARAMETER_ID parameterId, MOZA_VALUE* value)
{
    return fake_moza::readParameter(handle, parameterId, value);
}

MOZA_ERRNO Moza_DeviceParameterFetchSync(MOZA_DEVICE_HANDLE handle, MOZA_PARAMETER_ID parameterId, int32_t, MOZA_VALUE* value)
{
    return fake_moza::readParameter(handle, parameterId, value);
}

MOZA_ERRNO Moza_DeviceParameterGetValueSync(MOZA_DEVICE_HANDLE handle, MOZA_PARAMETER_ID parameterId, int32_t, MOZA_VALUE* value)
{
    return fake_moza::readParameter(handle, parameterId, value);
}

MOZA_ERRNO Moza_DeviceParameterSetValue(MOZA_DEVICE_HANDLE handle, MOZA_PARAMETER_ID parameterId, const MOZA_VALUE* value)
{
    if (const MOZA_ERRNO err = fake_moza::checkHandle(handle); err != MOZA_ERR_NO_ERROR)
    {
        return err;
    }
    std::vector<long long> numbers;
    fake_moza::flatten(value, numbers);
    if (numbers.empty())
    {
        return MOZA_ERR_INVALID_ARGUMENT;
    }
    if (numbers.size() > 1)
    {
        state().listParameters[parameterId] = numbers;
        state().listWrites.emplace_back(parameterId, numbers);
    }
    else
    {
        state().parameters[parameterId] = numbers.front();
    }
    state().parameterWrites.emplace_back(parameterId, numbers.front());
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_DeviceParameterSetValueSync(MOZA_DEVICE_HANDLE handle, MOZA_PARAMETER_ID parameterId, const MOZA_VALUE* value,
                                            int32_t, MOZA_VALUE* resultValue)
{
    const MOZA_ERRNO err = Moza_DeviceParameterSetValue(handle, parameterId, value);
    return err == MOZA_ERR_NO_ERROR ? fake_moza::readParameter(handle, parameterId, resultValue) : err;
}

MOZA_ERRNO Moza_DeviceCommandSend(MOZA_DEVICE_HANDLE handle, MOZA_COMMAND_ID commandId, const MOZA_VALUE* arguments)
{
    if (const MOZA_ERRNO err = fake_moza::checkHandle(handle); err != MOZA_ERR_NO_ERROR)
    {
        return err;
    }
    fake_moza::Command command;
    command.id = commandId;
    fake_moza::flatten(arguments, command.args);
    state().commands.push_back(command);
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_DeviceCommandSendSync(MOZA_DEVICE_HANDLE handle, MOZA_COMMAND_ID commandId, const MOZA_VALUE* arguments,
                                      int32_t, MOZA_VALUE* resultValue)
{
    if (const MOZA_ERRNO err = Moza_DeviceCommandSend(handle, commandId, arguments); err != MOZA_ERR_NO_ERROR)
    {
        return err;
    }
    fake_moza::Command& command = state().commands.back();
    command.sync                = true;

    if (commandId == fake_moza::kFfbControlCommandId)
    {
        return state().controlSyncResult;
    }
    if (commandId == fake_moza::kFfbNewEffectCommandId)
    {
        const long long index = command.args.size() >= 2 ? command.args[1] : 1;
        fake_moza::setInt(resultValue, true, state().acceptEffects ? index : 0);
    }
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_DeviceCommandGetInfoById(MOZA_DEVICE_HANDLE, MOZA_COMMAND_ID, MOZA_COMMAND_INFO*)
{
    return MOZA_ERR_NOT_SUPPORTED;
}

MOZA_ERRNO Moza_DeviceCommandGetInfoByName(MOZA_DEVICE_HANDLE, const char*, MOZA_COMMAND_INFO*)
{
    return MOZA_ERR_NOT_SUPPORTED;
}

MOZA_ERRNO Moza_DeviceCommandGetList(MOZA_DEVICE_HANDLE handle, MOZA_COMMAND_INFO infos[], MOZA_SIZE* infosSize)
{
    return fake_moza::emptyList(handle, infos, infosSize);
}

MOZA_ERRNO Moza_DeviceHidGetList(MOZA_DEVICE_HANDLE handle, MOZA_HID_DEVICE_INFO infos[], MOZA_SIZE* infosSize)
{
    if (const MOZA_ERRNO err = fake_moza::checkHandle(handle); err != MOZA_ERR_NO_ERROR)
    {
        return err;
    }
    if (!infosSize)
    {
        return MOZA_ERR_INVALID_ARGUMENT;
    }
    const MOZA_SIZE capacity = *infosSize;
    *infosSize               = 1;
    if (!infos)
    {
        return MOZA_ERR_NO_ERROR;
    }
    if (capacity < 1)
    {
        return MOZA_ERR_BUFFER_TOO_SMALL;
    }

    MOZA_HID_DEVICE_INFO info{};
    info.vendorId = 0x346E;
    std::snprintf(info.productName, sizeof(info.productName), "Fake Yoke Base HID");
    info.axisCount = 2;
    infos[0]       = info;
    return MOZA_ERR_NO_ERROR;
}

MOZA_ERRNO Moza_DeviceHidOpen(MOZA_DEVICE_HANDLE handle, uint8_t hidIndex)
{
    const MOZA_ERRNO err = fake_moza::checkHandle(handle);
    return err != MOZA_ERR_NO_ERROR ? err : hidIndex == 0 ? MOZA_ERR_NO_ERROR : MOZA_ERR_INDEX_OUT_OF_BOUNDS;
}

MOZA_ERRNO Moza_DeviceHidClose(MOZA_DEVICE_HANDLE handle, uint8_t)
{
    return fake_moza::checkHandle(handle);
}

MOZA_ERRNO Moza_DeviceHidGetInputState(MOZA_DEVICE_HANDLE handle, uint8_t hidIndex, MOZA_HID_INPUT_STATE* inputState)
{
    if (const MOZA_ERRNO err = fake_moza::checkHandle(handle); err != MOZA_ERR_NO_ERROR)
    {
        return err;
    }
    if (hidIndex != 0 || !inputState)
    {
        return MOZA_ERR_INVALID_ARGUMENT;
    }
    std::memset(inputState, 0, sizeof(*inputState));
    inputState->axes[MOZA_HA_X].normalizedValue = 0.5f;
    inputState->axes[MOZA_HA_Y].normalizedValue = 0.5f;
    return MOZA_ERR_NO_ERROR;
}

} // extern "C"
