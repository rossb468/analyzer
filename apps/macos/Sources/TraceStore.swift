import Foundation
import AnalyzerFFI

/// Captured curves, held for as long as the app runs.
///
/// Deliberately not part of a session. Transform size, window and averaging all
/// restart one, and the whole point of a captured trace is to compare it against
/// something measured after a change — including a change that restarts the
/// session. A store inside the session would lose the "before" curve exactly
/// when it mattered.
///
/// The core stores each trace at analysis resolution and re-reduces it onto
/// whichever axis is current, so a trace captured at 4096 points draws correctly
/// against a session running at 65536.
final class TraceStore {
    private(set) var handle: OpaquePointer?

    /// One scratch buffer per trace, so several can be drawn in one frame
    /// without overwriting each other mid-draw. Reused, so a redraw at 120 Hz
    /// does not allocate.
    private var scratch: [Int: [Float]] = [:]

    init() {
        handle = analyzer_trace_store_create()
    }

    deinit {
        guard let handle else { return }
        analyzer_trace_store_destroy(handle)
    }

    var count: Int {
        guard let handle else { return 0 }
        return Int(analyzer_trace_store_count(handle))
    }

    func traces() -> [CapturedTrace] {
        guard let handle else { return [] }
        var out: [CapturedTrace] = []
        for index in 0..<count {
            var raw = AnalyzerTraceInfo()
            guard analyzer_trace_store_info(handle, UInt(index), &raw) else { continue }
            let name = withUnsafeBytes(of: raw.name) { bytes in
                String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
            }
            out.append(
                CapturedTrace(
                    index: index,
                    name: name,
                    visible: raw.visible,
                    colour: raw.colour,
                    points: Int(raw.points),
                    sampleRate: raw.sample_rate,
                    binSpacingHz: raw.bin_spacing_hz
                )
            )
        }
        return out
    }

    @discardableResult
    func setVisible(_ index: Int, _ visible: Bool) -> Bool {
        guard let handle else { return false }
        return analyzer_trace_store_set_visible(handle, UInt(index), visible)
    }

    @discardableResult
    func remove(_ index: Int) -> Bool {
        guard let handle else { return false }
        // Indices shift when one is removed, so every cached buffer is stale.
        scratch.removeAll()
        return analyzer_trace_store_remove(handle, UInt(index))
    }

    func removeAll() {
        guard let handle else { return }
        scratch.removeAll()
        analyzer_trace_store_clear(handle)
    }

    /// Run `body` against a reused buffer of at least `columns` floats.
    func withScratch(
        _ index: Int,
        columns: Int,
        _ body: (UnsafeMutablePointer<Float>) -> Int
    ) -> ArraySlice<Float> {
        var storage = scratch[index] ?? []
        if storage.count < columns {
            storage = [Float](repeating: 0, count: columns)
        }
        let written = storage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return body(base)
        }
        scratch[index] = storage
        return storage[0..<written]
    }
}
