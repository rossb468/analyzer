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
    let outputChannels: UInt32
    let sampleRate: Double
    let isDefaultInput: Bool

    var id: String { uid }

    /// Whether this device can play a stimulus as well as capture.
    ///
    /// CoreAudio drives one device from one IOProc, so a transfer function
    /// against an internal reference needs both directions on the same device.
    /// A separate speaker and microphone means an aggregate device.
    var canPlay: Bool { outputChannels > 0 }
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

/// State of a running transfer function.
struct TransferInfo {
    /// Frames folded into the estimate. Zero means none is running.
    let frames: UInt32
    let delayFrames: UInt32
    let delayMs: Float
    let delayMetres: Float
    /// A delay estimate has been asked for and has not settled.
    let estimating: Bool

    /// Coherence is identically one for a single frame, so a curve built from
    /// a handful of them says nothing yet.
    var isSettled: Bool { frames >= 8 }

    var delaySummary: String {
        if estimating { return "finding delay…" }
        return String(format: "%.2f ms · %.2f m", delayMs, delayMetres)
    }
}

/// One equaliser band, as Swift sees it.
struct EqBand: Identifiable, Equatable {
    /// Position in the equaliser. Stable for as long as no band is removed,
    /// which is all SwiftUI needs to animate a list.
    let id: Int
    var kind: AnalyzerFilterKind
    var hz: Float
    var gainDb: Float
    var q: Float
    var enabled: Bool

    /// Whether gain means anything for this shape. A pass or reject filter
    /// ignores it, and a UI should say so rather than offer a dead control.
    var usesGain: Bool {
        kind == AnalyzerFilterKind_Peaking
            || kind == AnalyzerFilterKind_LowShelf
            || kind == AnalyzerFilterKind_HighShelf
    }

    var kindLabel: String {
        switch kind {
        case AnalyzerFilterKind_Peaking: "PK"
        case AnalyzerFilterKind_LowShelf: "LS"
        case AnalyzerFilterKind_HighShelf: "HS"
        case AnalyzerFilterKind_LowPass: "LP"
        case AnalyzerFilterKind_HighPass: "HP"
        case AnalyzerFilterKind_BandPass: "BP"
        case AnalyzerFilterKind_Notch: "NO"
        default: "AP"
        }
    }

    var raw: AnalyzerBand {
        AnalyzerBand(kind: kind, hz: hz, gain_db: gainDb, q: q, enabled: enabled)
    }
}

/// Headroom of the active equaliser.
struct EqInfo {
    let bandCount: Int
    /// Largest gain anywhere in the band. Bands add, so this routinely exceeds
    /// any single band's setting.
    let peakGainDb: Float
    let preampDb: Float
    let active: Bool

    /// Whether the equaliser is asking for more than unity and has not been
    /// trimmed for it. Anything above this clips before it reaches the
    /// converter, and silence is the wrong way to find that out.
    var clips: Bool { active && peakGainDb > 0.1 }
}

/// A curve captured for comparison.
///
/// The core stores it at analysis resolution and re-reduces it onto whatever
/// axis is current, so this carries only what the list needs to describe it.
struct CapturedTrace: Identifiable, Hashable {
    let index: Int
    let name: String
    var visible: Bool
    /// Palette index chosen by the core when it was captured. The core does not
    /// know what colour this is, only that two traces should not share one.
    let colour: UInt32
    let points: Int
    let sampleRate: Float
    let binSpacingHz: Float

    var id: Int { index }

