import Metal
import MetalKit
import simd

/// Metal renderer for the running spectrogram.
///
/// The core hands over one column of decibel values per analysis frame — a few
/// kilobytes — and nothing else. This keeps a ring-buffer texture on the GPU,
/// writes that column into the next slot, and scrolls by advancing a texture
/// coordinate. Cost per frame is one row upload and one full-screen quad,
/// independent of how much history is on screen.
///
/// The alternative, compositing the image CPU-side, is what makes REW's
/// waterfall slow: at a Retina drawable of roughly 2800×1600 that is 18 MB per
/// frame, over 2 GB/s of writes at 120 fps.
final class SpectrogramRenderer: NSObject, MTKViewDelegate {

    /// Frequency resolution of the ring texture.
    ///
    /// Fixed rather than tied to the drawable so that resizing the window does
    /// not throw away the history. Generous enough to stay sharp at Retina
    /// heights; at four bytes a cell the whole texture is 8 MB.
    static let rows = 2048
    /// How many analysis frames of history the ring holds.
    static let history = 1024

    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct Uniforms {
        float offset;    // where the newest column sits in the ring, normalised
        float scale;     // (history - 1) / history, so the right edge is newest
        float minDb;
        float maxDb;
    };

    struct Varying {
        float4 position [[position]];
        float2 uv;
    };

    // A full-screen triangle. Cheaper than a quad and avoids the diagonal seam.
    vertex Varying spectrogram_vertex(uint vid [[vertex_id]]) {
        float2 corner = float2((vid << 1) & 2, vid & 2);
        Varying out;
        out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
        // x runs left to right as time, y runs bottom to top as frequency.
        out.uv = corner;
        return out;
    }

    // Level to colour. A dark-to-hot ramp: anything near the noise floor stays
    // out of the way, and the eye picks the bright end out immediately.
    static float3 ramp(float t) {
        const float3 stops[5] = {
            float3(0.03, 0.03, 0.09),
            float3(0.13, 0.20, 0.55),
            float3(0.10, 0.65, 0.50),
            float3(0.90, 0.80, 0.25),
            float3(0.95, 0.25, 0.20)
        };
        float scaled = saturate(t) * 4.0;
        int index = int(floor(scaled));
        index = min(index, 3);
        return mix(stops[index], stops[index + 1], scaled - float(index));
    }

    fragment float4 spectrogram_fragment(Varying in [[stage_in]],
                                         texture2d<float> levels [[texture(0)]],
                                         constant Uniforms &u [[buffer(1)]]) {
        constexpr sampler ring(filter::linear, address::repeat);

        // The texture is laid out frequency across, time down, so a whole
        // column is one contiguous row and the upload is a single write.
        float time = fract(u.offset + in.uv.x * u.scale);
        float frequency = in.uv.y;
        float db = levels.sample(ring, float2(frequency, time)).r;

        float span = max(u.maxDb - u.minDb, 1e-6);
        return float4(ramp((db - u.minDb) / span), 1.0);
    }
    """

    private struct Uniforms {
        var offset: Float = 0
        var scale: Float = 0
        var minDb: Float = -120
        var maxDb: Float = 0
    }

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let texture: MTLTexture

    /// Next ring slot to write. The slot before it holds the newest column.
    private var writeSlot = 0

    /// Asked once per frame for the next column, or nil when nothing new has
    /// arrived. Returning nil is what keeps the spectrogram from scrolling
    /// duplicated data at the display's refresh rate rather than the analysis
    /// rate.
    var columnProvider: ((Int) -> ArraySlice<Float>?)?
    /// The level range the ramp spans.
    var levelRange: () -> (min: Float, max: Float) = { (-120, 0) }

    init?(view: MTKView) {
        guard let device = view.device ?? MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue()
        else { return nil }

        self.device = device
        self.queue = queue

        let library: MTLLibrary
        do {
            library = try device.makeLibrary(source: Self.shaderSource, options: nil)
        } catch {
            NSLog("analyzer: spectrogram shader compilation failed: \(error)")
            return nil
        }

        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "spectrogram_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "spectrogram_fragment")
        descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
        guard let pipeline = try? device.makeRenderPipelineState(descriptor: descriptor) else {
            return nil
        }
        self.pipeline = pipeline

        // Frequency across, time down: a column of the display is a row of the
        // texture, so each update is one contiguous write.
        let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .r32Float,
            width: Self.rows,
            height: Self.history,
            mipmapped: false
        )
        textureDescriptor.usage = [.shaderRead]
        textureDescriptor.storageMode = .managed
        guard let texture = device.makeTexture(descriptor: textureDescriptor) else { return nil }
        self.texture = texture

        super.init()
        clear()
    }

    /// Fill the ring with silence so a fresh spectrogram starts dark rather than
    /// showing whatever the allocation contained.
    func clear() {
        let floor = [Float](repeating: -200, count: Self.rows)
        floor.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress else { return }
            for row in 0..<Self.history {
                texture.replace(
                    region: MTLRegionMake2D(0, row, Self.rows, 1),
                    mipmapLevel: 0,
                    withBytes: base,
                    bytesPerRow: Self.rows * MemoryLayout<Float>.stride
                )
            }
        }
        writeSlot = 0
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // Nothing to do. The ring is a fixed size precisely so that resizing
        // the window does not discard the history.
    }

    func draw(in view: MTKView) {
        if let column = columnProvider?(Self.rows), !column.isEmpty {
            append(column)
        }

        guard let descriptor = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commands = queue.makeCommandBuffer(),
              let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)
        else { return }

        let range = levelRange()
        var uniforms = Uniforms(
            offset: Float(writeSlot) / Float(Self.history),
            scale: Float(Self.history - 1) / Float(Self.history),
            minDb: range.min,
            maxDb: range.max
        )

        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        commands.present(drawable)
        commands.commit()
    }

    /// Write one column into the next ring slot.
    private func append(_ column: ArraySlice<Float>) {
        var padded = [Float](repeating: -200, count: Self.rows)
        for (index, value) in column.enumerated() where index < Self.rows {
            // An empty reduction column comes back as negative infinity, which
            // would sample as a NaN once scaled. The floor is a level, so it
            // colours as silence.
            padded[index] = value.isFinite ? value : -200
        }

        padded.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress else { return }
            texture.replace(
                region: MTLRegionMake2D(0, writeSlot, Self.rows, 1),
                mipmapLevel: 0,
                withBytes: base,
                bytesPerRow: Self.rows * MemoryLayout<Float>.stride
            )
        }

        writeSlot = (writeSlot + 1) % Self.history
    }
}
