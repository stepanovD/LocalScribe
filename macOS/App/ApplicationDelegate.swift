import AppKit

@MainActor
final class ApplicationDelegate: NSObject, NSApplicationDelegate {
    weak var model: AppModel?

    private var termination = TerminationRequestCoordinator()

    func applicationShouldTerminate(
        _ sender: NSApplication
    ) -> NSApplication.TerminateReply {
        let model = model
        let decision = termination.request(requiresCleanup: model != nil)

        switch decision.reply {
        case .terminateNow:
            return .terminateNow
        case .terminateLater:
            break
        }

        guard decision.shouldStartCleanup, let model else {
            return .terminateLater
        }

        Task {
            await model.prepareForTermination()
            guard termination.cleanupDidFinish() else {
                return
            }
            sender.reply(toApplicationShouldTerminate: true)
            termination.replyDidFinish()
        }
        return .terminateLater
    }
}
