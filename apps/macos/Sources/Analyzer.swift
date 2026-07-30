import Foundation
import AnalyzerFFI

/// Swift face of the Rust core.
///
/// Everything below is a thin translation of the C ABI into Swift types. There
/// is deliberately no analysis logic here, and no arithmetic on frequencies or
/// levels: axis mapping happens in Rust and is queried, never reimplemented.
/// Duplicating that maths in Swift is how the drawn curve and the cursor readout
/// quietly stop agreeing with each other.

/// An audio input device.
struct AudioDevice: Identifiable, Hashable {
    let uid: String
    let name: String
    let inputChannels: UInt32
    let sampleRate: Double
    let isDefaultInput: Bool

    var id: String { uid }
}

/// Metadata about the most recent analysis frame.
struct FrameInfo {
    let sequence: UInt64
    let overruns: UInt64
    let framesAveraged: UInt32
    let averageFrames: UInt32
    let sampleRate: Float
    let binSpacingHz: Float
}

/// A harmonic distortion reading.
struct DistortionReading {
    let fundamentalHz: Float
    let thdPercent: Float
    let thdDb: Float
    let thdNPercent: Float
    let noiseFloorDb: Float
    let ordersAboveNyquist: UInt32
    let harmonics: [(order: Int, hz: Float, percent: Float, relativeDb: Float)]

    /// Honest about how many orders the figure covers, since a high fundamental
    /// pushes upper harmonics past Nyquist where they cannot be measured.
    var summary: String {
        let orders = ordersAboveNyquist > 0 && !harmonics.isEmpty
            ? " (to H\(harmonics.count + 1))"
            : ""
        return String(format: "THD %.3f%%%@ @ %@", thdPercent, orders, formatFrequency(fundamentalHz))
    }
}

/// A gridline.
struct GridTick {
    let value: Float
    let position: Float
    let major: Bool
}

enum AnalyzerError: LocalizedError {
    case failed(String)

    var errorDescription: String? {
        switch self {
        case .failed(let message): message
        }
    }
}

/// Reads the device list once and releases it immediately, copying the strings
/// out. The C list owns the storage the pointers refer to, so nothing may escape
/// this function.
func availableInputDevices() -> [AudioDevice] {
    guard let list = analyzer_device_list_create() else { return [] }
    defer { analyzer_device_list_destroy(list) }

    var devices: [AudioDevice] = []
    for index in 0..<analyzer_device_list_count(list) {
        var raw = AnalyzerDevice()
        guard analyzer_device_list_get(list, index, &raw),
              let uid = raw.uid, let name = raw.name,
              raw.input_channels > 0
        else { continue }

        devices.append(
            AudioDevice(
                uid: String(cString: uid),
                name: String(cString: name),
                inputChannels: raw.input_channels,
                sampleRate: raw.sample_rate,
                isDefaultInput: raw.is_default_input
            )
        )
    }
    return devices
}

/// A running capture and analysis session.
///
/// Owns the Rust handle and is the only thing allowed to free it.
final class AnalyzerSessionHandle {
    private var handle: OpaquePointer?

    /// Scratch the trace is copied into. Reused so a redraw at 120 Hz does not
    /// allocate; grown only when the drawable gets wider.
    private var traceStorage: [Float] = []
    private var averageStorage: [Float] = []

    /// Start capturing.
    ///
    /// - Parameter deviceUID: `nil` selects the system default input.
    init(deviceUID: String?, fftSize: UInt32, window: AnalyzerWindow, averaging: AnalyzerAveraging) throws {
        var config = analyzer_session_config_default()
        config.fft_size = fftSize
        config.window = window
        config.averaging = averaging

        var status = AnalyzerStatus()
        let started: OpaquePointer?

        if let uid = deviceUID {
            started = uid.withCString { pointer in
                config.device_uid = pointer
                return analyzer_session_start(&config, &status)
            }
        } else {
            config.device_uid = nil
            started = analyzer_session_start(&config, &status)
        }

        guard let started else {
            throw AnalyzerError.failed(Self.message(from: status))
        }
        handle = started
    }

    deinit { stop() }

    func stop() {
        guard let handle else { return }
        analyzer_session_stop(handle)
        self.handle = nil
    }

    /// The device actually being captured.
    var deviceName: String {
        guard let handle else { return "" }
        var buffer = [CChar](repeating: 0, count: 256)
        let written = analyzer_session_device_name(handle, &buffer, UInt(buffer.count))
        guard written > 0 else { return "" }
        return String(cString: buffer)
    }

    /// Tell the core the drawable size and view range. Call on every resize.
    func setPlot(
        width: Float, height: Float,
        minHz: Float, maxHz: Float,
        minDb: Float, maxDb: Float,
        reduction: AnalyzerReduction = AnalyzerReduction_Max
    ) {
        guard let handle else { return }
        _ = analyzer_session_set_plot(handle, width, height, minHz, maxHz, minDb, maxDb, reduction)
    }

    /// Whether anything new has arrived. Skipping the redraw when this is false
    /// is what keeps the app near zero CPU in a silent room.
    var hasNewFrame: Bool {
        guard let handle else { return false }
        return analyzer_session_has_new_frame(handle)
    }

