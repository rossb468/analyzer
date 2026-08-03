import SwiftUI
import UniformTypeIdentifiers
import AnalyzerFFI

/// Program settings, held by the core and mirrored here for binding.
///
/// The values, their validation and the file format all live in Rust. This class
/// supplies the one genuinely platform-specific part — where a preferences file
/// belongs on this operating system — and otherwise moves fields across the
/// boundary. Anything clamped or defaulted here instead would be a rule the
/// Windows and Linux clients had to rediscover.
@MainActor
final class SettingsStore: ObservableObject {

    /// Startup defaults.
    @Published var fftSize: UInt32 { didSet { save() } }
    @Published var window: AnalyzerWindow { didSet { save() } }
    @Published var averaging: AnalyzerAveraging { didSet { save() } }
    @Published var startOnLaunch: Bool { didSet { save() } }

    /// Plot axis.
    @Published var minHz: Float { didSet { axisChanged() } }
    @Published var maxHz: Float { didSet { axisChanged() } }
    @Published var minDb: Float { didSet { axisChanged() } }
    @Published var maxDb: Float { didSet { axisChanged() } }
    @Published var levelGridStep: Float { didSet { axisChanged() } }

    /// Calibration. `splOffsetDb` is meaningless while `hasSplOffset` is false —
    /// "never calibrated" and "calibrated to no correction" are different facts
    /// and the core keeps them apart, so this does too.
    @Published var hasSplOffset: Bool { didSet { save() } }
    @Published var splOffsetDb: Float { didSet { save() } }
    @Published var micCalPath: String { didSet { save() } }

    /// Bumped whenever an axis value changes, so a renderer holding cached plot
    /// geometry knows to re-declare it.
    @Published private(set) var axisGeneration = 0

    /// Last error from reading or writing the file, if any.
    @Published var errorMessage: String?

    /// Suppresses writes while the initial load is populating the properties.
    private var loading = true

    /// `~/Library/Application Support/dev.rossbower.analyzer/settings.cfg`.
    ///
    /// Rust creates the directory; the platform only decides where it goes.
    static var fileURL: URL {
        let base = FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask)
            .first ?? FileManager.default.homeDirectoryForCurrentUser
        return base
            .appendingPathComponent("dev.rossbower.analyzer", isDirectory: true)
            .appendingPathComponent("settings.cfg")
    }

    init() {
        var raw = analyzer_settings_default()
        var status = AnalyzerStatus()
        var failure: String?

        let ok = Self.fileURL.path.withCString { path in
            analyzer_settings_load(path, &raw, &status)
        }
        if !ok {
            failure = Self.statusMessage(status)
        }

        fftSize = raw.fft_size
        window = raw.window
        averaging = raw.averaging
        startOnLaunch = raw.start_on_launch
        minHz = raw.min_hz
        maxHz = raw.max_hz
        minDb = raw.min_db
        maxDb = raw.max_db
        levelGridStep = raw.level_grid_step
        hasSplOffset = raw.has_spl_offset
        splOffsetDb = raw.spl_offset_db
        micCalPath = Self.path(from: raw)
        errorMessage = failure
        loading = false
    }

    /// Restore everything to the core's defaults.
    func resetToDefaults() {
        let raw = analyzer_settings_default()
        loading = true
        fftSize = raw.fft_size
        window = raw.window
        averaging = raw.averaging
        startOnLaunch = raw.start_on_launch
        minHz = raw.min_hz
        maxHz = raw.max_hz
        minDb = raw.min_db
        maxDb = raw.max_db
        levelGridStep = raw.level_grid_step
        hasSplOffset = raw.has_spl_offset
        splOffsetDb = raw.spl_offset_db
        micCalPath = Self.path(from: raw)
        loading = false
        axisGeneration += 1
        save()
    }

    private func axisChanged() {
        guard !loading else { return }
        axisGeneration += 1
        save()
    }

    /// Write through to the core, which validates and then writes the file.
    ///
    /// The validated values are read straight back, so a range the core refuses
    /// snaps visibly in the UI rather than being silently disagreed with.
    private func save() {
        guard !loading else { return }

        var raw = analyzer_settings_default()
        raw.fft_size = fftSize
        raw.window = window
        raw.averaging = averaging
        raw.start_on_launch = startOnLaunch
        raw.min_hz = minHz
        raw.max_hz = maxHz
        raw.min_db = minDb
        raw.max_db = maxDb
        raw.level_grid_step = levelGridStep
        raw.has_spl_offset = hasSplOffset
        raw.spl_offset_db = splOffsetDb
        Self.setPath(micCalPath, on: &raw)

        var status = AnalyzerStatus()
        let ok = Self.fileURL.path.withCString { path in
            analyzer_settings_save(path, &raw, &status)
        }
        errorMessage = ok ? nil : Self.statusMessage(status)
    }

    // The path is an inline fixed buffer in the C struct, so it is read and
    // written as raw bytes rather than as a pointer with a lifetime.

    private static func path(from settings: AnalyzerSettings) -> String {
        withUnsafeBytes(of: settings.mic_cal_path) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }

    private static func setPath(_ path: String, on settings: inout AnalyzerSettings) {
        withUnsafeMutableBytes(of: &settings.mic_cal_path) { raw in
            raw.copyBytes(from: [UInt8](repeating: 0, count: raw.count))
            // One byte is reserved for the terminator. The core truncates on a
            // character boundary when it writes the file; this only has to avoid
            // running off the end of the buffer.
            let bytes = Array(path.utf8.prefix(raw.count - 1))
            raw.copyBytes(from: bytes)
        }
    }

    private static func statusMessage(_ status: AnalyzerStatus) -> String {
        withUnsafeBytes(of: status.message) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }
}

