#pragma once
//
// BVApp_GuiClient  --  macOS GUI front-end (ADDITIVE).
//
// This subclasses the author's BVApp_ConsoleClient purely to *reuse* its
// networking / file / send logic unchanged. It overrides ONLY:
//   * Run()           - so it services the io_context without ever touching the
//                       terminal (no raw mode, no stdin, no stdout menus);
//   * the Handle*()    - to call straight through to the author's implementation
//     event handlers     and then emit one coarse "something changed" event the
//                       GUI can react to by re-reading state.
//
// Nothing in the existing codebase is modified. The console client, BVApp, the
// broker and the CMake/CLI build all stay exactly as they are.
//
#include <functional>
#include <mutex>
#include <vector>
#include <string>
#include <set>
#include <map>
#include <cstdint>
#include <system_error>
#include <filesystem>
#include "BVApp_ConsoleClient.hpp"

enum class BVGuiEvent
{
    ServicesChanged,
    SessionsChanged,
    MessagesChanged,
    FileProgress,
    FileOffered
};

// An incoming file offer, surfaced as the transfer BEGINS so the user can
// Accept/Reject up-front (the bytes still stream; Reject deletes on arrival).
struct BVFileOffer
{
    std::uint32_t correlationKey;
    std::string   fileName;
    std::string   sender;
    std::uint64_t size;
};

class BVApp_GuiClient : public BVApp_ConsoleClient
{
public:
    using Observer = std::function<void(BVGuiEvent)>;

    BVApp_GuiClient(const BVServiceData thisMachineServiceData,
                    std::shared_ptr<threadsafe_queue<BVMessage>> outMbx,
                    std::shared_ptr<threadsafe_queue<BVMessage>> inMbx,
                    boost::asio::io_context& ioContext)
        : BVApp_ConsoleClient(thisMachineServiceData, outMbx, inMbx, ioContext)
    {
    }

    // Set by the bridge; invoked (on the io / mailbox thread) after state updates.
    void SetObserver(Observer obs) { this->observer = std::move(obs); }

    // GUI-safe replacement for the console loop: just run the network event loop.
    // The bridge calls this on a background thread (it blocks until stopped).
    void Run(void) override
    {
        GetIoContext().run();
    }

    // Mirrors the console client's quit path (TERMINATE_ALL + stop io), exposed
    // so the bridge can shut the app down cleanly. SendMessage is protected on
    // BVComponent, hence this lives here rather than in the bridge.
    void Shutdown(void)
    {
        SendMessage(BVMessage(BVEventType::BVEVENTTYPE_TERMINATE_ALL, nullptr));
        SetIsRunning(false);
        StopIOContext();
    }

    // ---- read accessors for the bridge -----------------------------------
    std::vector<BVServiceBrowseInstance> Services(void)
    {
        return GetServiceVectorCopy();
    }

    std::vector<std::string> Sessions(void)
    {
        return GetConnectionManager().GetSessionServiceNamesInDisplayOrder();
    }

    std::vector<BVChatMessage> MessagesFor(const std::string& serviceName)
    {
        std::lock_guard<std::mutex> l(this->chatLogsMapMutex);
        auto it = chatLogsM.find(serviceName);
        if (it == chatLogsM.end())
        {
            return {};
        }
        return it->second.logV;
    }

    // Latest file-transfer status line for the UI, e.g. "Receiving x.png… 45%",
    // "Received x.png", "Sent y.png". Empty when nothing is in flight.
    std::string TransferStatus(void)
    {
        // Live outgoing send wins (it has no event stream, only this slot);
        // otherwise fall back to the incoming/"Sent"/"Received" status.
        const std::string outgoing = GetConnectionManager().CurrentOutgoingTransferStatus();
        if (!outgoing.empty())
        {
            return outgoing;
        }
        std::lock_guard<std::mutex> l(xferMutex);
        return xferStatus;
    }

    // ---- actions (reuse the author's send/file APIs) ----------------------
    bool SendText(const std::string& serviceName, const std::string& text)
    {
        // The actual socket write MUST run on the io_context thread (Boost.Asio
        // is driven single-threaded there). The GUI calls this from the main
        // thread, so post the work over instead of touching the socket directly.
        boost::asio::post(GetIoContext(), [this, serviceName, text]()
        {
            SessionID sid;
            if (GetConnectionManager().GetSessionIDFromServiceName(serviceName, sid)
                != BVStatus::BVSTATUS_OK)
            {
                LogWarn("[GUI] SendText: no live session for {}", serviceName);
                return;
            }
            std::unique_ptr<BVTCPMessage<BVChatMessagePayload>> msg =
                ConstructChatMessageFromInput(text);
            const uint64_t timestamp = msg->header.timestamp;
            if (GetConnectionManager().SendDataToNode(std::move(msg), sid)
                == BVStatus::BVSTATUS_FATAL_ERROR)
            {
                LogError("[GUI] SendText: send to {} failed", serviceName);
                return;
            }
            {
                std::lock_guard<std::mutex> l(this->chatLogsMapMutex);
                const BVChatMessage mine(text, timestamp, GetThisMachineServiceData().hostname);
                try
                {
                    chatLogsM.at(serviceName).AddMessage(mine);
                }
                catch (const std::out_of_range&)
                {
                    chatLogsM.emplace(serviceName, BVChatMessageLog(serviceName, mine));
                }
            }
            Notify(BVGuiEvent::MessagesChanged);
        });
        return true;
    }

