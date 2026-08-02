import SwiftUI
import AnalyzerFFI

/// The trace list, in the inspector.
///
/// A captured trace is a measurement held at analysis resolution, not the pixel
/// columns it happened to be drawn as, so resizing the window or changing the
/// axis re-reduces it rather than stretching a picture. Nothing here does that
/// reduction: it asks the core for the curve at the current geometry.
struct TracesInspector: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        Section("Capture") {
            Button {
                model.captureTrace()
            } label: {
                Label("Capture trace", systemImage: "camera")
            }
            .disabled(!model.isRunning)
            .help("Store the live curve for comparison. It keeps its own resolution.")

            if model.traces.count >= AnalyzerModel.maxTraces {
                Text("\(AnalyzerModel.maxTraces) traces are drawn at once. "
                     + "Capturing more will store them, but only the first "
                     + "\(AnalyzerModel.maxTraces) appear on the plot.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }

        Section("Traces") {
            if model.traces.isEmpty {
                Text("None captured.")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(model.traces) { trace in
                    row(trace)
                }

                Button("Remove all", role: .destructive) { model.clearTraces() }
                    .disabled(model.traces.isEmpty)
            }
        }
    }

    private func row(_ trace: CapturedTrace) -> some View {
        HStack(spacing: 8) {
            Toggle(
                "",
                isOn: Binding(
                    get: { trace.visible },
                    set: { model.setTraceVisible(trace.index, $0) }
                )
            )
            .toggleStyle(.checkbox)
            .labelsHidden()

            // The swatch is the only place the palette index becomes a colour,
            // and it is the same lookup the plot layer uses.
            RoundedRectangle(cornerRadius: 2)
                .fill(swatch(trace.colour))
                .frame(width: 10, height: 10)
                .opacity(trace.visible ? 1 : 0.3)

            VStack(alignment: .leading, spacing: 0) {
                Text(trace.name)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(trace.detail)
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Button {
                model.removeTrace(trace.index)
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.secondary)
        }
    }

    private func swatch(_ colour: UInt32) -> Color {
        let rgba = AnalyzerModel.traceColour(colour)
        return Color(
            .sRGB,
            red: Double(rgba.x),
            green: Double(rgba.y),
            blue: Double(rgba.z),
            opacity: 1
        )
    }
}
