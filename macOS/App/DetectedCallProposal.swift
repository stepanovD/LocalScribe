import Foundation

struct DetectedCallProposal: Identifiable, Sendable, Equatable {
    let id: UUID
    let platform: CallPlatform

    var applicationName: String {
        platform.displayName
    }
}