    bool SendFile(const std::string& serviceName, const std::string& path)
    {
        std::error_code ec;
        const std::filesystem::path p = path;
        if (!std::filesystem::is_regular_file(p, ec))
        {
            return false;
        }
        // Same as SendText: initiate the transfer on the io_context thread.
        boost::asio::post(GetIoContext(), [this, serviceName, p]()
        {
            SessionID sid;
            if (GetConnectionManager().GetSessionIDFromServiceName(serviceName, sid)
                != BVStatus::BVSTATUS_OK)
            {
                LogWarn("[GUI] SendFile: no live session for {}", serviceName);
                return;
            }
            std::filesystem::path mutablePath = p;
            GetConnectionManager().InitiateFileTransferWithSession(sid, mutablePath);
            const std::string name = p.filename().string();
            AppendChatEntry(serviceName, "\xF0\x9F\x93\x8E Sent file: " + name,
                            GetThisMachineServiceData().hostname);
            {
                std::lock_guard<std::mutex> l(xferMutex);
                xferStatus = "Sent " + name;
            }
            Notify(BVGuiEvent::MessagesChanged);
            Notify(BVGuiEvent::FileProgress);
        });
        return true;
    }

    // ---- event handlers: author logic first, then notify the GUI ----------
    BVStatus HandlePublishedServices(std::unique_ptr<std::any> dp) override
    {
        BVStatus s = BVApp_ConsoleClient::HandlePublishedServices(std::move(dp));
        Notify(BVGuiEvent::ServicesChanged);
        return s;
    }
    BVStatus HandleResolvedServices(std::unique_ptr<std::any> dp) override
    {
        BVStatus s = BVApp_ConsoleClient::HandleResolvedServices(std::move(dp));
        Notify(BVGuiEvent::SessionsChanged);
        return s;
    }
    BVStatus HandleServiceDeregistration(std::unique_ptr<std::any> dp) override
    {
        BVStatus s = BVApp_ConsoleClient::HandleServiceDeregistration(std::move(dp));
        Notify(BVGuiEvent::ServicesChanged);
        return s;
    }
    BVStatus HandleMessageIncoming(std::unique_ptr<std::any> dp) override
    {
        BVStatus s = BVApp_ConsoleClient::HandleMessageIncoming(std::move(dp));
        Notify(BVGuiEvent::MessagesChanged);
        return s;
    }
    // Incoming OFFER (peer asks before sending). Surface it for Accept/Reject;
    // do NOT auto-accept (that's the CLI's behavior we override here).
    BVStatus HandleFileOffer(std::unique_ptr<std::any> dp) override
    {
        if (dp)
        {
            try
            {
                const BVTCPFileData fd = std::any_cast<BVTCPFileData>(*dp);
                std::string meta(fd.fdata.begin(), fd.fdata.end()); // "service|filename"
                const auto z = meta.find('\0');
                if (z != std::string::npos) { meta.resize(z); }
                const auto bar = meta.find('|');
                const std::string sender = (bar == std::string::npos) ? std::string{} : meta.substr(0, bar);
                const std::string name   = (bar == std::string::npos) ? meta : meta.substr(bar + 1);
                {
                    std::lock_guard<std::mutex> l(offerMutex);
                    pendingOffers.push_back(BVFileOffer{fd.correlationKey, name, sender, fd.fsize});
                    offerSenders[fd.correlationKey] = sender;
                }
                Notify(BVGuiEvent::FileOffered);
            }
            catch (const std::bad_any_cast&)
            {
            }
        }
        return BVStatus::BVSTATUS_OK;
    }

    BVStatus HandleFileTransferBegin(std::unique_ptr<std::any> dp) override
    {
        // Transfer is starting (peer already accepted). Just track progress.
        if (dp)
        {
            try
            {
                const BVTCPFileData fd = std::any_cast<BVTCPFileData>(*dp);
                std::string meta(fd.fdata.begin(), fd.fdata.end());
                const auto z = meta.find('\0');
                if (z != std::string::npos) { meta.resize(z); }
                const auto bar = meta.find('|');
                const std::string name = (bar == std::string::npos) ? meta : meta.substr(bar + 1);
                std::lock_guard<std::mutex> l(xferMutex);
                xferName = name;
                xferTotal = fd.fsize;
                xferReceived = 0;
                xferStatus = "Receiving " + name + "\xE2\x80\xA6 0%";
            }
            catch (const std::bad_any_cast&)
            {
            }
        }
        BVStatus s = BVApp_ConsoleClient::HandleFileTransferBegin(std::move(dp));
        Notify(BVGuiEvent::FileProgress);
        return s;
    }

