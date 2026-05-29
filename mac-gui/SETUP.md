# LocalChat macOS SwiftUI front-end

A mac-only SwiftUI GUI that sits on top of the existing C++ core. **Nothing in
the core (or the CLI / CMake build) is modified** — this is purely additive.

```
SwiftUI  ─▶  ChatModel (ObservableObject)
                 │
            BVChatBridge (Obj-C++)      ← Swift talks only to this
                 │  owns
            BVChatCore (C++)            ← reproduces main()'s setup
                 │
            BVApp_GuiClient : BVApp_ConsoleClient   ← reuses author's logic,
                                                       GUI-safe Run(), no terminal
```

The C++/Obj-C side (`BVApp_GuiClient.hpp`, `BVChatCore.hpp`, `BVChatBridge.h/.mm`)
all compile clean against the core. The Swift files are under `LocalChatGUI/`.

## Fastest path: xcodegen (reproducible)

```sh
brew install xcodegen          # once
cd mac-gui && xcodegen generate && open LocalChatGUI.xcodeproj
```

`project.yml` encodes every source (incl. the core `.cpp`s from `../src`), the
header search paths, the bridging header, the `-lspdlog -lfmt` link, and the
Bonjour Info.plist keys. Build & run two instances and they should discover each
other. The manual steps below are equivalent if you'd rather click it together.

## Create the Xcode app (manual alternative)

1. **Xcode ▸ File ▸ New ▸ Project ▸ macOS ▸ App.** Name `LocalChatGUI`,
   Interface **SwiftUI**, Language **Swift**. Save it under `mac-gui/`.
2. Delete the generated `ContentView.swift` (we have our own). Add to the target:
   - `LocalChatGUI/ChatModel.swift`, `ContentView.swift`, `LocalChatGUIApp.swift`
   - `BVChatBridge.h`, `BVChatBridge.mm`  (and the headers
     `BVChatCore.hpp`, `BVApp_GuiClient.hpp` — add as references)
3. **Bridging header:** add `LocalChatGUI/LocalChatGUI-Bridging-Header.h`, then set
   *Build Settings ▸ Swift Compiler – General ▸ Objective-C Bridging Header* to its path.
   (Swift only needs the Obj-C bridge, so no Swift/C++ interop flag is required.)

## Compile the core into the app (simplest, arch-correct)

Add these existing source files to the target (drag in as references, "Create groups"):

```
src/BVApp_ConsoleClient.cpp   # base class reused by BVApp_GuiClient
src/BVBroker.cpp
src/BVTCPConnectionManager.cpp
src/BVTCPSession.cpp
src/BVService_Bonjour.cpp
src/BVDiscovery_Bonjour.cpp
src/bonjour_api.c
src/bonjour_api_bridge.cpp
src/linked_list.c
```

(Alternatively link the prebuilt `libBV*.a` from a CMake build, but building from
source avoids arch/staleness mismatches.)

## Build settings

- **Header Search Paths** (recursive off): `$(SRCROOT)/../include`, `$(SRCROOT)/..`,
  `/opt/homebrew/include`
- **Library Search Paths:** `/opt/homebrew/lib`
- **Other Linker Flags:** `-lspdlog -lfmt`  (Homebrew spdlog; `dnssd` is in libSystem)
- **C++ Language Dialect:** `C++17`  •  **C Language Dialect:** default
- **Preprocessor Macros:** `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE`,
  `RESOLUTION_POLICY_HARD_FAIL=1`, and (Apple) `__APPLE__` is automatic
- For the `.mm`: **enable ARC** (default).

## Info.plist / entitlements (Bonjour needs these)

Add to **Info.plist**:
```xml
<key>NSLocalNetworkUsageDescription</key>
<string>LocalChat discovers and connects to peers on your local network.</string>
<key>NSBonjourServices</key>
<array>
  <string>_localchathost._tcp</string>
</array>
```

App Sandbox: easiest for a local tool is to **turn App Sandbox off**. If you keep
it on, enable: *Incoming Connections (Server)*, *Outgoing Connections (Client)*,
and *User Selected File ▸ Read* (and wrap the `fileImporter` URL in
`startAccessingSecurityScopedResource()` in `ChatModel.sendFile`).

## Run

Build & run. Discovered peers auto-connect (the core resolves + initiates the
session automatically), then appear under **Connected**. Select one to chat;
the paperclip sends a file.

## File offer / accept / reject — real handshake

The protocol now asks **before** anything transfers:

```
sender:   InitiateFileTransfer -> FILE_OFFER(key, name, size); request kept pending
receiver: FILE_OFFER -> APP_FILE_OFFER -> GUI prompts (CLI auto-accepts)
                     -> FILE_ACCEPT / FILE_REJECT(key)
sender:   FILE_ACCEPT -> build the transfer, stream BEGIN + chunks
          FILE_REJECT -> drop the pending request; nothing is sent
```

The transfer object (`BVFileTransferContext`) is unchanged — it's just created
*on accept* instead of immediately. `BVApp_ConsoleClient::HandleFileOffer`
auto-accepts (CLI); the GUI overrides it to prompt (`-acceptFile:`/`-rejectFile:`
reply with FILE_ACCEPT/REJECT). On reject, no bytes leave the sender.

**Both machines must run this build.** Mismatches degrade gracefully (no crash):
an old receiver never understands the OFFER so the file just never sends; an old
sender streams without offering so the new receiver gets it with no prompt.
