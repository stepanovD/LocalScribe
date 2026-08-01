struct TerminationRequestCoordinator {
    enum Reply: Equatable {
        case terminateNow
        case terminateLater
    }

    struct Decision: Equatable {
        let reply: Reply
        let shouldStartCleanup: Bool
    }

    private enum State {
        case idle
        case cleaningUp
        case replying
        case approved
    }

    private var state = State.idle

    mutating func request(requiresCleanup: Bool) -> Decision {
        switch state {
        case .idle where requiresCleanup:
            state = .cleaningUp
            return Decision(
                reply: .terminateLater,
                shouldStartCleanup: true
            )
        case .idle:
            state = .approved
            return Decision(
                reply: .terminateNow,
                shouldStartCleanup: false
            )
        case .cleaningUp, .replying:
            return Decision(
                reply: .terminateLater,
                shouldStartCleanup: false
            )
        case .approved:
            return Decision(
                reply: .terminateNow,
                shouldStartCleanup: false
            )
        }
    }

    mutating func cleanupDidFinish() -> Bool {
        guard case .cleaningUp = state else {
            return false
        }
        state = .replying
        return true
    }

    mutating func replyDidFinish() {
        guard case .replying = state else {
            return
        }
        state = .approved
    }
}
