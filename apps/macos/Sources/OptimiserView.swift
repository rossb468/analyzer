import SwiftUI
import AnalyzerFFI

/// Automatic equalisation controls.
///
/// The fit itself is entirely in the core; this sets its constraints and shows
/// what it achieved. The constraints are exposed rather than hidden because the
/// interesting ones are judgement calls a user may reasonably disagree with —
/// how much boost to allow, and how far up the band to correct.
struct OptimiserSettings: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        Section("Automatic EQ") {
            Stepper(value: filterCount, in: 1...16) {
                LabeledContent("Filters", value: "\(model.optimiser.max_filters)")
            }

            LabeledContent("Correct to") {
                HStack(spacing: 6) {
                    Slider(value: binding(\.to_hz), in: 100...20_000)
                    Text(formatFrequency(model.optimiser.to_hz))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 46, alignment: .trailing)
                }
            }
            .help("""
                Above the room's transition frequency the response changes \
                enormously with microphone position, so a filter fitted at one \
                position is fitted to noise everywhere else.
                """)

            LabeledContent("Max boost") {
                HStack(spacing: 6) {
                    Slider(value: binding(\.max_boost_db), in: 0...12)
                    Text(String(format: "%.0f dB", model.optimiser.max_boost_db))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 46, alignment: .trailing)
                }
            }
            .help("""
                A dip in a room measurement is usually a cancellation. Boosting \
                one consumes headroom and drives the woofer harder without \
                filling it in, which is why this defaults far below the cut limit.
                """)

            LabeledContent("Max cut") {
                HStack(spacing: 6) {
                    Slider(value: binding(\.max_cut_db), in: 0...24)
                    Text(String(format: "%.0f dB", model.optimiser.max_cut_db))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 46, alignment: .trailing)
                }
            }

            Button {
                model.runOptimiser()
            } label: {
                Label("Fit filters", systemImage: "wand.and.stars")
            }
            .disabled(!model.isRunning)
            .help("Replaces the parametric filters with a fit against the target.")

            if let result = model.optimisation {
                // Reported, not asserted. How much improvement is achievable
                // depends entirely on the measurement, and a fit that could not
                // do much should say so rather than look like a failure.
                LabeledContent("Result") {
                    Text(summary(result))
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(result.band_count == 0 ? .secondary : .primary)
                }
            }
        }
    }

    private func summary(_ result: AnalyzerOptimisation) -> String {
        guard result.band_count > 0 else { return "nothing to correct" }
        let improvement = result.initial_error_db - result.final_error_db
        return String(
            format: "%d filters · %.1f → %.1f dB (%+.1f)",
            result.band_count,
            result.initial_error_db,
            result.final_error_db,
            -improvement
        )
    }

    private var filterCount: Binding<Double> {
        Binding(
            get: { Double(model.optimiser.max_filters) },
            set: { newValue in
                model.optimiser.max_filters = UInt32(newValue)
            }
        )
    }

    private func binding(_ path: WritableKeyPath<AnalyzerOptimiserConfig, Float>) -> Binding<Float> {
        Binding(
            get: { model.optimiser[keyPath: path] },
            set: { model.optimiser[keyPath: path] = $0 }
        )
    }
}
