import Foundation
import Combine

/// Observable wrapper around the Objective-C++ BVChatBridge. The bridge delivers
/// all delegate callbacks on the main thread, so it's safe to mutate @Published
/// state directly from them.
final class ChatModel: NSObject, ObservableObject, BVChatBridgeDelegate {

    private let bridge = BVChatBridge()

    @Published var peers: [String] = []
    @Published var selectedPeer: String?
    @Published var messages: [BVMessageItem] = []
    @Published var hostname: String = ""
    @Published var running: Bool = false
    @Published var statusLine: String = "Starting…"
    /// Files that have arrived and are awaiting a Keep/Discard decision.
    @Published var pendingFiles: [BVReceivedFileItem] = []

    override init() {
        super.init()
        bridge.delegate = self
    }

    func start() {
        guard !running else { return }
        running = bridge.start()
        hostname = bridge.thisHostname()
        statusLine = running ? "This machine: \(hostname)" : "Failed to start (mDNS registration?)"
        refreshPeers()
    }

    func stop() {
        bridge.stop()
        running = false
    }

    func select(_ peer: String) {
        selectedPeer = peer
        refreshMessages()
    }

    func send(_ text: String) {
        guard let peer = selectedPeer else { return }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        _ = bridge.sendText(trimmed, to: peer)
        refreshMessages()
    }

    func sendFile(_ url: URL) {
        guard let peer = selectedPeer else { return }
        // For sandboxed builds, wrap in start/stopAccessingSecurityScopedResource.
        _ = bridge.sendFile(url.path, to: peer)
    }

    private func refreshPeers() {
        peers = bridge.connectedPeers()
        // Keep the selection valid as peers come and go.
        if let sel = selectedPeer, !peers.contains(sel) {
            selectedPeer = nil
            messages = []
        }
    }

    private func refreshMessages() {
        guard let peer = selectedPeer else { messages = []; return }
        messages = bridge.messages(with: peer)
    }

    // Keep the file (already saved to disk) — just dismiss the prompt.
    func keepFile(_ item: BVReceivedFileItem) {
        pendingFiles.removeAll { $0 === item }
    }

    // Discard: delete the received file from disk, then dismiss.
    func discardFile(_ item: BVReceivedFileItem) {
        bridge.discardFile(atPath: item.path)
        pendingFiles.removeAll { $0 === item }
    }

    // MARK: - BVChatBridgeDelegate (main thread)
    func chatBridgePeersChanged()    { refreshPeers() }
    func chatBridgeMessagesChanged() { refreshMessages() }
    func chatBridgeFileProgress()    { /* hook for a progress indicator later */ }
    func chatBridgeFileReceived()    { pendingFiles.append(contentsOf: bridge.takeReceivedFiles()) }
}
