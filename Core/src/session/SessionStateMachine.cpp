#include "SessionStateMachine.hpp"

namespace localscribe {

SessionStateMachine::SessionStateMachine(ls_phase_t initialPhase)
    : phase_(initialPhase)
{
}

Expected<void> SessionStateMachine::transition(ls_phase_t next)
{
    if (!isLegal(phase_, next)) {
        return Error{
            LS_INVALID_STATE,
            "illegal session transition from " + std::string(name(phase_))
                + " to " + std::string(name(next))};
    }
    phase_ = next;
    return success();
}

bool SessionStateMachine::isLegal(ls_phase_t from, ls_phase_t to) noexcept
{
    if (from == to) {
        return false;
    }
    switch (from) {
    case LS_PHASE_PREPARING:
        return to == LS_PHASE_RECORDING || to == LS_PHASE_FAILED_TO_START
            || to == LS_PHASE_RECOVERY_REQUIRED;
    case LS_PHASE_RECORDING:
        return to == LS_PHASE_PAUSED || to == LS_PHASE_FINALIZING
            || to == LS_PHASE_RECOVERY_REQUIRED;
    case LS_PHASE_PAUSED:
        return to == LS_PHASE_RECORDING || to == LS_PHASE_FINALIZING
            || to == LS_PHASE_RECOVERY_REQUIRED;
    case LS_PHASE_FINALIZING:
        return to == LS_PHASE_COMPLETE || to == LS_PHASE_INCOMPLETE_SOURCES
            || to == LS_PHASE_INTERRUPTED
            || to == LS_PHASE_RECOVERY_REQUIRED;
    case LS_PHASE_RECOVERY_REQUIRED:
        return to == LS_PHASE_FINALIZING || to == LS_PHASE_PREPARING;
    default:
        return false;
    }
}

bool SessionStateMachine::isTerminal(ls_phase_t phase) noexcept
{
    return phase == LS_PHASE_COMPLETE
        || phase == LS_PHASE_INCOMPLETE_SOURCES
        || phase == LS_PHASE_INTERRUPTED
        || phase == LS_PHASE_FAILED_TO_START;
}

bool SessionStateMachine::isPersisted(ls_phase_t phase) noexcept
{
    return phase >= LS_PHASE_PREPARING
        && phase <= LS_PHASE_FAILED_TO_START;
}

ls_published_status_t
SessionStateMachine::publishedStatus(ls_phase_t phase) noexcept
{
    switch (phase) {
    case LS_PHASE_RECORDING:
    case LS_PHASE_PAUSED:
    case LS_PHASE_FINALIZING:
        return LS_PUBLISHED_STATUS_RECORDING;
    case LS_PHASE_COMPLETE:
        return LS_PUBLISHED_STATUS_COMPLETE;
    case LS_PHASE_INCOMPLETE_SOURCES:
        return LS_PUBLISHED_STATUS_INCOMPLETE_SOURCES;
    case LS_PHASE_RECOVERY_REQUIRED:
    case LS_PHASE_INTERRUPTED:
        return LS_PUBLISHED_STATUS_INTERRUPTED;
    default:
        return LS_PUBLISHED_STATUS_UNKNOWN;
    }
}

std::string_view SessionStateMachine::name(ls_phase_t phase) noexcept
{
    switch (phase) {
    case LS_PHASE_PREPARING:
        return "preparing";
    case LS_PHASE_RECORDING:
        return "recording";
    case LS_PHASE_PAUSED:
        return "paused";
    case LS_PHASE_FINALIZING:
        return "finalizing";
    case LS_PHASE_RECOVERY_REQUIRED:
        return "recovery_required";
    case LS_PHASE_COMPLETE:
        return "complete";
    case LS_PHASE_INCOMPLETE_SOURCES:
        return "incomplete_sources";
    case LS_PHASE_INTERRUPTED:
        return "interrupted";
    case LS_PHASE_FAILED_TO_START:
        return "failed_to_start";
    default:
        return "unknown";
    }
}

} // namespace localscribe