    /// "16384 points · 2.93 Hz" — what the trace can actually resolve.
    var detail: String {
        String(format: "%d points · %.2f Hz", points, binSpacingHz)
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
                outputChannels: raw.output_channels,
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
    init(
        deviceUID: String?,
        fftSize: UInt32,
        window: AnalyzerWindow,
        averaging: AnalyzerAveraging,
        mode: AnalyzerMode = AnalyzerMode_Spectrum,
        reference: AnalyzerReference = AnalyzerReference_Internal,
        referenceChannel: UInt32 = 1,
        signal: AnalyzerSignal = AnalyzerSignal_Silence,
        signalLevelDb: Float = -20,
        signalHz: Float = 1000
    ) throws {
        var config = analyzer_session_config_default()
        config.fft_size = fftSize
        config.window = window
        config.averaging = averaging
        config.mode = mode
        config.reference = reference
        config.reference_channel = referenceChannel
        config.signal = signal
        config.signal_level_db = signalLevelDb
        config.signal_hz = signalHz

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

    /// Scratch for the transfer curves, one buffer each so a renderer can hold
    /// all three at once without one overwriting another mid-frame.
    private var transferStorage: [AnalyzerCurve.RawValue: [Float]] = [:]

    /// Copy one transfer function curve, one value per pixel column.
    ///
    /// Returns an empty slice in spectrum mode, so a caller can ask
    /// unconditionally and simply draw nothing.
    func copyTransfer(_ curve: AnalyzerCurve, columns: Int) -> ArraySlice<Float> {
        guard let handle, columns > 0 else { return [][...] }
        var storage = transferStorage[curve.rawValue] ?? []
        if storage.count < columns {
            storage = [Float](repeating: 0, count: columns)
        }
        let written = storage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_session_copy_transfer(handle, curve, base, UInt(columns)))
        }
        transferStorage[curve.rawValue] = storage
        return storage[0..<written]
    }

    var transferInfo: TransferInfo? {
        guard let handle else { return nil }
        var raw = AnalyzerTransferInfo()
        guard analyzer_session_transfer_info(handle, &raw) else { return nil }
        return TransferInfo(
            frames: raw.frames,
            delayFrames: raw.delay_frames,
            delayMs: raw.delay_ms,
            delayMetres: raw.delay_metres,
            estimating: raw.estimating
        )
    }

    /// Ask the core to measure the reference-to-measurement delay and remove it.
    func estimateDelay() {
        guard let handle else { return }
        _ = analyzer_session_estimate_delay(handle)
    }

    /// Set the reference delay by hand.
    func setDelay(frames: UInt32) {
        guard let handle else { return }
        _ = analyzer_session_set_delay(handle, frames)
    }

    /// Change the stimulus without restarting.
    ///
    /// Only has an effect if the session was started with an output open; a
    /// session started silent has no output stream to write into.
    func setSignal(_ signal: AnalyzerSignal, levelDb: Float, hz: Float) {
        guard let handle else { return }
        _ = analyzer_session_set_signal(handle, signal, levelDb, hz)
    }

    /// Map a phase in degrees to a pixel row.
    func y(forPhase degrees: Float) -> Float {
        guard let handle else { return .nan }
        return analyzer_phase_to_y(handle, degrees)
    }

    /// Map a coherence value to a pixel row.
    func y(forCoherence value: Float) -> Float {
        guard let handle else { return .nan }
        return analyzer_coherence_to_y(handle, value)
    }

    private var eqCurveStorage: [Float] = []
    private var correctedStorage: [Float] = []

    /// Choose which equaliser is active.
    func setEqMode(_ mode: AnalyzerEqMode) {
        guard let handle else { return }
        _ = analyzer_session_set_eq_mode(handle, mode)
    }

    /// Read every band of the active equaliser.
    func eqBands() -> [EqBand] {
        guard let handle else { return [] }
        let count = Int(analyzer_session_eq_band_count(handle))
        var bands: [EqBand] = []
        bands.reserveCapacity(count)
        for index in 0..<count {
            var raw = AnalyzerBand()
            guard analyzer_session_eq_get_band(handle, UInt(index), &raw) else { continue }
            bands.append(
                EqBand(
                    id: index,
                    kind: raw.kind,
                    hz: raw.hz,
                    gainDb: raw.gain_db,
                    q: raw.q,
                    enabled: raw.enabled
                )
            )
        }
        return bands
    }

