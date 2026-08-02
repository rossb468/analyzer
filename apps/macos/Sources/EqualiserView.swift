import SwiftUI
import AnalyzerFFI

/// The equaliser, split between two places.
///
/// `EqualiserInspector` holds the settings — which equaliser, what it costs in
/// headroom, what gets drawn. `EqualiserEditor` holds the faders and the filter
/// list, which need real width and so live under the plot rather than in a
/// 280-point column.
///
/// Nothing here designs a filter or evaluates a response. Every edit goes
/// straight to the core and the list is re-read from it, so the faders and the
/// drawn curve cannot describe different filters.

// MARK: - Inspector

struct EqualiserInspector: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        Section("Equaliser") {
            Picker("Type", selection: $model.eqMode) {
                Text("Off").tag(AnalyzerEqMode_Off)
                Text("Graphic").tag(AnalyzerEqMode_Graphic)
                Text("Parametric").tag(AnalyzerEqMode_Parametric)
            }
            .help("""
                The equaliser is drawn against the measurement and applied to \
                the generator, so the corrected curve is a prediction you can \
                also hear.
                """)
        }

        if let info = model.eqInfo, info.active {
            Section("Headroom") {
                // Bands add, so this routinely exceeds any single setting. It is
                // shown rather than corrected: quietly moving a level the user
                // set is worse than telling them about it.
                LabeledContent("Peak") {
                    HStack(spacing: 4) {
                        Text(String(format: "%+.1f dB", info.peakGainDb))
                            .foregroundStyle(info.clips ? .orange : .primary)
                        if info.clips {
                            Image(systemName: "exclamationmark.triangle.fill")
                                .foregroundStyle(.orange)
                                .help("""
                                    The equaliser asks for more than full scale \
                                    and will clip. Trim removes exactly this much.
                                    """)
                        }
                    }
                    .font(.system(size: 11, design: .monospaced))
                }

                LabeledContent("Trim") {
                    Text(String(format: "%+.1f dB", info.preampDb))
                        .font(.system(size: 11, design: .monospaced))
                }

                HStack {
                    Button("Trim") { model.trimEq() }
                        .disabled(!model.isRunning)
                        .help("Set the output trim so the equaliser's loudest point sits at unity.")
                    Button("Flatten") { model.flattenEq() }
                        .disabled(!model.isRunning)
                }
            }
        }

        TargetSettings(model: model)

        Section("Display") {
            Toggle("Corrected curve", isOn: $model.showCorrected)
                .help("Draw the measurement with the equaliser applied, beside the raw one.")
                .disabled(model.eqMode == AnalyzerEqMode_Off)
        }

        Section("Export") {
            // The point of designing a correction against a measurement is to
            // load it into whatever will actually apply it.
            Menu {
                Button("REW filter settings…") { model.exportFilters(AnalyzerFilterFormat_Rew) }
                Button("Equalizer APO…") {
                    model.exportFilters(AnalyzerFilterFormat_EqualizerApo)
                }
                Button("miniDSP biquads…") { model.exportFilters(AnalyzerFilterFormat_MiniDsp) }
            } label: {
                Label("Export filters", systemImage: "square.and.arrow.up")
            }
            .disabled(model.eqMode == AnalyzerEqMode_Off || !model.isRunning)
        }
    }
}

// MARK: - Editor

/// The faders and filter list, shown under the plot while the equaliser section
/// is selected.
struct EqualiserEditor: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        Group {
            switch model.eqMode {
            case AnalyzerEqMode_Graphic: graphic
            case AnalyzerEqMode_Parametric: parametric
            default: off
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(12)
        .background(.quaternary.opacity(0.25))
    }

    private var off: some View {
        VStack(spacing: 6) {
            Text("The equaliser is off.")
                .foregroundStyle(.secondary)
            Text("Choose Graphic or Parametric in the inspector to start.")
                .font(.caption)
                .foregroundStyle(.tertiary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // ------------------------------------------------------------- graphic --

    /// Ten faders on ISO octave centres.
    private var graphic: some View {
        HStack(alignment: .top, spacing: 4) {
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
            Spacer()
        }
    }

    // ---------------------------------------------------------- parametric --

    private var parametric: some View {
        VStack(alignment: .leading, spacing: 6) {
            if model.eqBands.isEmpty {
                Text("No filters. Add one to start.")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(.vertical, 20)
            } else {
                ScrollView(.vertical) {
                    VStack(spacing: 2) {
                        ForEach(model.eqBands) { band in
                            row(band)
                        }
                    }
                }
            }

            Button {
                model.addEqBand()
            } label: {
                Label("Add filter", systemImage: "plus")
            }
            .disabled(!model.isRunning)
        }
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

            field(value: binding(band, \.hz), fraction: 0, width: 62)
            // Greyed rather than hidden, so the row does not reflow when the
            // shape changes - and so it is obvious that gain does nothing here.
            field(value: binding(band, \.gainDb), fraction: 1, width: 56)
                .disabled(!band.usesGain)
                .opacity(band.usesGain ? 1 : 0.35)
            field(value: binding(band, \.q), fraction: 2, width: 52)

            Button {
                model.removeEqBand(band.id)
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.secondary)

            Spacer()
        }
        .font(.system(size: 11, design: .monospaced))
    }

    private func field(value: Binding<Float>, fraction: Int, width: CGFloat) -> some View {
        TextField("", value: value, format: .number.precision(.fractionLength(fraction)))
            .textFieldStyle(.roundedBorder)
            .frame(width: width)
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
}
