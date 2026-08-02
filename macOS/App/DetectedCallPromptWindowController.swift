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

    private var panel: DetectedCallBannerPanel?
    private var hostingController:
        NSHostingController<DetectedCallPromptView>?
    private var proposalID: UUID?
    private var resolvedProposalID: UUID?
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
        guard resolvedProposalID != proposal.id else {
            return
        }

        let isNewPresentation = proposalID != proposal.id
        if isNewPresentation {
            invalidateCurrentPresentation()
            proposalID = proposal.id
        }
        startAction = onStart
        dismissAction = onDismiss

        let rootView = DetectedCallPromptView(
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
        )

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

    func close() {
        if let proposalID {
            resolvedProposalID = proposalID
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

    private func resolveWithStart() {
        guard let proposalID, resolvedProposalID != proposalID else {
            return
        }
        resolvedProposalID = proposalID
        let action = startAction
        invalidateCurrentPresentation()
        hidePanel()
        action?()
    }

    private func resolveWithDismiss() {
        guard let proposalID, resolvedProposalID != proposalID else {
            return
        }
        resolvedProposalID = proposalID
        let action = dismissAction
        invalidateCurrentPresentation()
        hidePanel()
        action?()
    }

    private func invalidateCurrentPresentation() {
        proposalID = nil
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
                        ? "Starts microphone and system audio capture"
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
            return applicationName
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
