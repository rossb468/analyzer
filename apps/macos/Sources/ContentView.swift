import SwiftUI
import MetalKit
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
        do {
            let handle = try AnalyzerSessionHandle(
                deviceUID: selectedDeviceUID,
                fftSize: fftSize,
                window: window,
                averaging: averaging
            )
            session = handle
            deviceName = handle.deviceName
            isRunning = true
            errorMessage = nil
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

        init(model: AnalyzerModel) {
            self.model = model
        }

        func attach(to view: MTKView) {
            guard let renderer else { return }
            renderer.levelRange = (model.minDb, model.maxDb)

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

            renderer.traceProvider = { [weak self] columns in
                guard let self else { return [][...] }
                return MainActor.assumeIsolated {
                    guard let session = self.model.session else { return [][...] }
                    if columns != self.lastColumns {
                        self.lastColumns = columns
                        session.setPlot(
                            width: Float(columns),
                            height: 1,
                            minHz: self.model.minHz,
                            maxHz: self.model.maxHz,
                            minDb: self.model.minDb,
                            maxDb: self.model.maxDb
                        )
                    }
                    let trace = session.copyTrace(columns: columns)
                    if let info = session.frameInfo {
                        self.model.overruns = info.overruns
                        self.model.framesAveraged = info.framesAveraged
                        self.model.averageFrames = info.averageFrames
                    }
                    return trace
                }
            }

            renderer.averageProvider = { [weak self] columns in
                guard let self else { return [][...] }
                return MainActor.assumeIsolated {
                    guard self.model.showAverage, let session = self.model.session else {
                        return [][...]
                    }
                    return session.copyAverage(columns: columns)
                }
            }

            renderer.gridProvider = { [weak self] in
                guard let self else { return [] }
                return MainActor.assumeIsolated { self.gridLines() }
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
            plot
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

            Spacer()

            Toggle("Average", isOn: $model.showAverage)
                .toggleStyle(.checkbox)
            Button("Reset avg") { model.session?.resetAverage() }
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
            Spacer()
            Text("0 dBFS = full scale sine")
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
