import Metal
import MetalKit
import simd

/// Metal renderer for the live spectrum.
///
/// The CPU does almost nothing per frame. It copies one value per pixel column
/// into a shared buffer and issues one draw per curve; the vertex shader
/// derives x from the vertex id and y from the value, so no vertex geometry is
/// ever built on the CPU. That is the whole reason the core hands over reduced
/// values rather than points.
///
/// Text is not drawn here. Axis labels are native SwiftUI overlaid on top, which
/// is both sharper and far less code than a Metal glyph atlas.
final class SpectrumRenderer: NSObject, MTKViewDelegate {

    /// Shaders compiled at runtime rather than built into a metallib, so the
    /// build stays a plain script with no Xcode project to maintain. Costs a few
    /// milliseconds once at launch.
    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct Uniforms {
        float  count;      // trace points
        float  minValue;   // bottom of the axis this curve maps through
        float  maxValue;
        float  pad;
        float4 colour;
    };

    // One vertex per pixel column. x comes from the vertex id, y from the
    // value, so the CPU uploads values and nothing else. The range is per draw,
    // which is what lets decibels, degrees and coherence share one pipeline.
    vertex float4 trace_vertex(uint vid [[vertex_id]],
                               const device float *values [[buffer(0)]],
                               constant Uniforms &u [[buffer(1)]]) {
        float x = (u.count > 1.0) ? (float(vid) / (u.count - 1.0)) : 0.0;
        float span = max(u.maxValue - u.minValue, 1e-6);
        float t = saturate((values[vid] - u.minValue) / span);
        return float4(x * 2.0 - 1.0, t * 2.0 - 1.0, 0.0, 1.0);
    }

    fragment float4 trace_fragment(constant Uniforms &u [[buffer(1)]]) {
        return u.colour;
    }

    // Gridlines arrive as explicit clip-space endpoints.
    vertex float4 grid_vertex(uint vid [[vertex_id]],
                              const device float2 *points [[buffer(0)]]) {
        return float4(points[vid], 0.0, 1.0);
    }

    fragment float4 grid_fragment(constant float4 &colour [[buffer(1)]]) {
        return colour;
    }
    """

    private struct Uniforms {
        var count: Float = 0
        var minValue: Float = -120
        var maxValue: Float = 0
        var pad: Float = 0
        var colour: SIMD4<Float> = .init(0.35, 0.85, 0.45, 1)
    }

    /// One curve to draw.
    ///
    /// The shader maps a value through `range` onto the vertical axis, so a
    /// layer is not restricted to decibels: phase spans -180..180 and coherence
    /// spans 0..1 through exactly the same pipeline. Layers are drawn in list
    /// order, so whatever the user is watching move belongs last.
    /// The colour is resolved per frame rather than fixed at construction. A
    /// captured trace keeps the colour it was given even after an earlier one
    /// is deleted, which means a layer's colour is not known when the layer
    /// list is built.
    struct Layer {
        var provider: (Int) -> ArraySlice<Float>
        var range: (min: Float, max: Float)
        var colour: () -> SIMD4<Float>
    }

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let tracePipeline: MTLRenderPipelineState
    private let gridPipeline: MTLRenderPipelineState

    /// Shared-storage buffers, written by the CPU and read by the GPU without a
    /// blit. One per layer, grown only when the drawable does. Never shared
    /// between layers: the GPU reads them after the draw is encoded, so one
    /// buffer refilled mid-frame would tear.
    private var layerBuffers: [MTLBuffer?] = []
    private var gridBuffer: MTLBuffer?
    private var gridVertexCount = 0

    /// Curves to draw, back to front. Set by the view.
    var layers: [Layer] = []
    var gridProvider: (() -> [SIMD2<Float>])?
    var onResize: ((CGSize) -> Void)?

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
            NSLog("analyzer: shader compilation failed: \(error)")
            return nil
        }

        func pipeline(vertex: String, fragment: String) -> MTLRenderPipelineState? {
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = library.makeFunction(name: vertex)
            descriptor.fragmentFunction = library.makeFunction(name: fragment)
            descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
            return try? device.makeRenderPipelineState(descriptor: descriptor)
        }

        guard let trace = pipeline(vertex: "trace_vertex", fragment: "trace_fragment"),
              let grid = pipeline(vertex: "grid_vertex", fragment: "grid_fragment")
        else { return nil }

        tracePipeline = trace
        gridPipeline = grid
        super.init()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        onResize?(size)
    }

    func draw(in view: MTKView) {
        guard let descriptor = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commands = queue.makeCommandBuffer(),
              let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)
        else { return }

        let columns = max(Int(view.drawableSize.width), 1)

        if let points = gridProvider?(), !points.isEmpty {
            upload(points, into: &gridBuffer)
            gridVertexCount = points.count
            var colour = SIMD4<Float>(1, 1, 1, 0.12)
            encoder.setRenderPipelineState(gridPipeline)
            encoder.setVertexBuffer(gridBuffer, offset: 0, index: 0)
            encoder.setFragmentBytes(&colour, length: MemoryLayout<SIMD4<Float>>.size, index: 1)
            encoder.drawPrimitives(type: .line, vertexStart: 0, vertexCount: gridVertexCount)
        }

        if layerBuffers.count < layers.count {
            layerBuffers.append(
                contentsOf: [MTLBuffer?](repeating: nil, count: layers.count - layerBuffers.count)
            )
        }

        encoder.setRenderPipelineState(tracePipeline)
        for (index, layer) in layers.enumerated() {
            let values = layer.provider(columns)
            guard !values.isEmpty else { continue }

            upload(Array(values), into: &layerBuffers[index])
            var uniforms = Uniforms()
            uniforms.count = Float(values.count)
            uniforms.minValue = layer.range.min
            uniforms.maxValue = layer.range.max
            uniforms.colour = layer.colour()

            encoder.setVertexBuffer(layerBuffers[index], offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: values.count)
        }

        encoder.endEncoding()
        commands.present(drawable)
        commands.commit()
    }

    /// Copy into a shared buffer, reallocating only when it needs to grow.
    private func upload<T>(_ values: [T], into buffer: inout MTLBuffer?) {
        let bytes = MemoryLayout<T>.stride * values.count
        if buffer == nil || buffer!.length < bytes {
            buffer = device.makeBuffer(length: max(bytes, 4096), options: .storageModeShared)
        }
        guard let target = buffer else { return }
        values.withUnsafeBytes { source in
            guard let base = source.baseAddress else { return }
            target.contents().copyMemory(from: base, byteCount: bytes)
        }
    }
}
