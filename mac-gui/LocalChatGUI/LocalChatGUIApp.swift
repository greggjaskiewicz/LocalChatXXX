import SwiftUI

@main
struct LocalChatGUIApp: App {
    @StateObject private var model = ChatModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 640, minHeight: 420)
                // Defer off the view-update pass (start() mutates @Published state).
                .onAppear { DispatchQueue.main.async { model.start() } }
        }
    }
}
