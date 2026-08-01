import SwiftUI
import MetalKit
import UniformTypeIdentifiers
import AnalyzerFFI

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
    static let fftSizes: [UInt32] = [1024, 2048, 4096, 8192, 16384, 32768, 65536, 131_072]

    let minHz: Float = 20
    let maxHz: Float = 20_000
    let minDb: Float = -120
    let maxDb: Float = 0

    private(set) var session: AnalyzerSessionHandle?

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
                    This device has no output. A transfer function against an internal reference                     has to play the stimulus itself - create an aggregate device in Audio MIDI                     Setup combining your input and output, and select it here.
                    """
            }
            if signal == AnalyzerSignal_Silence {
                return """
                    Choose a stimulus. An internal reference is the generator's own signal, so                     there is nothing to reference while it is silent.
                    """
            }
        } else if inputChannels < 2 {
            return """
                This device has one input channel, so there is nowhere to wire a loopback. Use                 an internal reference instead.
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
            // The core starts every session with the equaliser off, so the
            // mode has to be re-applied rather than assumed to survive.
            applyEqMode()
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

/// Hosts an `MTKView` and drives it from the model.
struct SpectrumView: NSViewRepresentable {
    @ObservedObject var model: AnalyzerModel

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView()
        view.device = MTLCreateSystemDefaultDevice()
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0.07, green: 0.08, blue: 0.10, alpha: 1)
        // ProMotion displays run at 120; a plain 60 cap would leave half the
        // refresh rate unused on exactly the hardware this targets.
        view.preferredFramesPerSecond = 120
        // Redraw on a timer but let the coordinator skip frames with no new data,
        // rather than redrawing identical pixels.
        view.isPaused = false
        view.enableSetNeedsDisplay = false

        if let renderer = SpectrumRenderer(view: view) {
            context.coordinator.renderer = renderer
            context.coordinator.attach(to: view)
            view.delegate = renderer
        }
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {
        context.coordinator.model = model
    }

    @MainActor
    final class Coordinator {
        var model: AnalyzerModel
        var renderer: SpectrumRenderer?
        private var lastColumns = 0
        private var distortionCountdown = 0

        init(model: AnalyzerModel) {
            self.model = model
        }

        func attach(to view: MTKView) {
            guard let renderer else { return }

            renderer.onResize = { [weak self] size in
                guard let self else { return }
                MainActor.assumeIsolated {
                    self.model.session?.setPlot(
                        width: Float(size.width),
                        height: Float(size.height),
                        minHz: self.model.minHz,
                        maxHz: self.model.maxHz,
                        minDb: self.model.minDb,
                        maxDb: self.model.maxDb
                    )
                }
            }

            // Back to front. The long-term average sits under the live trace,
            // and coherence sits under everything because it is context for the
            // curve above it rather than the thing being read.
            renderer.layers = [
                layer(colour: SIMD4(0.55, 0.55, 0.62, 0.55), range: (0, 1)) { session, columns in
                    guard self.model.mode == AnalyzerMode_Transfer, self.model.showCoherence else {
                        return [][...]
                    }
                    return session.copyTransfer(AnalyzerCurve_Coherence, columns: columns)
                },
                layer(colour: SIMD4(0.55, 0.45, 0.95, 0.8), range: (-180, 180)) { session, columns in
                    guard self.model.mode == AnalyzerMode_Transfer, self.model.showPhase else {
                        return [][...]
                    }
                    return session.copyTransfer(AnalyzerCurve_Phase, columns: columns)
                },
                layer(colour: SIMD4(1.0, 0.65, 0.25, 0.9), range: levelRange) { session, columns in
                    guard self.model.showAverage else { return [][...] }
                    return session.copyAverage(columns: columns)
                },
                // The equaliser's own curve, on the same decibel axis as
                // everything else so a 6 dB cut looks like 6 dB.
                layer(colour: SIMD4(0.95, 0.35, 0.55, 0.85), range: levelRange) { session, columns in
                    guard self.model.eqMode != AnalyzerEqMode_Off else { return [][...] }
                    return session.copyEqCurve(columns: columns)
                },
                // What the measurement would look like corrected. The whole
                // reason an equaliser belongs in a measurement tool.
                layer(colour: SIMD4(0.4, 0.75, 1.0, 0.9), range: levelRange) { session, columns in
                    guard self.model.eqMode != AnalyzerEqMode_Off,
                          self.model.showCorrected,
                          self.model.mode == AnalyzerMode_Spectrum
                    else { return [][...] }
                    return session.copyCorrected(columns: columns)
                },
                // The main curve, and the one that carries the per-frame
                // bookkeeping: it is drawn every frame and the others are not.
                layer(colour: SIMD4(0.35, 0.85, 0.45, 1.0), range: levelRange) { session, columns in
                    self.refreshReadouts(session)
                    return self.model.mode == AnalyzerMode_Transfer
                        ? session.copyTransfer(AnalyzerCurve_Magnitude, columns: columns)
                        : session.copyTrace(columns: columns)
                },
            ]

            renderer.gridProvider = { [weak self] in
                guard let self else { return [] }
                return MainActor.assumeIsolated { self.gridLines() }
            }
        }

        /// The magnitude axis, which both spectrum and transfer function use.
        private var levelRange: (min: Float, max: Float) { (model.minDb, model.maxDb) }

        /// Build a layer that only runs while a session exists, and that
        /// re-declares the plot geometry the first time the width changes.
        private func layer(
            colour: SIMD4<Float>,
            range: (min: Float, max: Float),
            body: @escaping (AnalyzerSessionHandle, Int) -> ArraySlice<Float>
        ) -> SpectrumRenderer.Layer {
            SpectrumRenderer.Layer(
                provider: { [weak self] columns in
                    guard let self else { return [][...] }
                    return MainActor.assumeIsolated {
                        guard let session = self.model.session else { return [][...] }
                        self.syncGeometry(session, columns: columns)
                        return body(session, columns)
                    }
                },
                range: range,
                colour: colour
            )
        }

        /// One column count for every layer in a frame, so the curves overlay
        /// exactly instead of being a pixel apart from each other.
        private func syncGeometry(_ session: AnalyzerSessionHandle, columns: Int) {
            guard columns != lastColumns else { return }
            lastColumns = columns
            session.setPlot(
                width: Float(columns),
                height: 1,
                minHz: model.minHz,
                maxHz: model.maxHz,
                minDb: model.minDb,
                maxDb: model.maxDb
            )
        }

        private func refreshReadouts(_ session: AnalyzerSessionHandle) {
            if let info = session.frameInfo {
                model.overruns = info.overruns
                model.framesAveraged = info.framesAveraged
                model.averageFrames = info.averageFrames
            }
            model.transfer = model.mode == AnalyzerMode_Transfer ? session.transferInfo : nil

            // Distortion is a per-frame read but is only worth recomputing at a
            // rate a human can follow, not at 120 Hz.
            distortionCountdown -= 1
            if distortionCountdown <= 0 {
                distortionCountdown = 30
                model.distortion = session.distortion()
            }
        }

        /// Gridlines in clip space, taken from the core's tick positions so the
        /// lines and the SwiftUI labels above them cannot disagree.
        private func gridLines() -> [SIMD2<Float>] {
            guard let session = model.session, lastColumns > 0 else { return [] }
            var points: [SIMD2<Float>] = []

            for tick in session.frequencyTicks() where tick.major {
                let x = (tick.position / Float(lastColumns)) * 2 - 1
                points.append(SIMD2(x, -1))
                points.append(SIMD2(x, 1))
            }
            for tick in session.levelTicks(step: 20) {
                let t = (tick.value - model.minDb) / (model.maxDb - model.minDb)
                let y = t * 2 - 1
                points.append(SIMD2(-1, y))
                points.append(SIMD2(1, y))
            }
            return points
        }
    }
}

