import SwiftUI
import AnalyzerFFI

/// The window: tool list, plot, per-section inspector, status strip.
///
/// The plot and the editor under it share the detail column through a
/// `VSplitView`, so an equaliser being edited can be given as much or as little
/// room as the curve above it. Only the sections that have a wide editor get
/// one; the rest give the whole column to the plot.
struct ContentView: View {
    @StateObject private var model: AnalyzerModel

    init(settings: SettingsStore) {
        _model = StateObject(wrappedValue: AnalyzerModel(settings: settings))
    }

    var body: some View {
        NavigationSplitView {
            SidebarView(model: model)
        } detail: {
            VStack(spacing: 0) {
                if model.section == .equaliser {
                    VSplitView {
                        PlotView(model: model)
                            .frame(minHeight: 200)
                        EqualiserEditor(model: model)
                            .frame(minHeight: 130, idealHeight: 160)
                    }
                } else if model.section == .spectrogram {
                    SpectrogramView(model: model)
                } else {
                    PlotView(model: model)
                }
                Divider()
                StatusBarView(model: model)
            }
            .frame(minWidth: 520)
            .inspector(isPresented: $model.showInspector) {
                InspectorView(model: model)
            }
            .toolbar { toolbar }
        }
        .navigationTitle(model.section.title)
        .frame(minHeight: 460)
        .onAppear {
            model.refreshDevices()
            if model.settings.startOnLaunch {
                model.start()
            }
        }
        .onDisappear { model.stop() }
    }

    // ---------------------------------------------------------- toolbar --

    /// Global controls only: the device, the stimulus, transport, export.
    /// Anything belonging to one section is in the inspector instead, which is
    /// what stops this row growing back into the wall of pickers it replaced.
    @ToolbarContentBuilder
    private var toolbar: some ToolbarContent {
        ToolbarItem(placement: .navigation) {
            Picker("Input", selection: Binding(
                get: { model.selectedDeviceUID ?? "" },
                set: { model.selectedDeviceUID = $0.isEmpty ? nil : $0 }
            )) {
                ForEach(model.devices) { device in
                    Text(device.name).tag(device.uid)
                }
            }
            .labelsHidden()
            .frame(minWidth: 160)
            .help("Input device. Refreshed when the window opens.")
        }

        ToolbarItem(placement: .primaryAction) {
            Button {
                model.isRunning ? model.stop() : model.start()
            } label: {
                Label(
                    model.isRunning ? "Stop" : "Start",
                    systemImage: model.isRunning ? "stop.fill" : "play.fill"
                )
            }
            .help(model.isRunning ? "Stop capturing." : "Start capturing.")
        }

        ToolbarItem(placement: .primaryAction) {
            generatorMenu
        }

        ToolbarItem(placement: .primaryAction) {
            Menu {
                Button("Measurement…") { model.save(asText: false) }
                Button("Text (REW)…") { model.save(asText: true) }
            } label: {
                Label("Save", systemImage: "square.and.arrow.down")
            }
            .disabled(!model.isRunning)
        }

        ToolbarItem(placement: .primaryAction) {
            Button {
                model.showInspector.toggle()
            } label: {
                Label("Inspector", systemImage: "sidebar.trailing")
            }
            .help("Show or hide the inspector.")
        }
    }

    /// Stimulus, folded into one menu.
    ///
    /// Level and frequency are live and belong beside the signal that uses
    /// them, but they are three controls that were previously always on screen
    /// for the sake of a setting most sessions touch once.
    private var generatorMenu: some View {
        Menu {
            Picker("Signal", selection: $model.signal) {
                Text("Off").tag(AnalyzerSignal_Silence)
                Text("Pink noise").tag(AnalyzerSignal_PinkNoise)
                Text("White noise").tag(AnalyzerSignal_WhiteNoise)
                Text("Sine").tag(AnalyzerSignal_Sine)
            }
            .pickerStyle(.inline)
            .disabled(!model.canPlay)

            if model.signal != AnalyzerSignal_Silence {
                Divider()
                // Labelled in dBFS and capped below full scale. A generator
                // that defaults to loud is a generator that damages something.
                LabeledContent("Level") {
                    Text(String(format: "%.0f dBFS", model.signalLevelDb))
                }
                Slider(value: $model.signalLevelDb, in: -60...0)

                if model.signal == AnalyzerSignal_Sine {
                    LabeledContent("Frequency") {
                        Text(formatFrequency(model.signalHz))
                    }
                    Slider(value: $model.signalHz, in: 20...20_000)
                }
            }
        } label: {
            Label(signalLabel, systemImage: "waveform.circle")
        }
        .disabled(!model.canPlay)
        .help(model.canPlay
              ? "Stimulus played out of the selected device."
              : """
                This device has no output. Create an aggregate device in Audio \
                MIDI Setup to play and capture together.
                """)
    }

    private var signalLabel: String {
        switch model.signal {
        case AnalyzerSignal_PinkNoise: "Pink"
        case AnalyzerSignal_WhiteNoise: "White"
        case AnalyzerSignal_Sine: formatFrequency(model.signalHz)
        default: "Signal"
        }
    }
}
