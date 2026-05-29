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
#include <system_error>
#include <filesystem>
#include "BVApp_ConsoleClient.hpp"

enum class BVGuiEvent
{
    ServicesChanged,
    SessionsChanged,
    MessagesChanged,
    FileProgress,
    FileReceived
};

// A file that finished arriving and is now on disk, pending the user's
// Keep/Discard decision in the GUI.
struct BVReceivedFile
{
    std::string serviceName;
    std::string fileName;
    std::string path;
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
    BVStatus HandleFileTransferBegin(std::unique_ptr<std::any> dp) override
    {
        BVStatus s = BVApp_ConsoleClient::HandleFileTransferBegin(std::move(dp));
        Notify(BVGuiEvent::SessionsChanged);
        return s;
    }
    BVStatus HandleFileChunkSent(std::unique_ptr<std::any> dp) override
    {
        BVStatus s = BVApp_ConsoleClient::HandleFileChunkSent(std::move(dp));
        Notify(BVGuiEvent::FileProgress);
        return s;
    }
    BVStatus HandleFileTransferEnd(std::unique_ptr<std::any> dp) override
    {
        // Capture which file is completing BEFORE the base handler erases its
        // per-transfer bookkeeping, so the GUI can offer Keep/Discard once the
        // file has landed on disk.
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
            std::error_code ec;
            const std::filesystem::path path =
                std::filesystem::current_path(ec) / "data" / serviceName / fileName;
            {
                std::lock_guard<std::mutex> l(receivedFilesMutex);
                receivedFiles.push_back(BVReceivedFile{serviceName, fileName, path.string()});
            }
            Notify(BVGuiEvent::FileReceived);
        }
        else
        {
            Notify(BVGuiEvent::MessagesChanged);
        }
        return s;
    }

    // Drains the files that have arrived since the last call (the GUI shows a
    // Keep/Discard prompt for each).
    std::vector<BVReceivedFile> TakeReceivedFiles(void)
    {
        std::lock_guard<std::mutex> l(receivedFilesMutex);
        std::vector<BVReceivedFile> out;
        out.swap(receivedFiles);
        return out;
    }

private:
    Observer observer;
    std::mutex receivedFilesMutex;
    std::vector<BVReceivedFile> receivedFiles;
    void Notify(BVGuiEvent e)
    {
        if (observer)
        {
            observer(e);
        }
    }
};
