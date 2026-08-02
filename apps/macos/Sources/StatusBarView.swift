import SwiftUI
import AnalyzerFFI

/// The bottom strip: what is being captured and how much to trust it.
struct StatusBarView: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
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

            Text(model.mode == AnalyzerMode_Transfer
                 ? "magnitude dB · phase ±180° · coherence 0-1"
                 : "0 dBFS = full scale sine")
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
    }
}
