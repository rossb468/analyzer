import SwiftUI
import AppKit

/// Shown when the selected device cannot play the stimulus.
///
/// This is the single most confusing thing about the app on a laptop, and the
/// old wording — "This device has no output" — explained none of it. A user who
/// has just chosen a microphone knows perfectly well it has no output; what they
/// do not know is why that stops the app, given the Mac plainly has speakers.
///
/// So the message leads with the constraint rather than the symptom: one device
/// does both directions, because macOS drives one device per audio callback.
struct OutputHint: View {
    @ObservedObject var model: AnalyzerModel
    /// What is unavailable without an output, in a few words.
    let needs: String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label {
                Text("\(needs) needs a device that can play as well as record.")
                    .fontWeight(.medium)
            } icon: {
                Image(systemName: "speaker.slash")
                    .foregroundStyle(.orange)
            }

            Text(explanation)
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Button("Open Audio MIDI Setup") { Self.openAudioMidiSetup() }
                .controlSize(.small)
        }
        .padding(.vertical, 2)
    }

    private var explanation: String {
        let name = model.deviceName.isEmpty
            ? model.devices.first { $0.uid == model.selectedDeviceUID }?.name ?? "The selected input"
            : model.deviceName

        return """
            macOS drives one device per audio callback, so the stimulus and the \
            recording have to go through the same one. “\(name)” can only \
            record. Combine it with your speakers into an Aggregate Device in \
            Audio MIDI Setup, then choose that device in the toolbar.
            """
    }

    /// Opens the macOS utility that creates aggregate devices.
    ///
    /// Sending the user to find it themselves is most of the friction: it lives
    /// in Utilities and its name does not suggest it is where this is fixed.
    static func openAudioMidiSetup() {
        let url = URL(fileURLWithPath: "/System/Applications/Utilities/Audio MIDI Setup.app")
        NSWorkspace.shared.open(url)
    }
}