/// The Settings window.
///
/// A `Settings` scene, so the standard Command-comma menu item, the window
/// chrome and the tab styling all come from the system rather than being
/// rebuilt here.
struct SettingsView: View {
    @ObservedObject var store: SettingsStore

    var body: some View {
        TabView {
            general.tabItem { Label("General", systemImage: "gearshape") }
            axis.tabItem { Label("Axis", systemImage: "chart.xyaxis.line") }
            calibration.tabItem { Label("Calibration", systemImage: "ruler") }
        }
        .frame(width: 440)
        .padding(.top, 8)
    }

    // -------------------------------------------------------------- general -

    private var general: some View {
        Form {
            Section {
                Picker("Transform size", selection: $store.fftSize) {
                    ForEach(AnalyzerModel.fftSizes, id: \.self) { size in
                        Text(String(size)).tag(size)
                    }
                }
                Picker("Window", selection: $store.window) {
                    Text("Hann").tag(AnalyzerWindow_Hann)
                    Text("Blackman-Harris").tag(AnalyzerWindow_BlackmanHarris)
                    Text("Flat-top").tag(AnalyzerWindow_FlatTop)
                    Text("Rectangular").tag(AnalyzerWindow_Rectangular)
                }
                Picker("Averaging", selection: $store.averaging) {
                    Text("Fast").tag(AnalyzerAveraging_Fast)
                    Text("None").tag(AnalyzerAveraging_None)
                    Text("Infinite").tag(AnalyzerAveraging_Infinite)
                    Text("Peak hold").tag(AnalyzerAveraging_PeakHold)
                }
            } header: {
                Text("New sessions start with")
            }

            Section {
                Toggle("Start capturing when the window opens", isOn: $store.startOnLaunch)
            }

            footer
        }
        .formStyle(.grouped)
    }

    // ----------------------------------------------------------------- axis -

    private var axis: some View {
        Form {
            Section("Frequency") {
                LabeledContent("Range") {
                    HStack(spacing: 6) {
                        numberField($store.minHz, width: 70)
                        Text("to").foregroundStyle(.secondary)
                        numberField($store.maxHz, width: 70)
                        Text("Hz").foregroundStyle(.secondary)
                    }
                }
            }

            Section("Level") {
                LabeledContent("Range") {
                    HStack(spacing: 6) {
                        numberField($store.minDb, width: 70)
                        Text("to").foregroundStyle(.secondary)
                        numberField($store.maxDb, width: 70)
                        Text("dB").foregroundStyle(.secondary)
                    }
                }
                Picker("Gridlines every", selection: $store.levelGridStep) {
                    ForEach([Float(5), 6, 10, 12, 20], id: \.self) { step in
                        Text("\(Int(step)) dB").tag(step)
                    }
                }
            }

            Section {
                Text("""
                    A range the core cannot use — an inverted span, or a low end \
                    at or below zero hertz on a logarithmic axis — is replaced \
                    with one it can, and the field snaps to show it.
                    """)
                .font(.caption)
                .foregroundStyle(.secondary)
            }

            footer
        }
        .formStyle(.grouped)
    }

    // ---------------------------------------------------------- calibration -

    private var calibration: some View {
        Form {
            Section("Sound pressure level") {
                Toggle("SPL calibrated", isOn: $store.hasSplOffset)
                    .help("""
                        Off means no calibration has been measured, which is not \
                        the same as a measured offset of zero. Levels stay in \
                        dBFS until this is set.
                        """)

                LabeledContent("Offset") {
                    HStack(spacing: 6) {
                        numberField($store.splOffsetDb, width: 80)
                        Text("dB").foregroundStyle(.secondary)
                    }
                }
                .disabled(!store.hasSplOffset)
                .opacity(store.hasSplOffset ? 1 : 0.4)
            }

            Section("Microphone correction") {
                LabeledContent("File") {
                    HStack(spacing: 8) {
                        Text(store.micCalPath.isEmpty
                             ? "None"
                             : (store.micCalPath as NSString).lastPathComponent)
                            .foregroundStyle(store.micCalPath.isEmpty ? .secondary : .primary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .help(store.micCalPath)
                        Spacer()
                        Button("Choose…") { chooseCalFile() }
                        Button("Clear") { store.micCalPath = "" }
                            .disabled(store.micCalPath.isEmpty)
                    }
                }
            }

            footer
        }
        .formStyle(.grouped)
    }

    // --------------------------------------------------------------- shared -

    @ViewBuilder
    private var footer: some View {
        Section {
            HStack {
                if let message = store.errorMessage {
                    Label(message, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .font(.caption)
                        .lineLimit(2)
                }
                Spacer()
                Button("Restore Defaults") { store.resetToDefaults() }
            }
        }
    }

    private func numberField(_ value: Binding<Float>, width: CGFloat) -> some View {
        TextField("", value: value, format: .number.precision(.fractionLength(0...2)))
            .textFieldStyle(.roundedBorder)
            .frame(width: width)
            .multilineTextAlignment(.trailing)
    }

    private func chooseCalFile() {
        let panel = NSOpenPanel()
        panel.title = "Choose a microphone correction file"
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        // .cal, .frd and plain text are the formats these ship in; the core
        // decides whether the contents parse. Neither extension has a declared
        // system type, so they are constructed from the extension itself.
        panel.allowedContentTypes = ["cal", "frd"].compactMap {
            UTType(filenameExtension: $0)
        } + [.plainText]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        store.micCalPath = url.path
    }
}
