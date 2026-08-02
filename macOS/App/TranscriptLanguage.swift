import Foundation

extension CoreLanguageMode {
    static var selectableCases: [CoreLanguageMode] {
        [.russianEnglish, .russian, .english]
    }

    var displayName: String {
        switch self {
        case .russian:
            "Russian"
        case .english:
            "English"
        case .russianEnglish:
            "Auto-detect"
        case .unknown:
            "Unknown"
        }
    }
}

struct TranscriptLanguagePreferences {
    static let defaultLanguageModeKey =
        "transcription.defaultLanguageMode"

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    var defaultLanguageMode: CoreLanguageMode {
        get {
            guard let stored = UInt32(
                exactly: defaults.integer(
                    forKey: Self.defaultLanguageModeKey
                )
            ),
                let mode = CoreLanguageMode(rawValue: stored),
                mode != .unknown
            else {
                return .russianEnglish
            }
            return mode
        }
        nonmutating set {
            let mode = newValue == .unknown ? .russianEnglish : newValue
            defaults.set(
                Int(mode.rawValue),
                forKey: Self.defaultLanguageModeKey
            )
        }
    }
}