struct ContentView: View {
    @StateObject private var model = AnalyzerModel()

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            Divider()
            transferBar
            plot
            if model.eqMode != AnalyzerEqMode_Off {
                EqualiserView(model: model)
            }
            Divider()
            statusBar
        }
        .frame(minWidth: 720, minHeight: 420)
        .onAppear {
            model.refreshDevices()
            model.start()
        }
        .onDisappear { model.stop() }
    }

    /// The second row, shown only in transfer mode.
    ///
    /// Kept out of the main toolbar rather than disabled in place: five extra
    /// controls greyed out is worse than five controls that are not there.
    @ViewBuilder
    private var transferBar: some View {
        if model.mode == AnalyzerMode_Transfer {
            HStack(spacing: 12) {
                Picker("Reference", selection: $model.reference) {
                    Text("Internal").tag(AnalyzerReference_Internal)
                    Text("Loopback").tag(AnalyzerReference_Input)
                }
                .pickerStyle(.segmented)
                .frame(width: 200)

                if model.reference == AnalyzerReference_Input {
                    Picker("Channel", selection: $model.referenceChannel) {
                        ForEach(0..<Int(model.inputChannels), id: \.self) { channel in
                            Text("\(channel + 1)").tag(UInt32(channel))
                        }
                    }
                    .frame(width: 120)
                }

                Button("Find delay") { model.findDelay() }
                    .disabled(!model.isRunning)
                    .help("""
                        Cross-correlate the two channels and remove the propagation delay.                         Without this the phase curve winds through hundreds of turns and                         coherence collapses well before 1 kHz.
                        """)

                if let transfer = model.transfer {
                    Text(transfer.delaySummary)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(transfer.estimating ? .orange : .secondary)
                        .frame(width: 140, alignment: .leading)
                }

                Divider().frame(height: 16)

                Toggle("Phase", isOn: $model.showPhase).toggleStyle(.checkbox)
                Toggle("Coherence", isOn: $model.showCoherence).toggleStyle(.checkbox)

                Spacer()
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(.quaternary.opacity(0.3))
            Divider()
        }
    }

    /// Generator controls, shown whenever the device can play.
    @ViewBuilder
    private var generator: some View {
        Picker("Signal", selection: $model.signal) {
            Text("Off").tag(AnalyzerSignal_Silence)
            Text("Pink").tag(AnalyzerSignal_PinkNoise)
            Text("White").tag(AnalyzerSignal_WhiteNoise)
            Text("Sine").tag(AnalyzerSignal_Sine)
        }
        .frame(width: 150)
        .disabled(!model.canPlay)
        .help(model.canPlay
              ? "Stimulus played out of the selected device."
              : """
                This device has no output. Create an aggregate device in Audio MIDI Setup to                 play and capture together.
                """)

        if model.signal != AnalyzerSignal_Silence {
            // Labelled in dBFS and capped below full scale. A generator that
            // defaults to loud is a generator that damages something.
            Slider(value: $model.signalLevelDb, in: -60...0) {
                Text("Level")
            }
            .frame(width: 110)
            Text(String(format: "%.0f dB", model.signalLevelDb))
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 46, alignment: .trailing)
        }

        if model.signal == AnalyzerSignal_Sine {
            Slider(value: $model.signalHz, in: 20...20_000) { Text("Frequency") }
                .frame(width: 110)
            Text(formatFrequency(model.signalHz))
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 46, alignment: .trailing)
        }
    }

    private var toolbar: some View {
        HStack(spacing: 12) {
            Picker("Input", selection: Binding(
                get: { model.selectedDeviceUID ?? "" },
                set: { model.selectedDeviceUID = $0.isEmpty ? nil : $0 }
            )) {
                ForEach(model.devices) { device in
                    Text(device.name).tag(device.uid)
                }
            }
            .frame(maxWidth: 260)

            // Labelled with the time each frame spans, because that is the
            // cost being paid: 131072 points buys 0.37 Hz resolution and 2.7
            // seconds of latency, and a picker showing only the point count
            // hides half the trade.
            Picker("FFT", selection: $model.fftSize) {
                ForEach(AnalyzerModel.fftSizes, id: \.self) { size in
                    Text(model.fftLabel(size)).tag(size)
                }
            }
            .frame(width: 190)

            Picker("Window", selection: $model.window) {
                Text("Hann").tag(AnalyzerWindow_Hann)
                Text("Blackman-Harris").tag(AnalyzerWindow_BlackmanHarris)
                Text("Flat-top").tag(AnalyzerWindow_FlatTop)
                Text("Rectangular").tag(AnalyzerWindow_Rectangular)
            }
            .frame(width: 200)

            Picker("Average", selection: $model.averaging) {
                Text("Fast").tag(AnalyzerAveraging_Fast)
                Text("None").tag(AnalyzerAveraging_None)
                Text("Infinite").tag(AnalyzerAveraging_Infinite)
                Text("Peak hold").tag(AnalyzerAveraging_PeakHold)
            }
            .frame(width: 160)

            Picker("Mode", selection: $model.mode) {
                Text("RTA").tag(AnalyzerMode_Spectrum)
                Text("Transfer").tag(AnalyzerMode_Transfer)
            }
            .pickerStyle(.segmented)
            .frame(width: 150)

            Picker("EQ", selection: $model.eqMode) {
                Text("Off").tag(AnalyzerEqMode_Off)
                Text("Graphic").tag(AnalyzerEqMode_Graphic)
                Text("Parametric").tag(AnalyzerEqMode_Parametric)
            }
            .frame(width: 160)
            .help("""
                The equaliser is drawn against the measurement and applied to the                 generator, so the corrected curve is a prediction you can also hear.
                """)

            generator

            Spacer()

            Toggle("Average", isOn: $model.showAverage)
                .toggleStyle(.checkbox)
            Button("Reset avg") { model.session?.resetAverage() }
                .disabled(!model.isRunning)

            Menu("Save") {
                Button("Measurement…") { model.save(asText: false) }
                Button("Text (REW)…") { model.save(asText: true) }
            }
            .frame(width: 90)
            .disabled(!model.isRunning)

            Button(model.isRunning ? "Stop" : "Start") {
                model.isRunning ? model.stop() : model.start()
            }
        }
        .padding(10)
    }

    private var plot: some View {
        GeometryReader { geometry in
            ZStack(alignment: .topLeading) {
                SpectrumView(model: model)

                // Axis labels are native text drawn over the Metal surface.
                // Sharper than a glyph atlas and a fraction of the code, and the
                // positions come from the same tick data the gridlines use.
                ForEach(Array(frequencyLabels(width: geometry.size.width).enumerated()), id: \.offset) { _, label in
                    Text(label.text)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .position(x: label.x, y: geometry.size.height - 10)
                }
                ForEach(Array(levelLabels(height: geometry.size.height).enumerated()), id: \.offset) { _, label in
                    Text(label.text)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .position(x: 26, y: label.y)
                }

                if let message = model.errorMessage {
                    Text(message)
                        .font(.callout)
                        .padding(12)
                        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
                        .frame(maxWidth: 520)
                        .padding(24)
                }
            }
        }
    }

    private var statusBar: some View {
        HStack(spacing: 16) {
            Text(model.deviceName.isEmpty ? "not capturing" : model.deviceName)
            Text("\(model.framesAveraged) frames")
            if model.showAverage {
                // The average is only as good as the count behind it, so the
                // count is shown rather than left to be assumed.
                Text("avg \(model.averageFrames)")
                    .foregroundStyle(.orange)
            }
            if model.overruns > 0 {
                // Never hidden. A spectrum computed across dropped audio is
                // wrong rather than merely noisy.
                Label("\(model.overruns) dropped", systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
            }
            if let transfer = model.transfer {
                // The count matters: coherence is identically one for a single
                // frame, so an unsettled curve looks perfect and is not.
                Text("tf \(transfer.frames)")
                    .foregroundStyle(transfer.isSettled ? Color.secondary : Color.orange)
                if !transfer.isSettled {
                    Text("settling")
                        .foregroundStyle(.orange)
                }
            }
            if let distortion = model.distortion {
                // Only shown when a tone is actually present; otherwise the
                // figure would be the distortion of room noise.
                Text(distortion.summary)
                    .foregroundStyle(.cyan)
                    .help(
                        distortion.harmonics
                            .prefix(5)
                            .map { String(format: "H%d %.3f%%", $0.order, $0.percent) }
                            .joined(separator: "  ")
                    )
            }
            Spacer()
            if model.mode == AnalyzerMode_Transfer {
                Text("magnitude dB · phase ±180° · coherence 0-1")
            } else {
                Text("0 dBFS = full scale sine")
            }
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
    }

    private func frequencyLabels(width: CGFloat) -> [(text: String, x: CGFloat)] {
        guard let session = model.session, width > 0 else { return [] }
        return session.frequencyTicks()
            .filter(\.major)
            .map { (formatFrequency($0.value), CGFloat($0.position / Float(width) * Float(width))) }
    }

    private func levelLabels(height: CGFloat) -> [(text: String, y: CGFloat)] {
        guard let session = model.session, height > 0 else { return [] }
        return session.levelTicks(step: 20).map { tick in
            let t = (model.maxDb - tick.value) / (model.maxDb - model.minDb)
            return ("\(Int(tick.value))", CGFloat(t) * height)
        }
    }
}
