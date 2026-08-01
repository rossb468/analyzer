import SwiftUI
import AnalyzerFFI

/// The equaliser panel, below the plot.
///
/// Nothing here designs a filter or evaluates a response. Every edit goes
/// straight to the core and the list is re-read from it, so the faders and the
/// drawn curve cannot describe different filters.
struct EqualiserView: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        VStack(spacing: 0) {
            Divider()
            HStack(alignment: .top, spacing: 16) {
                if model.eqMode == AnalyzerEqMode_Graphic {
                    graphic
                } else {
                    parametric
                }
                Divider()
                controls
            }
            .padding(10)
        }
        .background(.quaternary.opacity(0.25))
    }

    // ------------------------------------------------------------- graphic --

    /// Ten faders on ISO octave centres.
    private var graphic: some View {
        HStack(spacing: 4) {
            ForEach(model.eqBands) { band in
                VStack(spacing: 2) {
                    Text(String(format: "%+.1f", band.gainDb))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(band.gainDb == 0 ? .secondary : .primary)

                    // SwiftUI has no vertical slider, and a rotated horizontal
                    // one is the standard answer on macOS.
                    Slider(
                        value: Binding(
                            get: { band.gainDb },
                            set: { model.setEqGain(band.id, $0) }
                        ),
                        in: -12...12
                    )
                    .rotationEffect(.degrees(-90))
                    .frame(width: 92)
                    .frame(width: 26, height: 92)

                    Text(formatFrequency(band.hz))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    // ---------------------------------------------------------- parametric --

    private var parametric: some View {
        VStack(alignment: .leading, spacing: 4) {
            if model.eqBands.isEmpty {
                Text("No filters. Add one to start.")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .frame(height: 92, alignment: .center)
            } else {
                ScrollView(.vertical) {
                    VStack(spacing: 2) {
                        ForEach(model.eqBands) { band in
                            row(band)
                        }
                    }
                }
                .frame(height: 116)
            }

            Button {
                model.addEqBand()
            } label: {
                Label("Add filter", systemImage: "plus")
            }
            .disabled(!model.isRunning)
        }
        .frame(minWidth: 460)
    }

    private func row(_ band: EqBand) -> some View {
        HStack(spacing: 6) {
            Toggle("", isOn: binding(band, \.enabled))
                .toggleStyle(.checkbox)
                .labelsHidden()

            Picker("", selection: binding(band, \.kind)) {
                Text("Peak").tag(AnalyzerFilterKind_Peaking)
                Text("Low shelf").tag(AnalyzerFilterKind_LowShelf)
                Text("High shelf").tag(AnalyzerFilterKind_HighShelf)
                Text("Low pass").tag(AnalyzerFilterKind_LowPass)
                Text("High pass").tag(AnalyzerFilterKind_HighPass)
                Text("Band pass").tag(AnalyzerFilterKind_BandPass)
                Text("Notch").tag(AnalyzerFilterKind_Notch)
                Text("All pass").tag(AnalyzerFilterKind_AllPass)
            }
            .labelsHidden()
            .frame(width: 108)

            field("Hz", value: binding(band, \.hz), format: "%.0f", width: 62)
            // Greyed rather than hidden, so the row does not reflow when the
            // shape changes - and so it is obvious that gain does nothing here.
            field("Gain", value: binding(band, \.gainDb), format: "%.1f", width: 56)
                .disabled(!band.usesGain)
                .opacity(band.usesGain ? 1 : 0.35)
            field("Q", value: binding(band, \.q), format: "%.2f", width: 52)

            Button {
                model.removeEqBand(band.id)
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.secondary)
        }
        .font(.system(size: 11, design: .monospaced))
    }

    private func field(
        _ label: String,
        value: Binding<Float>,
        format: String,
        width: CGFloat
    ) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            TextField(
                label,
                value: value,
                format: .number.precision(.fractionLength(format.contains(".0") ? 0 : 2))
            )
            .textFieldStyle(.roundedBorder)
            .frame(width: width)
        }
    }

    /// Bind one field of a band, writing the whole band back to the core.
    ///
    /// The core is the single owner; this view holds no filter state of its own.
    private func binding<T>(
        _ band: EqBand,
        _ path: WritableKeyPath<EqBand, T>
    ) -> Binding<T> {
        Binding(
            get: { band[keyPath: path] },
            set: { newValue in
                var updated = band
                updated[keyPath: path] = newValue
                model.setEqBand(band.id, updated)
            }
        )
    }

    // ------------------------------------------------------------ controls --

    private var controls: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let info = model.eqInfo, info.active {
                // Bands add, so this routinely exceeds any single setting. It
                // is shown rather than corrected: quietly moving a level the
                // user set is worse than telling them about it.
                HStack(spacing: 6) {
                    Text("peak")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%+.1f dB", info.peakGainDb))
                        .foregroundStyle(info.clips ? .orange : .primary)
                    if info.clips {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                            .help("The equaliser asks for more than full scale and will clip. "
                                  + "Trim removes exactly this much.")
                    }
                }
                HStack(spacing: 6) {
                    Text("trim")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%+.1f dB", info.preampDb))
                }
            }

            HStack(spacing: 6) {
                Button("Trim") { model.trimEq() }
                    .disabled(!model.isRunning)
                    .help("Set the output trim so the equaliser's loudest point sits at unity.")
                Button("Flatten") { model.flattenEq() }
                    .disabled(!model.isRunning)
            }

            Toggle("Corrected", isOn: $model.showCorrected)
                .toggleStyle(.checkbox)
                .help("Draw the measurement with the equaliser applied, beside the raw one.")
        }
        .font(.system(size: 11, design: .monospaced))
        .frame(width: 190, alignment: .leading)
    }
}
