import SwiftUI
import UniformTypeIdentifiers

/// macOS Settings window (⌘,). Currently just the received-files location;
/// it's a real settings surface to grow (auto-accept, chunk size, …) later.
struct SettingsView: View {
    @EnvironmentObject var model: ChatModel
    @AppStorage("saveDirectory") private var saveDirectory: String = ""
    @State private var showPicker = false

    private var displayPath: String {
        saveDirectory.isEmpty ? ChatModel.defaultDirectory : saveDirectory
    }

    var body: some View {
        Form {
            Section("Received files") {
                LabeledContent("Save to") {
                    HStack(spacing: 8) {
                        Text(displayPath)
                            .truncationMode(.middle)
                            .lineLimit(1)
                            .foregroundStyle(.secondary)
                        Button("Choose…") { showPicker = true }
                        if !saveDirectory.isEmpty {
                            Button("Reset") {
                                saveDirectory = ""
                                model.applySaveDirectory(ChatModel.defaultDirectory)
                            }
                        }
                    }
                }
                Text("Files are saved into a subfolder named after the sender, e.g. “\(displayPath)/mmmMac/…”.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .frame(width: 480, height: 170)
        .fileImporter(isPresented: $showPicker,
                      allowedContentTypes: [.folder],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first {
                saveDirectory = url.path
                model.applySaveDirectory(url.path)
            }
        }
    }
}
