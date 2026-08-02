import SwiftUI
import AnalyzerFFI

/// The per-section inspector.
///
/// Every control that only makes sense for one section lives here rather than in
/// the toolbar, which is why the toolbar is now short enough to read. Nothing in
/// this file computes anything: each control writes through the model to the
/// core and reads back whatever the core then says.
struct InspectorView: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        Form {
            switch model.section {
            case .rta: rta
            case .transfer: transfer
            case .equaliser: EqualiserInspector(model: model)
            case .measure: MeasureInspector(model: model)
            case .spectrogram: spectrogram
            case .traces: TracesInspector(model: model)
            }
        }
        .formStyle(.grouped)
        .inspectorColumnWidth(min: 240, ideal: 280, max: 360)
    }

    // ------------------------------------------------------------------ rta -

    @ViewBuilder
    private var rta: some View {
        AnalysisSettings(model: model)

        Section("Display") {
            Toggle("Long-term average", isOn: $model.showAverage)
            Button("Reset average") { model.session?.resetAverage() }
                .disabled(!model.isRunning)
        }
    }

    // ---------------------------------------------------------- spectrogram -

    @ViewBuilder
    private var spectrogram: some View {
        Section("Colour") {
            LabeledContent("Floor") {
                HStack(spacing: 6) {
                    Slider(value: $model.spectrogramFloorDb, in: -120...(-20))
                    Text(String(format: "%.0f dB", model.spectrogramFloorDb))
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 52, alignment: .trailing)
                }
            }
            .help("""
                Bottom of the colour ramp. Spreading it over the whole 120 dB \
                axis leaves everything but the loudest peaks in one colour.
                """)
        }

        AnalysisSettings(model: model)
    }

    // ------------------------------------------------------------- transfer -

    @ViewBuilder
    private var transfer: some View {
        if !model.canPlay && model.reference == AnalyzerReference_Internal {
            Section {
                OutputHint(model: model, needs: "An internal reference")
            }
        }

        Section("Reference") {
            Picker("Source", selection: $model.reference) {
                Text("Internal").tag(AnalyzerReference_Internal)
                Text("Loopback").tag(AnalyzerReference_Input)
            }
            .pickerStyle(.segmented)

            if model.reference == AnalyzerReference_Input {
                Picker("Channel", selection: $model.referenceChannel) {
                    ForEach(0..<Int(model.inputChannels), id: \.self) { channel in
                        Text("\(channel + 1)").tag(UInt32(channel))
                    }
                }
            }
        }

        Section("Delay") {
            Button("Find delay") { model.findDelay() }
                .disabled(!model.isRunning)
                .help("""
                    Cross-correlate the two channels and remove the propagation \
                    delay. Without this the phase curve winds through hundreds \
                    of turns and coherence collapses well before 1 kHz.
                    """)

            if let transfer = model.transfer {
                LabeledContent("Offset") {
                    Text(transfer.delaySummary)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(transfer.estimating ? .orange : .secondary)
                }
            }
        }

        Section("Curves") {
            Toggle("Phase", isOn: $model.showPhase)
            Toggle("Coherence", isOn: $model.showCoherence)
            Toggle("Long-term average", isOn: $model.showAverage)
        }

        AnalysisSettings(model: model)
    }
}

/// Transform settings, shared by every section that runs an analysis.
struct AnalysisSettings: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        Section("Analysis") {
            // Labelled with the time each frame spans, because that is the cost
            // being paid: 131072 points buys 0.37 Hz resolution and 2.7 seconds
            // of latency, and a picker showing only the point count hides half
            // the trade.
            Picker("Size", selection: $model.fftSize) {
                ForEach(AnalyzerModel.fftSizes, id: \.self) { size in
                    Text(model.fftLabel(size)).tag(size)
                }
            }

            Picker("Window", selection: $model.window) {
                Text("Hann").tag(AnalyzerWindow_Hann)
                Text("Blackman-Harris").tag(AnalyzerWindow_BlackmanHarris)
                Text("Flat-top").tag(AnalyzerWindow_FlatTop)
                Text("Rectangular").tag(AnalyzerWindow_Rectangular)
            }

            Picker("Averaging", selection: $model.averaging) {
                Text("Fast").tag(AnalyzerAveraging_Fast)
                Text("None").tag(AnalyzerAveraging_None)
                Text("Infinite").tag(AnalyzerAveraging_Infinite)
                Text("Peak hold").tag(AnalyzerAveraging_PeakHold)
            }
        }
    }
}
