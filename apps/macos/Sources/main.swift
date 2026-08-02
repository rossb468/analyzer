import SwiftUI

/// Entry point.
///
/// `-parse-as-library` is passed at build time so `@main` works from a plain
/// swiftc invocation without an Xcode project.
@main
struct AnalyzerApp: App {
    /// Owned here rather than by the window, so the Settings scene and the
    /// analyser are editing the same object.
    @StateObject private var settings = SettingsStore()

    var body: some Scene {
        WindowGroup("Analyzer") {
            ContentView(settings: settings)
        }
        .defaultSize(width: 1180, height: 640)

        // A `Settings` scene, which is what puts Settings… in the app menu on
        // Command-comma and gives the window its standard chrome.
        Settings {
            SettingsView(store: settings)
        }
    }
}
