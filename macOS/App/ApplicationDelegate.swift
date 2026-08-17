import AppKit
import Combine

@MainActor
final class ApplicationDelegate: NSObject, NSApplicationDelegate {
    var model: AppModel? {
        didSet {
            observeModel()
        }
    }

    private var termination = TerminationRequestCoordinator()
    private let callPrompt = DetectedCallPromptWindowController()
    private var modelSubscriptions = Set<AnyCancellable>()

    func applicationShouldTerminate(
        _ sender: NSApplication
    ) -> NSApplication.TerminateReply {
        callPrompt.close()
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

    private func observeModel() {
        modelSubscriptions.removeAll()
        guard let model else {
            callPrompt.close()
            return
        }

        // @Published delivers the new value from willSet. Consume the emitted
        // values directly instead of reading model properties that still hold
        // their previous values inside this callback.
        Publishers.CombineLatest4(
            model.$detectedCallProposal,
            model.$detectedCallAutoStopPrompt,
            model.$hasVaultSelection,
            model.$hasModelSelection
        )
            .combineLatest(model.$meetingLanguageMode)
            .sink {
                [weak self, weak model]
                values, languageMode in
                guard let self, let model else {
                    return
                }
                let (proposal, autoStopPrompt, hasVault, hasModel) = values
                self.refreshCallPrompt(
                    proposal: proposal,
                    autoStopPrompt: autoStopPrompt,
                    canStart: model.isRecordingEngineAvailable
                        && hasVault
                        && hasModel,
                    languageMode: languageMode
                )
            }
            .store(in: &modelSubscriptions)
    }

    private func refreshCallPrompt(
        proposal: DetectedCallProposal?,
        autoStopPrompt: DetectedCallAutoStopPrompt?,
        canStart: Bool,
        languageMode: CoreLanguageMode
    ) {
        guard let model else {
            callPrompt.close()
            return
        }

        if let autoStopPrompt {
            callPrompt.showAutoStop(
                prompt: autoStopPrompt,
                onKeepRecording: { [weak model] in
                    model?.keepRecordingAfterCallEndWarning(
                        promptID: autoStopPrompt.id
                    )
                },
                onStopNow: { [weak model] in
                    model?.stopNowAfterCallEndWarning(
                        promptID: autoStopPrompt.id
                    )
                }
            )
            return
        }

        guard let proposal else {
            callPrompt.close()
            return
        }

        callPrompt.show(
            proposal: proposal,
            canStart: canStart,
            languageMode: languageMode,
            onLanguageChange: { [weak model] mode in
                model?.meetingLanguageMode = mode
            },
            onStart: { [weak model] in
                model?.confirmDetectedCallStart(proposalID: proposal.id)
            },
            onDismiss: { [weak model] in
                model?.dismissDetectedCall(proposalID: proposal.id)
            }
        )
    }
}
