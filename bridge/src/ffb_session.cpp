#include "ffb_session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace bridge {

namespace {

using namespace moza::as::wheelbase_type;

// MainCtrl_FfbControl values.
constexpr int kControlStart    = 1;
constexpr int kControlReset    = 2;
constexpr int kControlStopAll  = 3;

// enableAxis bits, in the order the SDK lists them for MainCtrl_FfbConstant: X, Y, DirectionEnable.
constexpr std::uint8_t kEnableAxisX = 1 << 0;
constexpr std::uint8_t kEnableAxisY = 1 << 1;
constexpr std::uint8_t kEnableDirection = 1 << 2;

constexpr int kControlTimeoutMs  = 200;
constexpr int kRegisterTimeoutMs = 500;

bool report(const char* what, moza::Errno err)
{
    if (err == MOZA_ERR_NO_ERROR)
    {
        return true;
    }
    std::fprintf(stderr, "%s failed: %s\n", what, errnoName(err));
    return false;
}

} // namespace

const char* errnoName(moza::Errno err)
{
    switch (err)
    {
    case MOZA_ERR_NO_ERROR: return "MOZA_ERR_NO_ERROR";
    case MOZA_ERR_UNKNOWN: return "MOZA_ERR_UNKNOWN";
    case MOZA_ERR_SERVICE_NOT_FOUND: return "MOZA_ERR_SERVICE_NOT_FOUND";
    case MOZA_ERR_SERVICE_VERSION_MISMATCH: return "MOZA_ERR_SERVICE_VERSION_MISMATCH";
    case MOZA_ERR_INVALID_ARGUMENT: return "MOZA_ERR_INVALID_ARGUMENT";
    case MOZA_ERR_NOT_INITIALIZED: return "MOZA_ERR_NOT_INITIALIZED";
    case MOZA_ERR_ALREADY_INITIALIZED: return "MOZA_ERR_ALREADY_INITIALIZED";
    case MOZA_ERR_BUFFER_TOO_SMALL: return "MOZA_ERR_BUFFER_TOO_SMALL";
    case MOZA_ERR_DEVICE_NOT_FOUND: return "MOZA_ERR_DEVICE_NOT_FOUND";
    case MOZA_ERR_DEVICE_NOT_OPEN: return "MOZA_ERR_DEVICE_NOT_OPEN";
    case MOZA_ERR_INVALID_HANDLE: return "MOZA_ERR_INVALID_HANDLE";
    case MOZA_ERR_BUSY: return "MOZA_ERR_BUSY";
    case MOZA_ERR_TIMEOUT: return "MOZA_ERR_TIMEOUT";
    case MOZA_ERR_NOT_SUPPORTED: return "MOZA_ERR_NOT_SUPPORTED";
    case MOZA_ERR_PARAMETER_READ_COMMAND_ERROR: return "MOZA_ERR_PARAMETER_READ_COMMAND_ERROR";
    case MOZA_ERR_PARAMETER_WRITE_COMMAND_ERROR: return "MOZA_ERR_PARAMETER_WRITE_COMMAND_ERROR";
    case MOZA_ERR_INDEX_OUT_OF_BOUNDS: return "MOZA_ERR_INDEX_OUT_OF_BOUNDS";
    case MOZA_ERR_PENDING: return "MOZA_ERR_PENDING";
    default: return "<unknown>";
    }
}

std::string formatValue(const moza::Value& value)
{
    switch (value.type())
    {
    case MOZA_VT_INT: return std::to_string(value.asInt());
    case MOZA_VT_UINT: return std::to_string(value.asUInt());
    case MOZA_VT_DOUBLE: return std::to_string(value.asDouble());
    case MOZA_VT_BOOL: return value.asBool() ? "true" : "false";
    case MOZA_VT_STRING: return '"' + value.asString() + '"';
    case MOZA_VT_BINARY: return "<" + std::to_string(value.asBinary().size()) + " bytes>";
    case MOZA_VT_LIST:
    {
        std::string text = "[";
        for (const auto& item : value.asList())
        {
            if (text.size() > 1)
            {
                text += ", ";
            }
            text += formatValue(item);
        }
        return text + "]";
    }
    default: return "<none>";
    }
}

