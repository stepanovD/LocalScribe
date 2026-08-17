import AppKit
import SwiftUI

@MainActor
final class DetectedCallPromptWindowController: NSObject {
    private enum Layout {
        static let windowSize = NSSize(width: 540, height: 96)
        static let horizontalScreenInset: CGFloat = 4
        static let verticalScreenInset: CGFloat = 2
        static let entryOffset: CGFloat = 8
    }

    enum PresentationID: Hashable {
        case start(UUID)
        case autoStop(UUID)
    }

    private var panel: DetectedCallBannerPanel?
    private var hostingController:
        NSHostingController<AnyView>?
    private var presentationID: PresentationID?
    private var resolvedPresentationID: PresentationID?
    private var startAction: (() -> Void)?
    private var dismissAction: (() -> Void)?

    func show(
        proposal: DetectedCallProposal,
        canStart: Bool,
        languageMode: CoreLanguageMode,
        onLanguageChange: @escaping (CoreLanguageMode) -> Void,
        onStart: @escaping () -> Void,
        onDismiss: @escaping () -> Void
    ) {
        let presentationID = PresentationID.start(proposal.id)
        guard resolvedPresentationID != presentationID else {
            return
        }

        let isNewPresentation = self.presentationID != presentationID
        if isNewPresentation {
            invalidateCurrentPresentation()
            self.presentationID = presentationID
        }
        startAction = onStart
        dismissAction = onDismiss

        let rootView = AnyView(DetectedCallPromptView(
            applicationName: proposal.applicationName,
            canStart: canStart,
            languageMode: languageMode,
            onLanguageChange: onLanguageChange,
            onStart: { [weak self] in
                self?.resolveWithStart()
            },
            onDismiss: { [weak self] in
                self?.resolveWithDismiss()
            }
        ))

        let panel = panel ?? makePanel()
        if let hostingController {
            hostingController.rootView = rootView
        } else {
            let hostingController = NSHostingController(rootView: rootView)
            self.hostingController = hostingController
            panel.contentViewController = hostingController
        }

        panel.onCancel = { [weak self] in
            self?.resolveWithDismiss()
        }
        panel.setAccessibilityLabel(
            "Meeting detected in \(proposal.applicationName)"
        )

        let targetFrame = targetFrame(for: panel)
        if isNewPresentation || !panel.isVisible {
            present(panel, at: targetFrame)
            announce(proposal)
        } else {
            panel.setFrame(targetFrame, display: true)
            panel.orderFrontRegardless()
        }
    }

    func showAutoStop(
        prompt: DetectedCallAutoStopPrompt,
        onKeepRecording: @escaping () -> Void,
        onStopNow: @escaping () -> Void
    ) {
        let presentationID = PresentationID.autoStop(prompt.id)
        guard resolvedPresentationID != presentationID else {
            return
        }

        let isNewPresentation = self.presentationID != presentationID
        if isNewPresentation {
            invalidateCurrentPresentation()
            self.presentationID = presentationID
        }
        startAction = onStopNow
        dismissAction = onKeepRecording

        let rootView = DetectedCallAutoStopPromptView(
            applicationName: prompt.proposal.applicationName,
            deadline: prompt.deadline,
            onKeepRecording: { [weak self] in
                self?.resolveWithDismiss()
            },
            onStopNow: { [weak self] in
                self?.resolveWithStart()
            }
        )

        let panel = panel ?? makePanel()
        if let hostingController {
            hostingController.rootView = AnyView(rootView)
        } else {
            let hostingController = NSHostingController(
                rootView: AnyView(rootView)
            )
            self.hostingController = hostingController
            panel.contentViewController = hostingController
        }
        panel.onCancel = { [weak self] in
            self?.resolveWithDismiss()
        }
        panel.setAccessibilityLabel(
            "Call appears to have ended in \(prompt.proposal.applicationName)"
        )

        let targetFrame = targetFrame(for: panel)
        if isNewPresentation || !panel.isVisible {
            present(panel, at: targetFrame)
            announceAutoStop(prompt)
        } else {
            panel.setFrame(targetFrame, display: true)
            panel.orderFrontRegardless()
        }
    }

    func close() {
        if let presentationID {
            resolvedPresentationID = presentationID
        }
        invalidateCurrentPresentation()
        hidePanel()
    }