    /// Copy the reduced trace, one level per pixel column.
    ///
    /// The returned slice is owned by this object and valid until the next call.
    func copyTrace(columns: Int) -> ArraySlice<Float> {
        guard let handle, columns > 0 else { return [][...] }
        if traceStorage.count < columns {
            traceStorage = [Float](repeating: 0, count: columns)
        }
        let written = traceStorage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_session_copy_trace(handle, base, UInt(columns)))
        }
        return traceStorage[0..<written]
    }

    /// Copy the long-term average trace.
    ///
    /// Separate storage from the live trace so a renderer can hold both at once
    /// without one overwriting the other mid-frame.
    func copyAverage(columns: Int) -> ArraySlice<Float> {
        guard let handle, columns > 0 else { return [][...] }
        if averageStorage.count < columns {
            averageStorage = [Float](repeating: 0, count: columns)
        }
        let written = averageStorage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_session_copy_average(handle, base, UInt(columns)))
        }
        return averageStorage[0..<written]
    }

    /// Restart the long-term average without disturbing the live trace.
    func resetAverage() {
        guard let handle else { return }
        _ = analyzer_session_reset_average(handle)
    }

    var frameInfo: FrameInfo? {
        guard let handle else { return nil }
        var raw = AnalyzerFrameInfo()
        guard analyzer_session_frame_info(handle, &raw) else { return nil }
        return FrameInfo(
            sequence: raw.sequence,
            overruns: raw.overruns,
            framesAveraged: raw.frames_averaged,
            averageFrames: raw.average_frames,
            sampleRate: raw.sample_rate,
            binSpacingHz: raw.bin_spacing_hz
        )
    }

    /// Measure distortion in the current spectrum.
    ///
    /// Returns nil when no fundamental stands clear enough of the noise floor
    /// for the figure to mean anything, so a UI can hide the readout rather than
    /// show a number derived from hiss.
    func distortion(fundamentalHz: Float = 0) -> DistortionReading? {
        guard let handle else { return nil }
        var raw = AnalyzerDistortion()
        guard analyzer_session_distortion(handle, fundamentalHz, &raw) else { return nil }

        var harmonics: [(order: Int, hz: Float, percent: Float, relativeDb: Float)] = []
        let hz = withUnsafeBytes(of: raw.harmonic_hz) { Array($0.bindMemory(to: Float.self)) }
        let percent = withUnsafeBytes(of: raw.harmonic_percent) {
            Array($0.bindMemory(to: Float.self))
        }
        let relative = withUnsafeBytes(of: raw.harmonic_relative_db) {
            Array($0.bindMemory(to: Float.self))
        }
        for index in 0..<Int(raw.harmonic_count) where index < hz.count {
            // Harmonics start at the second order; the fundamental is the first.
            harmonics.append((index + 2, hz[index], percent[index], relative[index]))
        }

        return DistortionReading(
            fundamentalHz: raw.fundamental_hz,
            thdPercent: raw.thd_percent,
            thdDb: raw.thd_db,
            thdNPercent: raw.thd_n_percent,
            noiseFloorDb: raw.noise_floor_db,
            ordersAboveNyquist: raw.orders_above_nyquist,
            harmonics: harmonics
        )
    }

    // Axis queries. These call into Rust rather than computing anything here,
    // so labels, cursor readout and the drawn curve cannot disagree.

    func x(forFrequency hz: Float) -> Float {
        guard let handle else { return .nan }
        return analyzer_freq_to_x(handle, hz)
    }

    func frequency(atX x: Float) -> Float {
        guard let handle else { return .nan }
        return analyzer_x_to_freq(handle, x)
    }

    func y(forLevel db: Float) -> Float {
        guard let handle else { return .nan }
        return analyzer_db_to_y(handle, db)
    }

    func level(atY y: Float) -> Float {
        guard let handle else { return .nan }
        return analyzer_y_to_db(handle, y)
    }

    func frequencyTicks() -> [GridTick] {
        guard let handle else { return [] }
        var raw = [AnalyzerTick](repeating: AnalyzerTick(), count: 64)
        let count = raw.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_frequency_ticks(handle, base, UInt(buffer.count)))
        }
        return raw.prefix(count).map { GridTick(value: $0.value, position: $0.position, major: $0.major) }
    }

    func levelTicks(step: Float) -> [GridTick] {
        guard let handle else { return [] }
        var raw = [AnalyzerTick](repeating: AnalyzerTick(), count: 64)
        let count = raw.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_level_ticks(handle, step, base, UInt(buffer.count)))
        }
        return raw.prefix(count).map { GridTick(value: $0.value, position: $0.position, major: $0.major) }
    }

    private static func message(from status: AnalyzerStatus) -> String {
        withUnsafeBytes(of: status.message) { raw in
            let bytes = raw.prefix { $0 != 0 }
            return String(decoding: bytes, as: UTF8.self)
        }
    }
}

/// Format a frequency the way audio people expect: 20, 500, 2k, 20k.
func formatFrequency(_ hz: Float) -> String {
    if hz >= 1000 {
        let k = hz / 1000
        return abs(k.rounded() - k) < 0.05 ? "\(Int(k.rounded()))k" : String(format: "%.1fk", k)
    }
    return hz >= 10 ? "\(Int(hz.rounded()))" : String(format: "%.1f", hz)
}
