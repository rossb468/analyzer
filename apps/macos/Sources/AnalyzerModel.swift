import SwiftUI
import AppKit
import Combine
import UniformTypeIdentifiers
import AnalyzerFFI

/// A section of the app, as listed in the sidebar.
///
/// Two kinds live in one list. `rta`, `transfer`, `measure` and `spectrogram`
/// decide what the plot draws and so are mutually exclusive; `equaliser` and
/// `traces` are editors whose output overlays whichever of those is active.
/// Selecting one of the latter therefore leaves the plot alone — the sidebar
/// says what is being edited, not what is drawn.
enum AppSection: String, CaseIterable, Identifiable, Hashable {
    case rta
    case transfer
    case equaliser

    var id: String { rawValue }

    /// Sections that decide what the plot draws, listed above the editors.
    static let analysis: [AppSection] = [.rta, .transfer]
    /// Sections that edit something drawn over whatever analysis is running.
    static let editors: [AppSection] = [.equaliser]

    var title: String {
        switch self {
        case .rta: "RTA"
        case .transfer: "Transfer"
        case .equaliser: "Equaliser"
        }
    }

    var symbol: String {
        switch self {
        case .rta: "waveform"
        case .transfer: "arrow.left.arrow.right"
        case .equaliser: "slider.vertical.3"
        }
    }

    /// Whether choosing this section changes what the plot shows.
    var drivesPlot: Bool { Self.analysis.contains(self) }

    /// The analysis mode this section needs, for the ones that drive the plot.
    var analysisMode: AnalyzerMode? {
        switch self {
        case .rta: AnalyzerMode_Spectrum
        case .transfer: AnalyzerMode_Transfer
        case .equaliser: nil
        }
    }
}

/// Holds the session and the settings the UI can change.
///
/// Nothing here computes an analysis value. It starts and stops a session, moves
/// settings across the boundary, and asks Rust where things go on screen.
@MainActor
final class AnalyzerModel: ObservableObject {
    @Published var devices: [AudioDevice] = []
    @Published var selectedDeviceUID: String?
    @Published var errorMessage: String?
    @Published var isRunning = false
    @Published var deviceName = ""
    @Published var overruns: UInt64 = 0
    @Published var framesAveraged: UInt32 = 0
    @Published var averageFrames: UInt32 = 0
    /// Whether the long-term average trace is drawn.
    @Published var showAverage = true
    /// Most recent distortion reading, or nil when no tone stands clear of the
    /// noise floor.
    @Published var distortion: DistortionReading?
    /// State of the transfer function, or nil when none is running.
    @Published var transfer: TransferInfo?

    /// Drawable pixels per point.
    ///
    /// The core is given the plot geometry in drawable pixels, because that is
    /// the resolution the trace is reduced to; SwiftUI places labels in points.
    /// Without this the axis labels sit at double their correct position on
    /// every Retina display.
    @Published var plotScale: CGFloat = 1

    // ------------------------------------------------------------- sections -

    /// Which section the sidebar has selected.
    @Published var section: AppSection = .rta { didSet { sectionChanged() } }
    /// Whether the inspector is showing. Sticky across section changes, because
    /// hiding it is a deliberate act of reclaiming width.
    @Published var showInspector = true

    /// The section currently deciding what the plot draws.
    ///
    /// Selecting the equaliser or the trace list must not blank the curve being
    /// worked on, so those selections leave this where it was.
    @Published private(set) var plotSection: AppSection = .rta

    /// Which equaliser is active.
    @Published var eqMode: AnalyzerEqMode = AnalyzerEqMode_Off { didSet { applyEqMode() } }
    /// Bands of the active equaliser, mirrored for the UI to bind against.
    @Published var eqBands: [EqBand] = []
    /// Headroom of the active equaliser.
    @Published var eqInfo: EqInfo?
    /// Whether the corrected trace - measurement plus equaliser - is drawn.
    @Published var showCorrected = true