    private func makePanel() -> DetectedCallBannerPanel {
        let panel = DetectedCallBannerPanel(
            contentRect: NSRect(origin: .zero, size: Layout.windowSize),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        panel.isReleasedWhenClosed = false
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.becomesKeyOnlyIfNeeded = true
        panel.worksWhenModal = true
        panel.level = .statusBar
        panel.collectionBehavior = [
            .canJoinAllSpaces,
            .fullScreenAuxiliary,
            .ignoresCycle,
        ]
        panel.animationBehavior = .none
        panel.isMovable = false
        panel.isExcludedFromWindowsMenu = true
        self.panel = panel
        return panel
    }

    private func targetFrame(for panel: NSPanel) -> NSRect {
        let screen = targetScreen(for: panel)
        let visibleFrame = screen?.visibleFrame ?? .zero
        return NSRect(
            x: visibleFrame.maxX
                - Layout.windowSize.width
                - Layout.horizontalScreenInset,
            y: visibleFrame.maxY
                - Layout.windowSize.height
                - Layout.verticalScreenInset,
            width: Layout.windowSize.width,
            height: Layout.windowSize.height
        )
    }

    private func targetScreen(for panel: NSPanel) -> NSScreen? {
        let mouseLocation = NSEvent.mouseLocation
        return NSScreen.screens.first(where: {
            NSMouseInRect(mouseLocation, $0.frame, false)
        }) ?? panel.screen ?? NSScreen.main ?? NSScreen.screens.first
    }

    private func present(_ panel: NSPanel, at targetFrame: NSRect) {
        let reduceMotion = NSWorkspace.shared
            .accessibilityDisplayShouldReduceMotion
        panel.alphaValue = reduceMotion ? 1 : 0
        panel.setFrame(
            targetFrame.offsetBy(
                dx: 0,
                dy: reduceMotion ? 0 : Layout.entryOffset
            ),
            display: true
        )
        panel.orderFrontRegardless()

        guard !reduceMotion else {
            return
        }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.22
            context.allowsImplicitAnimation = true
            panel.animator().alphaValue = 1
            panel.animator().setFrame(targetFrame, display: true)
        }
    }

    private func announce(_ proposal: DetectedCallProposal) {
        guard let panel else {
            return
        }
        NSAccessibility.post(
            element: panel,
            notification: .announcementRequested,
            userInfo: [
                .announcement:
                    "Meeting detected in \(proposal.applicationName). "
                    + "Start recording?",
                .priority: NSAccessibilityPriorityLevel.high.rawValue,
            ]
        )
    }

    private func announceAutoStop(_ prompt: DetectedCallAutoStopPrompt) {
        guard let panel else {
            return
        }
        NSAccessibility.post(
            element: panel,
            notification: .announcementRequested,
            userInfo: [
                .announcement:
                    "The \(prompt.proposal.applicationName) call appears to "
                    + "have ended. Recording will stop in 10 seconds unless you "
                    + "choose Keep Recording.",
                .priority: NSAccessibilityPriorityLevel.high.rawValue,
            ]
        )
    }

    private func resolveWithStart() {
        guard let presentationID,
              resolvedPresentationID != presentationID
        else {
            return
        }
        resolvedPresentationID = presentationID
        let action = startAction
        invalidateCurrentPresentation()
        hidePanel()
        action?()
    }

    private func resolveWithDismiss() {
        guard let presentationID,
              resolvedPresentationID != presentationID
        else {
            return
        }
        resolvedPresentationID = presentationID
        let action = dismissAction
        invalidateCurrentPresentation()
        hidePanel()
        action?()
    }

    private func invalidateCurrentPresentation() {
        presentationID = nil
        startAction = nil
        dismissAction = nil
    }

    private func hidePanel() {
        panel?.orderOut(nil)
        panel?.alphaValue = 1
    }
}

@MainActor
private final class DetectedCallBannerPanel: NSPanel {
    var onCancel: (() -> Void)?

    override var canBecomeKey: Bool {
        true
    }

    override var canBecomeMain: Bool {
        false
    }

    override func cancelOperation(_ sender: Any?) {
        onCancel?()
    }
}

@MainActor
private struct DetectedCallPromptView: View {
    let applicationName: String
    let canStart: Bool
    let languageMode: CoreLanguageMode
    let onLanguageChange: (CoreLanguageMode) -> Void
    let onStart: () -> Void
    let onDismiss: () -> Void

