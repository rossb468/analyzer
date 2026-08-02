import SwiftUI
import AnalyzerFFI

/// Swept measurement controls.
///
/// A sweep is played, the response recorded, and the two deconvolved into an
/// impulse response. Everything reported here — the gated response drawn on the
/// plot, the decay times, the arrival — falls out of that one impulse response
/// rather than being measured separately. None of that happens in this file.
struct MeasureInspector: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        Section("Sweep") {
            LabeledContent("Length") {
                HStack(spacing: 6) {
                    Slider(value: binding(\.seconds), in: 0.5...10)
                    Text(String(format: "%.1f s", model.measureConfig.seconds))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 46, alignment: .trailing)
                }
            }
            .help("""
                A longer sweep puts more energy into the room and lifts the \
                measurement further above the noise floor.
                """)

            LabeledContent("Level") {
                HStack(spacing: 6) {
                    Slider(value: binding(\.level_db), in: -40...0)
                    Text(String(format: "%.0f dBFS", model.measureConfig.level_db))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 56, alignment: .trailing)
                }
            }

            LabeledContent("Tail") {
                HStack(spacing: 6) {
                    Slider(value: binding(\.tail_seconds), in: 0...5)
                    Text(String(format: "%.1f s", model.measureConfig.tail_seconds))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 46, alignment: .trailing)
                }
            }
            .help("How long to keep recording after the sweep, to catch the decay.")
        }

        Section("Gate") {
            LabeledContent("Length") {
                HStack(spacing: 6) {
                    Slider(value: binding(\.gate_ms), in: 1...100)
                    Text(String(format: "%.0f ms", model.measureConfig.gate_ms))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 52, alignment: .trailing)
                }
            }
            .help("""
                The window taken from the impulse response before the first \
                reflection. Shorter excludes more of the room and resolves less \
                far down.
                """)

            if let result = model.measurement {
                LabeledContent("Resolves to") {
                    Text(formatFrequency(result.resolution_hz))
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
                .help("""
                    Below this the curve is an artefact of the gate rather than \
                    a property of the room.
                    """)
            }
        }

        if !model.canPlay {
            Section {
                OutputHint(model: model, needs: "Measuring")
            }
        }

        Section {
            if model.isMeasuring {
                VStack(alignment: .leading, spacing: 6) {
                    ProgressView(value: model.measureProgress)
                    Button("Cancel", role: .destructive) { model.cancelMeasurement() }
                }
            } else {
                Button {
                    model.startMeasurement()
                } label: {
                    Label("Measure", systemImage: "dot.radiowaves.left.and.right")
                }
                .disabled(!model.isRunning || !model.canPlay)
                .help("Play a sweep and measure the response.")
            }
        }

        if let result = model.measurement {
            Section("Result") {
                reading("Arrival", String(format: "%.2f ms · %.2f m",
                                          result.arrival_ms, result.arrival_metres))
                    .help("""
                        Includes the converter round trip, not just the flight \
                        time through the air. Nothing synchronises the sweep and \
                        the recording to a sample, and correcting for that needs \
                        a loopback reference, which is a measurement in itself.
                        """)

                if result.has_edt { reading("EDT", seconds(result.edt)) }
                if result.has_t20 { reading("T20", seconds(result.t20)) }
                if result.has_t30 { reading("T30", seconds(result.t30)) }

                if result.has_decay_spread {
                    // Above roughly 0.1 the decay is not a straight line and no
                    // single number describes it, so this is shown rather than
                    // a single confident figure.
                    reading("Spread", String(format: "%.0f%%", result.decay_spread * 100))
                        .foregroundStyle(result.decay_spread > 0.1 ? .orange : .primary)
                        .help("""
                            How far the decay estimates disagree. Above about \
                            10% the decay is not a straight line and no single \
                            reverberation time describes it.
                            """)
                }

                if !result.has_edt && !result.has_t20 && !result.has_t30 {
                    Text("No decay could be measured — the tail did not stand clear "
                         + "of the noise floor.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Section("Display") {
                Toggle("Draw measured response", isOn: $model.showMeasured)
                Toggle("Impulse response", isOn: $model.showImpulse)
            }
        }
    }

    private func reading(_ label: String, _ value: String) -> some View {
        LabeledContent(label) {
            Text(value)
                .font(.system(size: 11, design: .monospaced))
        }
    }

    private func seconds(_ value: Float) -> String {
        String(format: "%.3f s", value)
    }

    private func binding(
        _ path: WritableKeyPath<AnalyzerMeasureConfig, Float>
    ) -> Binding<Float> {
        Binding(
            get: { model.measureConfig[keyPath: path] },
            set: { model.measureConfig[keyPath: path] = $0 }
        )
    }
}
