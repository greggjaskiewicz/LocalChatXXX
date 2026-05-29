import Foundation
import Combine

/// Observable wrapper around the Objective-C++ BVChatBridge. The bridge delivers
/// all delegate callbacks on the main thread, so it's safe to mutate @Published
/// state directly from them.
final class ChatModel: NSObject, ObservableObject, BVChatBridgeDelegate {

    private let bridge = BVChatBridge()
    private var progressTimer: Timer?

    @Published var peers: [String] = []
    @Published var selectedPeer: String?
    @Published var messages: [BVMessageItem] = []
    @Published var hostname: String = ""
    @Published var running: Bool = false
    @Published var statusLine: String = "Starting…"
    /// Incoming file offers awaiting an Accept/Reject decision.
    @Published var pendingOffers: [BVFileOfferItem] = []
    /// Live file-transfer status, e.g. "Receiving x.png… 45%" / "Sent y.png".
    @Published var transferStatus: String = ""

    override init() {
        super.init()
        bridge.delegate = self
    }

    func start() {
        guard !running else { return }
        running = bridge.start()
        hostname = bridge.thisHostname()
        statusLine = running ? "This machine: \(hostname)" : "Failed to start (mDNS registration?)"
        applySaveDirectory(ChatModel.savedDirectory)
        refreshPeers()
        // Poll: sessions are added in ConnectHandler with no app event, and
        // outgoing send progress has no event stream either. Cheap, and only
        // assigns on change to avoid needless re-renders.
        progressTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            let s = self.bridge.transferStatus()
            if s != self.transferStatus { self.transferStatus = s }
            self.refreshPeers()
        }
    }

    func stop() {
        progressTimer?.invalidate()
        progressTimer = nil
        bridge.stop()
        running = false
    }

    // Where received files are saved. Defaults to ~/Downloads when unset.
    static var defaultDirectory: String {
        FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask).first?.path ?? ""
    }
    static var savedDirectory: String {
        let s = UserDefaults.standard.string(forKey: "saveDirectory") ?? ""
        return s.isEmpty ? defaultDirectory : s
    }
    func applySaveDirectory(_ path: String) {
        bridge.setSaveDirectory(path)
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
        let p = bridge.connectedPeers()
        if p != peers { peers = p }   // only publish on change (called ~4x/sec)
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

    // Accept the offer — the file is kept (it streams in / is already in).
    func acceptOffer(_ item: BVFileOfferItem) {
        bridge.acceptFile(item.correlationKey)
        pendingOffers.removeAll { $0 === item }
    }

    // Reject — delete the file now if it already arrived, else on completion.
    func rejectOffer(_ item: BVFileOfferItem) {
        bridge.rejectFile(item.correlationKey)
        pendingOffers.removeAll { $0 === item }
    }

    // MARK: - BVChatBridgeDelegate (main thread)
    func chatBridgePeersChanged()    { refreshPeers() }
    func chatBridgeMessagesChanged() { refreshMessages() }
    func chatBridgeFileProgress()    { transferStatus = bridge.transferStatus(); refreshMessages() }
    func chatBridgeFileOffered() {
        pendingOffers.append(contentsOf: bridge.takeFileOffers())
    }
}