    /// What the session computes. Changing this restarts it, because the
    /// transfer function needs a second channel and possibly an output.
    @Published var mode: AnalyzerMode = AnalyzerMode_Spectrum { didSet { restartIfRunning() } }
    @Published var reference: AnalyzerReference = AnalyzerReference_Internal {
        didSet { restartIfRunning() }
    }
    @Published var referenceChannel: UInt32 = 1 { didSet { restartIfRunning() } }
    /// Whether phase is drawn over the magnitude.
    @Published var showPhase = false
    /// Whether coherence is drawn over the magnitude.
    @Published var showCoherence = true

    /// Stimulus. Changing the signal itself needs a restart only when it turns
    /// the output on or off; level and frequency are live.
    @Published var signal: AnalyzerSignal = AnalyzerSignal_Silence {
        didSet { signalChanged(wasSilent: oldValue == AnalyzerSignal_Silence) }
    }
    @Published var signalLevelDb: Float = -20 { didSet { pushSignal() } }
    @Published var signalHz: Float = 1000 { didSet { pushSignal() } }

    @Published var fftSize: UInt32 = 4096 { didSet { restartIfRunning() } }
    @Published var window: AnalyzerWindow = AnalyzerWindow_Hann { didSet { restartIfRunning() } }
    @Published var averaging: AnalyzerAveraging = AnalyzerAveraging_Fast { didSet { restartIfRunning() } }

    /// Selectable transform sizes.
    ///
    /// The upper end is genuinely useful for room work - a room mode is a few
    /// hertz wide and 4096 points cannot resolve one - but it is slow to settle,
    /// so the picker shows the time cost alongside.
    ///
    /// Must match `analyzer_model::settings::FFT_SIZES`, which is what validates
    /// a hand-edited preferences file.
    static let fftSizes: [UInt32] = [1024, 2048, 4096, 8192, 16384, 32768, 65536, 131_072]

    /// Program settings. The plot's axis range lives here rather than on the
    /// model, because it survives the session that drew through it.
    let settings: SettingsStore

    var minHz: Float { settings.minHz }
    var maxHz: Float { settings.maxHz }
    var minDb: Float { settings.minDb }
    var maxDb: Float { settings.maxDb }
    var levelGridStep: Float { settings.levelGridStep }
    /// Bumped by the settings store whenever an axis value changes.
    var axisGeneration: Int { settings.axisGeneration }

    private var settingsSubscription: AnyCancellable?

    private(set) var session: AnalyzerSessionHandle?

    /// Starts from the stored defaults rather than from constants, so the
    /// Settings window's "new sessions start with" actually decides that.
    init(settings: SettingsStore) {
        self.settings = settings
        fftSize = settings.fftSize
        window = settings.window
        averaging = settings.averaging

        // Views bind to the model, not to the store, so a change to the axis
        // range has to be republished or nothing redraws until the next frame
        // happens to touch the model.
        settingsSubscription = settings.objectWillChange.sink { [weak self] _ in
            self?.objectWillChange.send()
        }
    }

    /// Whether the selected device can play a stimulus.
    ///
    /// CoreAudio drives one device from one IOProc, so playing and capturing
    /// together means one device doing both. A laptop's built-in input and
    /// output are separate devices, which is why this is so often false and why
    /// the answer is an aggregate device in Audio MIDI Setup.
    var canPlay: Bool {
        devices.first { $0.uid == selectedDeviceUID }?.canPlay ?? false
    }

    /// Input channels the selected device offers.
    var inputChannels: UInt32 {
        devices.first { $0.uid == selectedDeviceUID }?.inputChannels ?? 1
    }

    /// Whether a transfer function can be started with the current selection.
    ///
    /// An internal reference needs an output to reference; a loopback reference
    /// needs a second input channel to carry it.
    var transferBlocker: String? {
        guard mode == AnalyzerMode_Transfer else { return nil }
        if reference == AnalyzerReference_Internal {
            if !canPlay {
                return """
                    This device has no output. A transfer function against an \
                    internal reference has to play the stimulus itself — create \
                    an aggregate device in Audio MIDI Setup combining your input \
                    and output, and select it here.
                    """
            }
            if signal == AnalyzerSignal_Silence {
                return """
                    Choose a stimulus. An internal reference is the generator's \
                    own signal, so there is nothing to reference while it is \
                    silent.
                    """
            }
        } else if inputChannels < 2 {
            return """
                This device has one input channel, so there is nowhere to wire a \
                loopback. Use an internal reference instead.
                """
        }
        return nil
    }

