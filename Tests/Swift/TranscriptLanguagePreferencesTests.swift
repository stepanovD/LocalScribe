import Foundation
import XCTest

@testable import LocalScribeApp

final class TranscriptLanguagePreferencesTests: XCTestCase {
    func testDefaultsToAutomaticLanguageDetection() throws {
        let defaults = try makeDefaults()
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let preferences = TranscriptLanguagePreferences(defaults: defaults)

        XCTAssertEqual(preferences.defaultLanguageMode, .russianEnglish)
    }

    func testPersistsSelectedDefaultLanguage() throws {
        let defaults = try makeDefaults()
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let preferences = TranscriptLanguagePreferences(defaults: defaults)

        preferences.defaultLanguageMode = .russian

        let restored = TranscriptLanguagePreferences(defaults: defaults)
        XCTAssertEqual(restored.defaultLanguageMode, .russian)
    }

    func testInvalidStoredLanguageFallsBackToAutomaticDetection() throws {
        let defaults = try makeDefaults()
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        defaults.set(
            999,
            forKey: TranscriptLanguagePreferences.defaultLanguageModeKey
        )

        let preferences = TranscriptLanguagePreferences(defaults: defaults)

        XCTAssertEqual(preferences.defaultLanguageMode, .russianEnglish)
    }

    private var defaultsName: String {
        "TranscriptLanguagePreferencesTests"
    }

    private func makeDefaults() throws -> UserDefaults {
        let defaults = try XCTUnwrap(UserDefaults(suiteName: defaultsName))
        defaults.removePersistentDomain(forName: defaultsName)
        return defaults
    }
}