std::optional<moza::DeviceInfo> findFlightBase(int timeoutMs, const std::string& nameFilter, const std::atomic<bool>* cancel)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        auto [err, devices] = moza::getDeviceList();
        if (err == MOZA_ERR_NO_ERROR)
        {
            for (const auto& info : devices)
            {
                // One product can report several type bits, so test the bit rather than equality.
                const bool isFlightBase = info.category == MOZA_DC_AS && (info.type & MOZA_DT_AS_WHEELBASE) != 0;
                if (isFlightBase && info.state != MOZA_DS_DISCONNECTED && std::strstr(info.name, nameFilter.c_str()) != nullptr)
                {
                    return info;
                }
            }
        }

        if ((cancel && cancel->load()) || std::chrono::steady_clock::now() >= deadline)
        {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

Condition conditionFromPercent(double percent, int fullScale)
{
    const double fraction = std::clamp(percent, 0.0, 100.0) / 100.0;
    const int    scale    = std::clamp(fullScale, 0, 65535);

    Condition condition;
    condition.coefficient = static_cast<std::int16_t>(std::min(std::lround(fraction * scale), 32767L));
    condition.saturation  = static_cast<std::uint16_t>(scale);
    return condition;
}

ForceAxes forceAlong(Axis axis)
{
    ForceAxes axes;
    axes.x               = axis == Axis::Roll;
    axes.y               = axis == Axis::Pitch;
    axes.directionEnable = true;
    axes.directionX      = axis == Axis::Roll ? 9000 : 0;
    return axes;
}

FfbSession::FfbSession(moza::as::Wheelbase& base, std::uint16_t durationMs, bool verbose)
    : m_base(base), m_durationMs(durationMs), m_verbose(verbose)
{
}

FfbSession::~FfbSession()
{
    stop();
}

bool FfbSession::control(int value, const char* what)
{
    // MainCtrl_FfbControl has no reply payload, so a synchronous send that times out waiting for
    // one may still have been delivered; only other errors count as failure.
    const moza::Errno err = m_base.cmdMainCtrlFfbControl().sendSync(value, kControlTimeoutMs);
    if (m_verbose)
    {
        std::printf("%s -> %s\n", what, errnoName(err));
    }
    return err == MOZA_ERR_TIMEOUT || report(what, err);
}

bool FfbSession::operate(std::uint8_t effect, MainCtrlFfbEffectOperationRequestOperation operation)
{
    MainCtrlFfbEffectOperationRequest request{};
    request.effectBlockIndex = effect;
    request.operation        = operation;
    return report("MainCtrl_FfbEffectOperation", m_base.cmdMainCtrlFfbEffectOperation().send(request));
}

bool FfbSession::begin()
{
    if (!control(kControlReset, "MainCtrl_FfbControl(reset)"))
    {
        return false;
    }
    m_effects.clear();
    m_active = true;
    return control(kControlStart, "MainCtrl_FfbControl(start)");
}

std::uint8_t FfbSession::addEffect(EffectType type)
{
    MainCtrlFfbNewEffectRequest request{};
    request.type  = type;
    request.index = static_cast<std::uint8_t>(m_effects.size() + 1);

    auto [err, index] = m_base.cmdMainCtrlFfbNewEffect().sendSync(request, kRegisterTimeoutMs);
    if (!report("MainCtrl_FfbNewEffect", err))
    {
        return 0;
    }
    if (index == 0)
    {
        std::fprintf(stderr, "MainCtrl_FfbNewEffect: the base refused effect type %d at index %d\n",
                     static_cast<int>(type), static_cast<int>(request.index));
        return 0;
    }
    if (m_verbose)
    {
        std::printf("MainCtrl_FfbNewEffect(type %d) -> index %d\n", static_cast<int>(type), static_cast<int>(index));
    }

    m_effects.push_back(index);
    return index;
}

bool FfbSession::setCondition(std::uint8_t effect, Axis axis, const Condition& condition)
{
    MainCtrlFfbConditionRequest request{};
    request.effectBlockIndex     = effect;
    request.parameterBlockOffset = static_cast<std::uint8_t>(axis);
    request.centerPointOffset    = condition.center;
    request.positiveCoefficient  = condition.coefficient;
    request.negativeCoefficient  = condition.coefficient;
    request.positiveSaturation   = condition.saturation;
    request.negativeSaturation   = condition.saturation;
    request.deadBand             = condition.deadBand;
    request.duration             = m_durationMs;
    request.enableAxis           = kEnableAxisX | kEnableAxisY;
    return report("MainCtrl_FfbCondition", m_base.cmdMainCtrlFfbCondition().send(request));
}

bool FfbSession::setConstant(std::uint8_t effect, std::int16_t magnitude, const ForceAxes& axes)
{
    // Sent as a raw argument list: the typed MainCtrlFfbConstantRequest nests the three enableAxis
    // flags in a sub-list, which SDK 1.0.0.4 fails to serialize (it logs a type mismatch, sends an
    // empty payload and still returns success). Passed flat they pack into the one enableAxis byte.
    const auto flag = [](bool on) { return moza::Value(static_cast<uint64_t>(on ? 1 : 0)); };
    const std::vector<moza::Value> arguments{
        moza::Value(static_cast<uint64_t>(effect)),
        moza::Value(static_cast<int64_t>(magnitude)),
        moza::Value(static_cast<uint64_t>(m_durationMs)),
        flag(axes.x),
        flag(axes.y),
        flag(axes.directionEnable),
        moza::Value(static_cast<uint64_t>(axes.directionX)),
        moza::Value(static_cast<uint64_t>(axes.directionY)),
    };
    return report("MainCtrl_FfbConstant",
                  m_base.command(moza::as::Wheelbase::kMainCtrlFfbConstantCommandId).send(arguments));
}

bool FfbSession::setPeriodic(std::uint8_t effect, std::int16_t magnitude, std::uint32_t periodMs, const ForceAxes& axes,
                             std::uint16_t durationMs)
{
    MainCtrlFfbPeriodicRequest request{};
    request.effectBlockIndex = effect;
    request.magnitude        = magnitude;
    request.period           = periodMs;
    request.duration         = durationMs != 0 ? durationMs : m_durationMs;
    // A plain bitmask here, unlike the constant-force command's nested flags, so the typed request serializes.
    request.enableAxis = static_cast<std::uint8_t>((axes.x ? kEnableAxisX : 0) | (axes.y ? kEnableAxisY : 0) |
                                                   (axes.directionEnable ? kEnableDirection : 0));
    request.directionX = axes.directionX;
    request.directionY = axes.directionY;
    return report("MainCtrl_FfbPeriodic", m_base.cmdMainCtrlFfbPeriodic().send(request));
}

bool FfbSession::start(std::uint8_t effect)
{
    return operate(effect, MainCtrlFfbEffectOperationRequestOperation::Start);
}

void FfbSession::stop()
{
    if (!m_active)
    {
        return;
    }
    m_active = false;

    for (const std::uint8_t effect : m_effects)
    {
        operate(effect, MainCtrlFfbEffectOperationRequestOperation::Stop);
        operate(effect, MainCtrlFfbEffectOperationRequestOperation::Free);
    }
    m_effects.clear();

    // Sent synchronously so the queued stop/free commands are flushed before the caller closes the device.
    control(kControlStopAll, "MainCtrl_FfbControl(stop all)");
}

} // namespace bridge