    @Environment(\.accessibilityReduceTransparency)
    private var reduceTransparency

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "waveform")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 36, height: 36)
                .background(Color.accentColor, in: RoundedRectangle(
                    cornerRadius: 9,
                    style: .continuous
                ))
                .accessibilityHidden(true)

            VStack(alignment: .leading, spacing: 2) {
                Text("Meeting detected")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(.primary)
                    .lineLimit(1)
                Text(subtitle)
                    .font(.system(size: 13))
                    .foregroundStyle(
                        canStart ? Color.secondary : Color.orange
                    )
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isHeader)

            Picker(
                "Meeting language",
                selection: Binding(
                    get: { languageMode },
                    set: { mode in
                        onLanguageChange(mode)
                    }
                )
            ) {
                ForEach(
                    CoreLanguageMode.selectableCases,
                    id: \.rawValue
                ) { mode in
                    Text(mode.displayName).tag(mode)
                }
            }
            .labelsHidden()
            .pickerStyle(.menu)
            .frame(width: 108)
            .help("Language for this meeting")

            Button("Start recording", action: onStart)
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(!canStart)
                .help(
                    canStart
                        ? "Record microphone and system audio"
                        : "Finish setup in Settings first"
                )
                .accessibilityLabel("Start Recording")
                .accessibilityHint(
                    canStart
                        ? "Starts capture; a confirmed call end will show a warning before stopping"
                        : "Finish setup in Settings before recording"
                )

            Button(action: onDismiss) {
                Image(systemName: "xmark")
                    .font(.system(size: 11, weight: .semibold))
                    .frame(width: 24, height: 24)
                    .contentShape(Circle())
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
            .help("Not Now")
            .accessibilityLabel("Not Now")
            .accessibilityHint(
                "Dismisses this suggestion without recording"
            )
        }
        .padding(.horizontal, 14)
        .frame(width: 516, height: 72)
        .background(cardBackground, in: RoundedRectangle(
            cornerRadius: 18,
            style: .continuous
        ))
        .overlay {
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .stroke(Color(nsColor: .separatorColor).opacity(0.55))
        }
        .shadow(color: .black.opacity(0.20), radius: 10, y: 5)
        .padding(12)
        .frame(width: 540, height: 96)
    }

    private var subtitle: String {
        if canStart {
            return "\(applicationName) · Auto-stop after confirmed end"
        }
        return "\(applicationName) · Complete setup first"
    }

    private var cardBackground: AnyShapeStyle {
        if reduceTransparency {
            return AnyShapeStyle(Color(nsColor: .windowBackgroundColor))
        }
        return AnyShapeStyle(.regularMaterial)
    }
}

@MainActor
private struct DetectedCallAutoStopPromptView: View {
    let applicationName: String
    let deadline: ContinuousClock.Instant
    let onKeepRecording: () -> Void
    let onStopNow: () -> Void

    @Environment(\.accessibilityReduceTransparency)
    private var reduceTransparency

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "phone.down.fill")
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 36, height: 36)
                .background(Color.orange, in: RoundedRectangle(
                    cornerRadius: 9,
                    style: .continuous
                ))
                .accessibilityHidden(true)

            VStack(alignment: .leading, spacing: 2) {
                Text("Call appears to have ended")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(.primary)
                    .lineLimit(1)
                TimelineView(.periodic(from: .now, by: 1)) { _ in
                    Text(
                        "\(applicationName) · Stopping recording in "
                            + "\(remainingSeconds()) seconds"
                    )
                    .font(.system(size: 13))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isHeader)

            Button("Keep Recording", action: onKeepRecording)
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .help("Keep recording and check again later")
                .accessibilityHint(
                    "Cancels this countdown and keeps capture running"
                )

            Button("Stop Now", role: .destructive, action: onStopNow)
                .controlSize(.large)
                .help("Stop recording and finalize the transcript now")
                .accessibilityHint(
                    "Stops capture and finalizes the transcript now"
                )

            Button(action: onKeepRecording) {
                Image(systemName: "xmark")
                    .font(.system(size: 11, weight: .semibold))
                    .frame(width: 24, height: 24)
                    .contentShape(Circle())
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
            .help("Keep Recording")
            .accessibilityLabel("Close and Keep Recording")
            .accessibilityHint(
                "Closes this warning and keeps capture running"
            )
        }
        .padding(.horizontal, 14)
        .frame(width: 516, height: 72)
        .background(cardBackground, in: RoundedRectangle(
            cornerRadius: 18,
            style: .continuous
        ))
        .overlay {
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .stroke(Color(nsColor: .separatorColor).opacity(0.55))
        }
        .shadow(color: .black.opacity(0.20), radius: 10, y: 5)
        .padding(12)
        .frame(width: 540, height: 96)
    }

    private func remainingSeconds() -> Int {
        let now = ContinuousClock.now
        guard now < deadline else {
            return 0
        }
        let components = now.duration(to: deadline).components
        return Int(clamping: components.seconds)
            + (components.attoseconds > 0 ? 1 : 0)
    }

    private var cardBackground: AnyShapeStyle {
        if reduceTransparency {
            return AnyShapeStyle(Color(nsColor: .windowBackgroundColor))
        }
        return AnyShapeStyle(.regularMaterial)
    }
}
