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
        List(selection: $model.section) {
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

    private func row(_ section: AppSection) -> some View {
        Label(section.title, systemImage: section.symbol)
            .tag(section)
            .badge(badge(for: section))
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
        case .rta, .transfer:
            return nil
        }
    }
}