    /// Rate the current session runs at, for labelling.
    private var displayRate: Double {
        devices.first { $0.uid == selectedDeviceUID }?.sampleRate ?? 48_000
    }

    /// "16384 · 341 ms · 2.9 Hz" - point count, frame length, bin spacing.
    func fftLabel(_ size: UInt32) -> String {
        let rate = displayRate
        let seconds = Double(size) / rate
        let spacing = rate / Double(size)
        let time = seconds >= 1.0
            ? String(format: "%.1f s", seconds)
            : String(format: "%.0f ms", seconds * 1000)
        return "\(size) · \(time) · \(String(format: "%.2g", spacing)) Hz"
    }

    func refreshDevices() {
        devices = availableInputDevices()
        if selectedDeviceUID == nil {
            selectedDeviceUID = devices.first(where: \.isDefaultInput)?.uid ?? devices.first?.uid
        }
    }

    func start() {
        stop()
        if let blocker = transferBlocker {
            errorMessage = blocker
            isRunning = false
            return
        }
        do {
            let handle = try AnalyzerSessionHandle(
                deviceUID: selectedDeviceUID,
                fftSize: fftSize,
                window: window,
                averaging: averaging,
                mode: mode,
                reference: reference,
                referenceChannel: referenceChannel,
                signal: signal,
                signalLevelDb: signalLevelDb,
                signalHz: signalHz
            )
            session = handle
            deviceName = handle.deviceName
            isRunning = true
            errorMessage = nil
            // The core starts every session with the equaliser off and a flat
            // target, so both have to be re-applied rather than assumed to
            // survive the restart that a settings change causes.
            applyEqMode()
            applyTarget()
        } catch {
            // The most common failure by far is microphone permission, and the
            // Rust side already explains that case in detail rather than just
            // returning a status code.
            errorMessage = error.localizedDescription
            isRunning = false
        }
    }

    func stop() {
        session?.stop()
        session = nil
        isRunning = false
        transfer = nil
    }

    /// Measure the reference-to-measurement delay and remove it.
    func findDelay() {
        session?.estimateDelay()
    }

    /// A section that drives the plot takes it over; an editor section does not.
    private func sectionChanged() {
        guard section.drivesPlot else { return }
        plotSection = section
        if let wanted = section.analysisMode, wanted != mode {
            mode = wanted
        }
    }

    // ------------------------------------------------------------ equaliser -

    /// Push the mode to the core and pull back whatever bands it now has.
    ///
    /// The core owns the bands; this list is a mirror. Editing the mirror and
    /// hoping the two stay in step is how a fader ends up controlling the wrong
    /// filter.
    private func applyEqMode() {
        session?.setEqMode(eqMode)
        refreshEq()
    }

    func refreshEq() {
        eqBands = session?.eqBands() ?? []
        eqInfo = session?.eqInfo
    }

    func setEqGain(_ index: Int, _ gainDb: Float) {
        session?.setEqGain(index, gainDb)
        refreshEq()
    }

    func setEqBand(_ index: Int, _ band: EqBand) {
        session?.setEqBand(index, band)
        refreshEq()
    }

    func addEqBand() {
        guard session?.addEqBand(EqBand(
            id: 0,
            kind: AnalyzerFilterKind_Peaking,
            hz: 1000,
            gainDb: 0,
            q: 4,
            enabled: true
        )) != nil else {
            errorMessage = "The equaliser is full."
            return
        }
        refreshEq()
    }

    func removeEqBand(_ index: Int) {
        session?.removeEqBand(index)
        refreshEq()
    }

    func flattenEq() {
        session?.flattenEq()
        refreshEq()
    }

    /// Trim the output so the equaliser's loudest point sits at unity.
    func trimEq() {
        session?.trimEq()
        refreshEq()
    }

    // -------------------------------------------------------------- target -

    /// The response a correction is aiming at. Mirrored from the core, which
    /// owns it, for the same reason the equaliser bands are.
    @Published var target = analyzer_target_default()
    /// Whether the target curve is drawn.
    @Published var showTarget = false

