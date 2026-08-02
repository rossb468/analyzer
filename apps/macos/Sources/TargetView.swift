import SwiftUI
import AnalyzerFFI

/// Target curve controls, shown in the equaliser inspector.
///
/// A target belongs with the equaliser because it is what the equaliser is
/// aiming at: the measurement says what the room does, and this says what it
/// should do. Nothing here evaluates the curve — the shape and its parameters
/// go to the core and the drawn layer is read back from it.
struct TargetSettings: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        Section("Target") {
            Picker("Shape", selection: shapeBinding) {
                Text("Flat").tag(AnalyzerTargetShape_Flat)
                Text("Tilt").tag(AnalyzerTargetShape_Tilt)
                Text("Room").tag(AnalyzerTargetShape_Room)
                Text("From file").tag(AnalyzerTargetShape_Custom)
            }
            .help("""
                Flat is the wrong answer for a room: a loudspeaker measured flat \
                anechoically sounds thin in one, because the ear expects the bass \
                lift a real space produces.
                """)

            if model.target.shape == AnalyzerTargetShape_Room {
                slider("Bass lift", value: shelfBinding, range: 0...12, unit: "dB", fraction: 1)
                slider(
                    "Transition",
                    value: transitionBinding,
                    range: 40...500,
                    unit: "Hz",
                    fraction: 0
                )
            }

            if model.target.shape == AnalyzerTargetShape_Room
                || model.target.shape == AnalyzerTargetShape_Tilt {
                slider("Tilt", value: tiltBinding, range: -3...1, unit: "dB/oct", fraction: 2)
            }

            if model.target.shape == AnalyzerTargetShape_Custom {
                LabeledContent("Curve") {
                    Text(model.target.has_custom ? "Loaded" : "None loaded")
                        .foregroundStyle(model.target.has_custom ? .primary : .secondary)
                        .font(.system(size: 11, design: .monospaced))
                }
            }

            LabeledContent("Offset") {
                Text(String(format: "%+.1f dB", model.target.offset_db))
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.secondary)
            }

            HStack {
                Button("Align") { model.alignTarget() }
                    .disabled(!model.isRunning)
                    .help("""
                        Move the target onto the measurement by matching their \
                        average level from 200 Hz to 2 kHz. A target is relative, \
                        so until this runs it sits wherever the offset happens \
                        to be.
                        """)
                Button("Load…") { model.loadTarget() }
                    .disabled(!model.isRunning)
                    .help("A frequency and level text file, as .frd, .cal or plain text.")
            }

            Toggle("Draw target", isOn: $model.showTarget)
        }
    }

    // Every parameter writes the whole target back and re-reads it, so the
    // controls cannot describe a curve different from the one being drawn.

    private var shapeBinding: Binding<AnalyzerTargetShape> {
        binding(\.shape)
    }

    private var shelfBinding: Binding<Float> { binding(\.shelf_db) }
    private var transitionBinding: Binding<Float> { binding(\.transition_hz) }
    private var tiltBinding: Binding<Float> { binding(\.db_per_octave) }

    private func binding<T>(_ path: WritableKeyPath<AnalyzerTarget, T>) -> Binding<T> {
        Binding(
            get: { model.target[keyPath: path] },
            set: { newValue in
                model.target[keyPath: path] = newValue
                model.applyTarget()
            }
        )
    }

    private func slider(
        _ label: String,
        value: Binding<Float>,
        range: ClosedRange<Float>,
        unit: String,
        fraction: Int
    ) -> some View {
        LabeledContent(label) {
            HStack(spacing: 6) {
                Slider(value: value, in: range)
                Text(String(format: "%.\(fraction)f %@", value.wrappedValue, unit))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .frame(width: 66, alignment: .trailing)
            }
        }
    }
}