    @discardableResult
    func setEqBand(_ index: Int, _ band: EqBand) -> Bool {
        guard let handle else { return false }
        var raw = band.raw
        return analyzer_session_eq_set_band(handle, UInt(index), &raw)
    }

    @discardableResult
    func setEqGain(_ index: Int, _ gainDb: Float) -> Bool {
        guard let handle else { return false }
        return analyzer_session_eq_set_gain(handle, UInt(index), gainDb)
    }

    /// Append a band. Returns its index, or nil when the equaliser is full.
    @discardableResult
    func addEqBand(_ band: EqBand) -> Int? {
        guard let handle else { return nil }
        var raw = band.raw
        let index = analyzer_session_eq_add_band(handle, &raw)
        return index < 0 ? nil : Int(index)
    }

    @discardableResult
    func removeEqBand(_ index: Int) -> Bool {
        guard let handle else { return false }
        return analyzer_session_eq_remove_band(handle, UInt(index))
    }

    func flattenEq() {
        guard let handle else { return }
        _ = analyzer_session_eq_flatten(handle)
    }

    /// Trim the output so the equaliser's loudest point sits at unity.
    func trimEq() {
        guard let handle else { return }
        _ = analyzer_session_eq_trim(handle)
    }

    func setEqPreamp(_ db: Float) {
        guard let handle else { return }
        _ = analyzer_session_eq_set_preamp(handle, db)
    }

    var eqInfo: EqInfo? {
        guard let handle else { return nil }
        var raw = AnalyzerEqInfo()
        guard analyzer_session_eq_info(handle, &raw) else { return nil }
        return EqInfo(
            bandCount: Int(raw.band_count),
            peakGainDb: raw.peak_gain_db,
            preampDb: raw.preamp_db,
            active: raw.active
        )
    }

