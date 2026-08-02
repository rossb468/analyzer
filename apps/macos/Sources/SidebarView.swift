import SwiftUI
import AnalyzerFFI

/// The tool list.
///
/// Two groups, because the entries do two different things. Picking an analysis
/// section changes what the plot draws; picking an editor changes only what the
/// inspector edits, and leaves the curve where it was. Mixing them into one flat
/// list would hide that difference behind identical-looking rows.
struct SidebarView: View {
    @ObservedObject var model: AnalyzerModel

    var body: some View {
        List(selection: selection) {
            Section("Analysis") {
                ForEach(AppSection.analysis) { row($0) }
            }
            Section("Tools") {
                ForEach(AppSection.editors) { row($0) }
            }
        }
        .listStyle(.sidebar)
        .navigationSplitViewColumnWidth(min: 150, ideal: 170, max: 240)
    }

    /// An explicitly optional binding, which is what makes the rows clickable.
    ///
    /// `List` takes its single selection as `Binding<SelectionValue?>`. Handing
    /// it the model's non-optional `section` compiles - the binding is promoted
    /// - but promotes `SelectionValue` to `AppSection?` along with it, and every
    /// row is tagged with a plain `AppSection`. The tags then match nothing and
    /// no row is selectable, silently.
    ///
    /// Writing the optional out here pins `SelectionValue` to `AppSection`.
    /// A nil write is ignored rather than stored: clicking the empty space below
    /// the list should not leave the app with no tool selected.
    private var selection: Binding<AppSection?> {
        Binding(
            get: { model.section },
            set: { newValue in
                if let newValue {
                    model.section = newValue
                }
            }
        )
    }

    private func row(_ section: AppSection) -> some View {
        Label(section.title, systemImage: section.symbol)
            .badge(badge(for: section))
            .tag(section)
    }

    /// A short status for sections doing something worth noticing from
    /// elsewhere: an active equaliser is the one that changes what is heard.
    private func badge(for section: AppSection) -> Text? {
        switch section {
        case .equaliser:
            guard model.eqMode != AnalyzerEqMode_Off, let info = model.eqInfo, info.active else {
                return nil
            }
            return Text("\(info.bandCount)")
        case .traces:
            return model.traces.isEmpty ? nil : Text("\(model.traces.count)")
        case .rta, .transfer, .spectrogram, .measure:
            return nil
        }
    }
}
