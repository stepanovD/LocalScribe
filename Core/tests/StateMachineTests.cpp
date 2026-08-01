#include "TestSupport.hpp"

#include "../src/session/SessionStateMachine.hpp"

using namespace localscribe;

LS_TEST(state_machine_happy_path_and_published_status)
{
    SessionStateMachine machine(LS_PHASE_PREPARING);
    LS_CHECK(machine.transition(LS_PHASE_RECORDING));
    LS_CHECK_EQ(
        SessionStateMachine::publishedStatus(machine.phase()),
        LS_PUBLISHED_STATUS_RECORDING);
    LS_CHECK(machine.transition(LS_PHASE_PAUSED));
    LS_CHECK(machine.transition(LS_PHASE_RECORDING));
    LS_CHECK(machine.transition(LS_PHASE_FINALIZING));
    LS_CHECK(machine.transition(LS_PHASE_COMPLETE));
    LS_CHECK(SessionStateMachine::isTerminal(machine.phase()));
    LS_CHECK_EQ(
        SessionStateMachine::publishedStatus(machine.phase()),
        LS_PUBLISHED_STATUS_COMPLETE);
}

LS_TEST(state_machine_rejects_shortcuts_and_terminal_restart)
{
    SessionStateMachine preparing(LS_PHASE_PREPARING);
    LS_CHECK(!preparing.transition(LS_PHASE_COMPLETE));
    LS_CHECK_EQ(preparing.phase(), LS_PHASE_PREPARING);

    SessionStateMachine recording(LS_PHASE_RECORDING);
    LS_CHECK(!recording.transition(LS_PHASE_COMPLETE));
    LS_CHECK_EQ(recording.phase(), LS_PHASE_RECORDING);

    SessionStateMachine terminal(LS_PHASE_INTERRUPTED);
    LS_CHECK(!terminal.transition(LS_PHASE_RECORDING));
    LS_CHECK_EQ(terminal.phase(), LS_PHASE_INTERRUPTED);
}

LS_TEST(state_machine_recovery_can_only_finalize_or_reprepare)
{
    SessionStateMachine recovery(LS_PHASE_RECOVERY_REQUIRED);
    LS_CHECK(recovery.transition(LS_PHASE_FINALIZING));
    LS_CHECK(recovery.transition(LS_PHASE_INTERRUPTED));
    LS_CHECK_EQ(
        SessionStateMachine::publishedStatus(LS_PHASE_RECOVERY_REQUIRED),
        LS_PUBLISHED_STATUS_INTERRUPTED);
    LS_CHECK_EQ(
        SessionStateMachine::publishedStatus(LS_PHASE_PREPARING),
        LS_PUBLISHED_STATUS_UNKNOWN);
}
