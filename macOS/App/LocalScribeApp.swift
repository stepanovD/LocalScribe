import SwiftUI

@main
struct LocalScribeApp: App {
    @NSApplicationDelegateAdaptor(ApplicationDelegate.self)
    private var applicationDelegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        MenuBarExtra {
            MenuBarContentView(model: model)
        } label: {
            Label(model.menuBarTitle, systemImage: model.menuBarSymbol)
                .accessibilityLabel(model.menuBarTitle)
        }
        .menuBarExtraStyle(.window)

        Settings {
            SettingsView(model: model)
        }
    }

    init() {
        applicationDelegate.model = model
    }
}
