import SwiftUI

/// Entry point.
///
/// `-parse-as-library` is passed at build time so `@main` works from a plain
/// swiftc invocation without an Xcode project.
@main
struct AnalyzerApp: App {
    var body: some Scene {
        WindowGroup("Analyzer") {
            ContentView()
        }
        .defaultSize(width: 1000, height: 560)
    }
}
