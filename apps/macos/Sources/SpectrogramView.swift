import SwiftUI
import MetalKit
import AnalyzerFFI

/// Hosts the spectrogram's `MTKView`.
struct SpectrogramSurface: NSViewRepresentable {
    @ObservedObject var model: AnalyzerModel

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView()
        view.device = MTLCreateSystemDefaultDevice()
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0.03, green: 0.03, blue: 0.09, alpha: 1)
        view.preferredFramesPerSecond = 120
        view.isPaused = false
        view.enableSetNeedsDisplay = false

        if let renderer = SpectrogramRenderer(view: view) {
            context.coordinator.renderer = renderer
            context.coordinator.attach()
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
        var renderer: SpectrogramRenderer?

        init(model: AnalyzerModel) {
            self.model = model
        }

        func attach() {
            guard let renderer else { return }

            renderer.columnProvider = { [weak self] rows in
                guard let self else { return nil }
                return MainActor.assumeIsolated {
                    guard let session = self.model.session else { return nil }
                    // Only append when the analysis has actually produced
                    // something. Without this the display's refresh rate, not
                    // the analysis rate, would decide how fast time scrolls.
                    guard session.hasNewFrame else { return nil }
                    return session.copySpectrogramColumn(rows: rows)
                }
            }

            renderer.levelRange = { [weak self] in
                guard let self else { return (-120, 0) }
                return MainActor.assumeIsolated {
                    // The spectrogram's ramp spans a narrower range than the
                    // trace plot's axis. Mapping a 120 dB span onto the ramp
                    // leaves everything but the loudest peaks in the first
                    // colour, because a room's useful detail lives in the top
                    // 60 dB or so.
                    (self.model.spectrogramFloorDb, self.model.maxDb)
                }
            }
        }
    }
}

/// The spectrogram, with its frequency axis labelled down the side.
struct SpectrogramView: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .topLeading) {
                SpectrogramSurface(model: model)

                // Frequency runs up the drawable here rather than across it, so
                // the tick positions come from the core with the axis asked for
                // in that orientation.
                ForEach(Array(frequencyLabels(height: geometry.size.height).enumerated()),
                        id: \.offset) { _, label in
                    Text(label.text)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.white.opacity(0.75))
                        .shadow(radius: 2)
                        .position(x: 26, y: label.y)
                }

                Text("time →")
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(.white.opacity(0.5))
                    .position(x: geometry.size.width - 34, y: geometry.size.height - 12)

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

    /// Positions for the frequency ticks, flipped so low sits at the bottom.
    ///
    /// The mapping comes from the core, asked for along an axis as long as this
    /// view is tall. That query builds a temporary axis rather than redeclaring
    /// the session's, so labelling the spectrogram cannot disturb the geometry
    /// the trace plot is drawing against.
    private func frequencyLabels(height: CGFloat) -> [(text: String, y: CGFloat)] {
        guard let session = model.session, height > 0 else { return [] }
        return session.frequencyTicks(along: Float(height))
            .filter(\.major)
            .map { tick in
                // The axis runs low to high; the drawable runs top to bottom.
                (formatFrequency(tick.value), height - CGFloat(tick.position))
            }
    }
}
