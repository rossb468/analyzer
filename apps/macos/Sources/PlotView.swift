import SwiftUI
import MetalKit
import AnalyzerFFI

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
        private var lastAxisGeneration = -1
        private var drawableSize = CGSize.zero
        private var distortionCountdown = 0

        init(model: AnalyzerModel) {
            self.model = model
        }

        func attach(to view: MTKView) {
            guard let renderer else { return }

            renderer.onResize = { [weak self, weak view] size in
                guard let self else { return }
                MainActor.assumeIsolated {
                    self.drawableSize = size
                    // The core is told the geometry in drawable pixels, because
                    // that is the resolution the trace is reduced to. The views
                    // that place labels work in points, so the ratio has to
                    // travel with it - see `AnalyzerModel.plotScale`.
                    if let bounds = view?.bounds.width, bounds > 0 {
                        self.model.plotScale = size.width / bounds
                    }
                    self.declareGeometry(width: Float(size.width), height: Float(size.height))
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
                // What the correction is aiming at. Drawn under the equaliser
                // and the measurement, because it is the thing being aimed at
                // rather than the thing being read.
                layer(colour: SIMD4(0.85, 0.80, 0.35, 0.75), range: levelRange) { session, columns in
                    guard self.model.showTarget else { return [][...] }
                    return session.copyTarget(columns: columns)
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
                        self.syncGeometry(columns: columns)
                        return body(session, columns)
                    }
                },
                range: range,
                colour: colour
            )
        }

        /// One column count for every layer in a frame, so the curves overlay
        /// exactly instead of being a pixel apart from each other.
        ///
        /// The resize callback normally gets here first; this covers the first
        /// frame of a session started against an already-sized view, and a
        /// change to the axis range made in the Settings window while the view
        /// is the same size it already was.
        private func syncGeometry(columns: Int) {
            let generation = model.axisGeneration
            guard columns != lastColumns || generation != lastAxisGeneration else { return }
            lastColumns = columns
            lastAxisGeneration = generation
            let height = drawableSize.height > 0 ? Float(drawableSize.height) : Float(columns) / 2
            declareGeometry(width: Float(columns), height: height)
        }

        private func declareGeometry(width: Float, height: Float) {
            model.session?.setPlot(
                width: width,
                height: height,
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
            for tick in session.levelTicks(step: model.levelGridStep) {
                let t = (tick.value - model.minDb) / (model.maxDb - model.minDb)
                let y = t * 2 - 1
                points.append(SIMD2(-1, y))
                points.append(SIMD2(1, y))
            }
            return points
        }
    }
}

/// The Metal surface plus the native axis labels drawn over it.
///
/// The labels are positioned from the core's own tick data, converted from
/// drawable pixels to points. That conversion is the only arithmetic here: the
/// frequency-to-x and decibel-to-y mappings themselves come from Rust, so a
/// label cannot drift away from the gridline under it.
struct PlotView: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .topLeading) {
                SpectrumView(model: model)

                // Axis labels are native text drawn over the Metal surface.
                // Sharper than a glyph atlas and a fraction of the code.
                ForEach(Array(frequencyLabels().enumerated()), id: \.offset) { _, label in
                    Text(label.text)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .position(x: label.x, y: geometry.size.height - 10)
                }
                ForEach(Array(levelLabels().enumerated()), id: \.offset) { _, label in
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

    private func frequencyLabels() -> [(text: String, x: CGFloat)] {
        guard let session = model.session, model.plotScale > 0 else { return [] }
        return session.frequencyTicks()
            .filter(\.major)
            .map { (formatFrequency($0.value), CGFloat($0.position) / model.plotScale) }
    }

    private func levelLabels() -> [(text: String, y: CGFloat)] {
        guard let session = model.session, model.plotScale > 0 else { return [] }
        return session.levelTicks(step: model.levelGridStep).map { tick in
            (String(Int(tick.value)), CGFloat(session.y(forLevel: tick.value)) / model.plotScale)
        }
    }
}