    func refreshTarget() {
        target = session?.target ?? analyzer_target_default()
    }

    /// Push the mirrored target back to the core and re-read what it made of it.
    func applyTarget() {
        session?.setTarget(target)
        refreshTarget()
    }

    /// Align the target to the current measurement.
    ///
    /// A target says nothing about absolute level, so until this runs it sits
    /// wherever the offset happens to be rather than on the curve.
    func alignTarget() {
        guard session?.alignTarget() == true else {
            errorMessage = "Nothing has been captured yet to align against."
            return
        }
        refreshTarget()
        showTarget = true
    }

    func loadTarget() {
        guard let session else { return }
        let panel = NSOpenPanel()
        panel.title = "Choose a target curve"
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.allowedContentTypes = ["frd", "cal"].compactMap {
            UTType(filenameExtension: $0)
        } + [.plainText]

        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try session.loadTarget(from: url)
            refreshTarget()
            showTarget = true
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    /// Ask for a location and write the equaliser there.
    ///
    /// The formats differ in what they can carry, and the panel says so: a
    /// parametric target redesigns the filters itself, while miniDSP gets
    /// coefficients designed at this session's sample rate and is only correct
    /// on a device running at that rate.
    func exportFilters(_ format: AnalyzerFilterFormat) {
        guard let session else { return }

        let panel = NSSavePanel()
        panel.title = "Export filters"
        panel.nameFieldStringValue = Self.filterFileName(format)
        panel.allowedContentTypes = [.plainText]
        panel.canCreateDirectories = true
        panel.message = Self.filterAdvice(format, sampleRate: displayRate)

        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try session.exportFilters(format, to: url)
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private static func filterFileName(_ format: AnalyzerFilterFormat) -> String {
        switch format {
        case AnalyzerFilterFormat_EqualizerApo: "config.txt"
        case AnalyzerFilterFormat_MiniDsp: "biquads.txt"
        default: "filters.txt"
        }
    }

    private static func filterAdvice(_ format: AnalyzerFilterFormat, sampleRate: Double) -> String {
        switch format {
        case AnalyzerFilterFormat_EqualizerApo:
            """
            Equalizer APO reads this as a config.txt. The trim is written as its \
            Preamp line.
            """
        case AnalyzerFilterFormat_MiniDsp:
            """
            Coefficients are designed for \(Int(sampleRate)) Hz and are only \
            correct on a device running at that rate. The trim is folded into \
            the first biquad, because the format has nowhere else to put it.
            """
        default:
            """
            REW imports this under Equaliser: Generic. The trim is a note in the \
            header, not a filter.
            """
        }
    }

    /// A stimulus turning on or off changes whether an output stream exists,
    /// which is a device operation; anything else is a live change the audio
    /// thread picks up on its next callback.
    private func signalChanged(wasSilent: Bool) {
        let nowSilent = signal == AnalyzerSignal_Silence
        if wasSilent != nowSilent {
            restartIfRunning()
        } else {
            pushSignal()
        }
    }

    private func pushSignal() {
        session?.setSignal(signal, levelDb: signalLevelDb, hz: signalHz)
    }

    /// Ask for a location and write the current spectrum there.
    ///
    /// The panel is driven from the model rather than the view so the error path
    /// has somewhere to land - a failed save that says nothing is worse than no
    /// save button.
    func save(asText: Bool) {
        guard let session else { return }
        let panel = NSSavePanel()
        panel.title = asText ? "Export measurement as text" : "Save measurement"
        panel.nameFieldStringValue = asText ? "measurement.txt" : "measurement.anlz"
        panel.allowedContentTypes = asText ? [.plainText] : []
        panel.canCreateDirectories = true

        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            let name = url.deletingPathExtension().lastPathComponent
            if asText {
                try session.exportText(to: url, name: name)
            } else {
                try session.save(to: url, name: name)
            }
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func restartIfRunning() {
        // The engine has no live reconfiguration by design; resizing analysis
        // buffers under a running audio callback is not worth the
        // synchronisation for something changed by clicking a menu.
        guard isRunning else { return }
        start()
    }
}
