import Metal
import MetalKit
import simd

/// Metal renderer for the live spectrum.
///
/// The CPU does almost nothing per frame. It copies a trace of one level per
/// pixel column into a shared buffer and issues two draws; the vertex shader
/// derives x from the vertex id and y from the level, so no vertex geometry is
/// ever built on the CPU. That is the whole reason the core hands over reduced
/// levels rather than points.
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
        float  minDb;
        float  maxDb;
        float  pad;
        float4 colour;
    };

    // One vertex per pixel column. x comes from the vertex id, y from the level,
    // so the CPU uploads levels and nothing else.
    vertex float4 trace_vertex(uint vid [[vertex_id]],
                               const device float *levels [[buffer(0)]],
                               constant Uniforms &u [[buffer(1)]]) {
        float x = (u.count > 1.0) ? (float(vid) / (u.count - 1.0)) : 0.0;
        float db = levels[vid];
        float t = saturate((db - u.minDb) / max(u.maxDb - u.minDb, 1e-6));
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
        var minDb: Float = -120
        var maxDb: Float = 0
        var pad: Float = 0
        var colour: SIMD4<Float> = .init(0.35, 0.85, 0.45, 1)
    }

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let tracePipeline: MTLRenderPipelineState
    private let gridPipeline: MTLRenderPipelineState

    /// Shared-storage buffers, written by the CPU and read by the GPU without a
    /// blit. Grown only when the drawable does.
    private var traceBuffer: MTLBuffer?
    private var averageBuffer: MTLBuffer?
    private var gridBuffer: MTLBuffer?
    private var gridVertexCount = 0

    private var uniforms = Uniforms()

    /// Set by the view before each draw.
    var traceProvider: ((Int) -> ArraySlice<Float>)?
    /// Long-term average, drawn under the live trace. Nil hides it.
    var averageProvider: ((Int) -> ArraySlice<Float>)?
    var gridProvider: (() -> [SIMD2<Float>])?
    var levelRange: (min: Float, max: Float) = (-120, 0)
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

        // The average is drawn first so the live trace sits on top of it - the
        // live one is what the user is watching move.
        if let levels = averageProvider?(columns), !levels.isEmpty {
            upload(Array(levels), into: &averageBuffer)
            var average = uniforms
            average.count = Float(levels.count)
            average.minDb = levelRange.min
            average.maxDb = levelRange.max
            average.colour = SIMD4<Float>(1.0, 0.65, 0.25, 0.9)

            encoder.setRenderPipelineState(tracePipeline)
            encoder.setVertexBuffer(averageBuffer, offset: 0, index: 0)
            encoder.setVertexBytes(&average, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.setFragmentBytes(&average, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: levels.count)
        }

        if let levels = traceProvider?(columns), !levels.isEmpty {
            upload(Array(levels), into: &traceBuffer)
            uniforms.count = Float(levels.count)
            uniforms.minDb = levelRange.min
            uniforms.maxDb = levelRange.max

            encoder.setRenderPipelineState(tracePipeline)
            encoder.setVertexBuffer(traceBuffer, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: levels.count)
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