    // Drains offers that arrived since the last call (the GUI prompts for each).
    std::vector<BVFileOffer> TakeFileOffers(void)
    {
        std::lock_guard<std::mutex> l(offerMutex);
        std::vector<BVFileOffer> out;
        out.swap(pendingOffers);
        return out;
    }
    void AcceptFile(std::uint32_t correlationKey) { RespondToOffer(correlationKey, true); }
    void RejectFile(std::uint32_t correlationKey) { RespondToOffer(correlationKey, false); }
    BVStatus HandleFileChunkSent(std::unique_ptr<std::any> dp) override
    {
        if (dp)
        {
            try
            {
                const BVTCPFileData fd = std::any_cast<BVTCPFileData>(*dp);
                std::lock_guard<std::mutex> l(xferMutex);
                xferReceived += fd.csize;
                if (xferTotal > 0)
                {
                    std::uint64_t pct = (xferReceived * 100) / xferTotal;
                    if (pct > 100) { pct = 100; }
                    xferStatus = "Receiving " + xferName + "\xE2\x80\xA6 " + std::to_string(pct) + "%";
                }
            }
            catch (const std::bad_any_cast&)
            {
            }
        }
        BVStatus s = BVApp_ConsoleClient::HandleFileChunkSent(std::move(dp));
        Notify(BVGuiEvent::FileProgress);
        return s;
    }
    BVStatus HandleFileTransferEnd(std::unique_ptr<std::any> dp) override
    {
        // A rejected transfer never starts (the sender drops it), so anything
        // that reaches END was accepted -> just record it.
        std::string serviceName;
        std::string fileName;
        bool haveInfo = false;
        if (dp)
        {
            try
            {
                const BVTCPFileData fd = std::any_cast<BVTCPFileData>(*dp);
                auto it = this->fileTransferData.find(fd.correlationKey);
                if (it != this->fileTransferData.end())
                {
                    serviceName = std::get<0>(it->second);
                    fileName    = std::get<1>(it->second);
                    haveInfo    = true;
                }
            }
            catch (const std::bad_any_cast&)
            {
            }
        }

        BVStatus s = BVApp_ConsoleClient::HandleFileTransferEnd(std::move(dp));

        if (haveInfo)
        {
            AppendChatEntry(serviceName, "\xF0\x9F\x93\x8E Received file: " + fileName, serviceName);
            std::lock_guard<std::mutex> l(xferMutex);
            xferStatus = "Received " + fileName;
            xferReceived = 0;
            xferTotal = 0;
        }
        Notify(BVGuiEvent::MessagesChanged);
        return s;
    }

private:
    Observer observer;

    // Incoming file offers awaiting Accept/Reject, plus key -> sender so the
    // reply can be routed back to the offering peer.
    std::mutex offerMutex;
    std::vector<BVFileOffer> pendingOffers;
    std::map<std::uint32_t, std::string> offerSenders;

    // Send the Accept/Reject reply on the io thread (it touches the session).
    void RespondToOffer(std::uint32_t key, bool accept)
    {
        std::string sender;
        {
            std::lock_guard<std::mutex> l(offerMutex);
            auto it = offerSenders.find(key);
            if (it != offerSenders.end()) { sender = it->second; offerSenders.erase(it); }
        }
        if (sender.empty()) { return; }
        boost::asio::post(GetIoContext(), [this, sender, key, accept]()
        {
            GetConnectionManager().RespondToFileOffer(sender, key, accept);
        });
    }

    // File-transfer progress line (incoming) + outgoing "Sent" status.
    std::mutex xferMutex;
    std::string xferStatus;
    std::string xferName;
    std::uint64_t xferTotal{0};
    std::uint64_t xferReceived{0};

    // Append a synthetic chat-log entry (used for "Sent/Received file: …").
    void AppendChatEntry(const std::string& serviceName,
                         const std::string& text,
                         const std::string& sender)
    {
        std::lock_guard<std::mutex> l(this->chatLogsMapMutex);
        const BVChatMessage m(text, 0, sender);
        try
        {
            chatLogsM.at(serviceName).AddMessage(m);
        }
        catch (const std::out_of_range&)
        {
            chatLogsM.emplace(serviceName, BVChatMessageLog(serviceName, m));
        }
    }

    void Notify(BVGuiEvent e)
    {
        if (observer)
        {
            observer(e);
        }
    }
};