    /// Copy the equaliser's own curve. Pass a band index for one band alone.
    func copyEqCurve(columns: Int, band: Int? = nil) -> ArraySlice<Float> {
        guard let handle, columns > 0 else { return [][...] }
        if eqCurveStorage.count < columns {
            eqCurveStorage = [Float](repeating: 0, count: columns)
        }
        let written = eqCurveStorage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(
                analyzer_session_copy_eq_curve(handle, band.map { Int($0) } ?? -1, base, UInt(columns))
            )
        }
        return eqCurveStorage[0..<written]
    }

    /// Copy the measured trace with the equaliser applied.
    func copyCorrected(columns: Int) -> ArraySlice<Float> {
        guard let handle, columns > 0 else { return [][...] }
        if correctedStorage.count < columns {
            correctedStorage = [Float](repeating: 0, count: columns)
        }
        let written = correctedStorage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_session_copy_corrected(handle, base, UInt(columns)))
        }
        return correctedStorage[0..<written]
    }

    private var targetStorage: [Float] = []

    /// The session's target curve.
    var target: AnalyzerTarget? {
        guard let handle else { return nil }
        var raw = analyzer_target_default()
        guard analyzer_session_target(handle, &raw) else { return nil }
        return raw
    }

    func setTarget(_ target: AnalyzerTarget) {
        guard let handle else { return }
        var raw = target
        _ = analyzer_session_set_target(handle, &raw)
    }

    /// Load a custom target from a frequency/level text file.
    func loadTarget(from url: URL) throws {
        guard let handle else { throw AnalyzerError.failed("no session running") }
        var status = AnalyzerStatus()
        let ok = url.path.withCString { path in
            analyzer_session_load_target(handle, path, &status)
        }
        guard ok else { throw AnalyzerError.failed(Self.message(from: status)) }
    }

    /// Align the target to the current measurement.
    @discardableResult
    func alignTarget() -> Bool {
        guard let handle else { return false }
        return analyzer_session_align_target(handle)
    }

    /// Copy the target curve, one level per pixel column.
    func copyTarget(columns: Int) -> ArraySlice<Float> {
        guard let handle, columns > 0 else { return [][...] }
        if targetStorage.count < columns {
            targetStorage = [Float](repeating: 0, count: columns)
        }
        let written = targetStorage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_session_copy_target(handle, base, UInt(columns)))
        }
        return targetStorage[0..<written]
    }

    private var measuredStorage: [Float] = []
    private var impulseStorage: [Float] = []

    // ---------------------------------------------------------- measurement -

    /// Play a sweep and start recording the response.
    func startMeasurement(_ config: AnalyzerMeasureConfig) throws {
        guard let handle else { throw AnalyzerError.failed("no session running") }
        var settings = config
        var status = AnalyzerStatus()
        guard analyzer_session_start_measurement(handle, &settings, &status) else {
            throw AnalyzerError.failed(Self.message(from: status))
        }
    }

    var measureProgress: AnalyzerMeasureProgress? {
        guard let handle else { return nil }
        var raw = AnalyzerMeasureProgress()
        guard analyzer_session_measure_progress(handle, &raw) else { return nil }
        return raw
    }

    func cancelMeasurement() {
        guard let handle else { return }
        analyzer_session_cancel_measurement(handle)
    }

    /// Deconvolve the recording. Throws while the sweep is still playing.
    func finishMeasurement() throws -> AnalyzerMeasureResult {
        guard let handle else { throw AnalyzerError.failed("no session running") }
        var result = AnalyzerMeasureResult()
        var status = AnalyzerStatus()
        guard analyzer_session_finish_measurement(handle, &result, &status) else {
            throw AnalyzerError.failed(Self.message(from: status))
        }
        return result
    }

    var measurementResult: AnalyzerMeasureResult? {
        guard let handle else { return nil }
        var raw = AnalyzerMeasureResult()
        guard analyzer_session_measurement_result(handle, &raw) else { return nil }
        return raw
    }

    /// Copy the measured response, reduced onto the current axis.
    func copyMeasured(columns: Int) -> ArraySlice<Float> {
        guard let handle, columns > 0 else { return [][...] }
        if measuredStorage.count < columns {
            measuredStorage = [Float](repeating: 0, count: columns)
        }
        let written = measuredStorage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_session_copy_measured(handle, base, UInt(columns)))
        }
        return measuredStorage[0..<written]
    }

    /// Copy the impulse response, decimated and normalised to its peak.
    func copyImpulse(columns: Int, seconds: Float) -> ArraySlice<Float> {
        guard let handle, columns > 0 else { return [][...] }
        if impulseStorage.count < columns {
            impulseStorage = [Float](repeating: 0, count: columns)
        }
        let written = impulseStorage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_session_copy_impulse(handle, base, UInt(columns), seconds))
        }
        return impulseStorage[0..<written]
    }

    private var spectrogramStorage: [Float] = []

    /// Copy the newest frame reduced onto `rows` frequency positions.
    ///
    /// Decibels, not colours: mapping level to colour is the renderer's job.
    func copySpectrogramColumn(rows: Int) -> ArraySlice<Float> {
        guard let handle, rows > 0 else { return [][...] }
        if spectrogramStorage.count < rows {
            spectrogramStorage = [Float](repeating: 0, count: rows)
        }
        let written = spectrogramStorage.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_session_copy_spectrogram_column(handle, base, UInt(rows)))
        }
        return spectrogramStorage[0..<written]
    }

    /// Frequency ticks along an axis of `length`, without disturbing the plot
    /// geometry the trace view declared.
    func frequencyTicks(along length: Float) -> [GridTick] {
        guard let handle, length > 0 else { return [] }
        var raw = [AnalyzerTick](repeating: AnalyzerTick(), count: 64)
        let count = raw.withUnsafeMutableBufferPointer { buffer -> Int in
            guard let base = buffer.baseAddress else { return 0 }
            return Int(analyzer_frequency_ticks_for(handle, length, base, UInt(buffer.count)))
        }
        return raw.prefix(count).map {
            GridTick(value: $0.value, position: $0.position, major: $0.major)
        }
    }

    /// Capture the live curve into `store`.
    ///
    /// The store is passed in rather than owned here because it has to outlive
    /// the session: transform size, window and averaging all restart one, and a
    /// "before" trace that vanished when you changed the setting you wanted to
    /// compare would be useless.
    @discardableResult
    func captureTrace(into store: TraceStore, named name: String?) -> Int? {
        guard let handle, let storeHandle = store.handle else { return nil }
        let index: Int = {
            guard let name, !name.isEmpty else {
                return Int(analyzer_trace_store_capture(storeHandle, handle, nil))
            }
            return name.withCString {
                Int(analyzer_trace_store_capture(storeHandle, handle, $0))
            }
        }()
        return index < 0 ? nil : index
    }

    /// Copy a captured trace, reduced onto this session's current axis.
    func copyCapturedTrace(
        _ index: Int,
        from store: TraceStore,
        columns: Int
    ) -> ArraySlice<Float> {
        guard let handle, let storeHandle = store.handle, columns > 0 else { return [][...] }
        return store.withScratch(index, columns: columns) { base in
            Int(analyzer_trace_store_copy(storeHandle, UInt(index), handle, base, UInt(columns)))
        }
    }

    /// Fit filters to the gap between the measurement and the target.
    ///
    /// Replaces the parametric equaliser's bands and selects it, so the fit is
    /// immediately drawn and heard.
    ///
    /// - Throws: [`AnalyzerError`] when there is nothing captured to correct.
    @discardableResult
    func optimise(_ config: AnalyzerOptimiserConfig) throws -> AnalyzerOptimisation {
        guard let handle else { throw AnalyzerError.failed("no session running") }
        var settings = config
        var result = AnalyzerOptimisation()
        var status = AnalyzerStatus()
        guard analyzer_session_optimise(handle, &settings, &result, &status) else {
            throw AnalyzerError.failed(Self.message(from: status))
        }
        return result
    }

    /// Write the active equaliser out in `format`.
    ///
    /// - Throws: [`AnalyzerError`] with whatever the core reported, which is
    ///   either a filesystem problem or the equaliser being off.
    func exportFilters(_ format: AnalyzerFilterFormat, to url: URL) throws {
        guard let handle else { throw AnalyzerError.failed("no session running") }
        var status = AnalyzerStatus()
        let ok = url.path.withCString { path in
            analyzer_session_export_filters(handle, format, path, &status)
        }
        guard ok else { throw AnalyzerError.failed(Self.message(from: status)) }
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

    /// Write the current spectrum to a measurement file.
    ///
    /// - Throws: [`AnalyzerError`] with whatever the core reported, which is
    ///   usually a filesystem problem worth showing verbatim.
    func save(to url: URL, name: String, splOffsetDb: Float = 0) throws {
        try write(to: url, name: name, splOffsetDb: splOffsetDb, asText: false)
    }

    /// Write the current spectrum as REW-compatible text.
    func exportText(to url: URL, name: String, splOffsetDb: Float = 0) throws {
        try write(to: url, name: name, splOffsetDb: splOffsetDb, asText: true)
    }

    private func write(to url: URL, name: String, splOffsetDb: Float, asText: Bool) throws {
        guard let handle else { throw AnalyzerError.failed("no session running") }
        var status = AnalyzerStatus()
        let ok = url.path.withCString { path in
            name.withCString { label in
                asText
                    ? analyzer_session_export_text(handle, path, label, splOffsetDb, &status)
                    : analyzer_session_save_measurement(handle, path, label, splOffsetDb, &status)
            }
        }
        guard ok else { throw AnalyzerError.failed(Self.message(from: status)) }
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
