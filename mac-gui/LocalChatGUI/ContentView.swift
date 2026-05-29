import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject var model: ChatModel

    var body: some View {
        NavigationSplitView {
            List(selection: Binding(
                get: { model.selectedPeer },
                // Defer off the current view-update pass so we don't mutate
                // @Published state mid-update (SwiftUI warns + UB otherwise).
                set: { newValue in
                    DispatchQueue.main.async { if let p = newValue { model.select(p) } }
                }
            )) {
                Section("Connected") {
                    if model.peers.isEmpty {
                        Text("No peers yet…").foregroundStyle(.secondary)
                    }
                    ForEach(model.peers, id: \.self) { peer in
                        Label(peer, systemImage: "person.fill").tag(peer)
                    }
                }
            }
            .navigationTitle("Peers")
            .navigationSplitViewColumnWidth(min: 180, ideal: 220)
        } detail: {
            if let peer = model.selectedPeer {
                ChatView(peer: peer)
            } else {
                ContentUnavailableViewCompat(
                    title: "Select a peer",
                    systemImage: "bubble.left.and.bubble.right",
                    description: model.statusLine)
            }
        }
        .safeAreaInset(edge: .bottom) {
            Text(model.statusLine)
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 12)
                .padding(.vertical, 4)
        }
        // One prompt at a time; re-presents if more files are queued.
        .alert("Incoming file",
               isPresented: Binding(get: { !model.pendingFiles.isEmpty },
                                    set: { _ in }),
               presenting: model.pendingFiles.first) { file in
            Button("Keep") { model.keepFile(file) }
            Button("Discard", role: .destructive) { model.discardFile(file) }
        } message: { file in
            Text("\(file.sender) sent “\(file.fileName)”. Keep it or discard?")
        }
    }
}

struct ChatView: View {
    @EnvironmentObject var model: ChatModel
    let peer: String

    @State private var draft = ""
    @State private var showImporter = false

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 6) {
                        ForEach(Array(model.messages.enumerated()), id: \.offset) { idx, msg in
                            MessageRow(message: msg, me: model.hostname).id(idx)
                        }
                    }
                    .padding(12)
                }
                .onChange(of: model.messages.count) { _ in
                    if let last = model.messages.indices.last {
                        withAnimation { proxy.scrollTo(last, anchor: .bottom) }
                    }
                }
            }

            Divider()

            HStack(spacing: 8) {
                Button {
                    showImporter = true
                } label: {
                    Image(systemName: "paperclip")
                }
                .help("Send a file")

                TextField("Message \(peer)…", text: $draft)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(sendDraft)

                Button("Send", action: sendDraft)
                    .disabled(draft.trimmingCharacters(in: .whitespaces).isEmpty)
                    .keyboardShortcut(.return, modifiers: [])
            }
            .padding(10)
        }
        .navigationTitle(peer)
        .fileImporter(isPresented: $showImporter,
                      allowedContentTypes: [.item],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first {
                model.sendFile(url)
            }
        }
    }

    private func sendDraft() {
        model.send(draft)
        draft = ""
    }
}

struct MessageRow: View {
    let message: BVMessageItem
    let me: String

    private var isMine: Bool { message.sender == me }

    var body: some View {
        HStack {
            if isMine { Spacer(minLength: 40) }
            VStack(alignment: isMine ? .trailing : .leading, spacing: 2) {
                Text(message.sender).font(.caption2).foregroundStyle(.secondary)
                Text(message.text)
                    .padding(.horizontal, 10).padding(.vertical, 6)
                    .background(isMine ? Color.accentColor.opacity(0.85) : Color.gray.opacity(0.2))
                    .foregroundStyle(isMine ? Color.white : Color.primary)
                    .clipShape(RoundedRectangle(cornerRadius: 12))
            }
            if !isMine { Spacer(minLength: 40) }
        }
        .frame(maxWidth: .infinity, alignment: isMine ? .trailing : .leading)
    }
}

/// Tiny shim so this compiles on macOS versions without ContentUnavailableView.
struct ContentUnavailableViewCompat: View {
    let title: String
    let systemImage: String
    let description: String
    var body: some View {
        VStack(spacing: 10) {
            Image(systemName: systemImage).font(.system(size: 40)).foregroundStyle(.secondary)
            Text(title).font(.title3)
            Text(description).font(.caption).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
