#pragma once
//
// BVChatCore  --  macOS GUI back-end facade (ADDITIVE).
//
// Reproduces what main() does (register the Bonjour service, set up logging,
// create discovery + the GUI app client, attach to the broker, subscribe the
// events, launch the worker/mailbox/io threads), but drives a BVApp_GuiClient
// instead of the console client and runs the io_context on a background thread.
//
// This is the single C++ object the Objective-C++ bridge owns. Nothing in the
// existing codebase is modified.
//
#if __APPLE__

#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <boost/asio.hpp>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

#include "const.h"
#include "BVBroker.hpp"
#include "BVMessage.hpp"
#include "threadsafequeue.hpp"
#include "BVService_Bonjour.hpp"
#include "BVDiscovery_Bonjour.hpp"
#include "BVApp_GuiClient.hpp"

class BVChatCore
{
public:
    using EventCallback = std::function<void(BVGuiEvent)>;

    BVChatCore() = default;
    ~BVChatCore() { stop(); }

    BVChatCore(const BVChatCore&) = delete;
    BVChatCore& operator=(const BVChatCore&) = delete;

    // Full setup, mirroring main(). Returns false if any crucial step fails.
    bool start(EventCallback cb)
    {
        if (running)
        {
            return true;
        }
        SetupLogger();

        std::string hostname = boost::asio::ip::host_name();
        std::string domain   = "local.";

        service = std::make_unique<BVService_Bonjour>(hostname, domain, PORT);
        if (service->Register() != BVStatus::BVSTATUS_OK)
        {
            return false;
        }
        const BVServiceData hostData = service->GetHostData();

        broker = std::make_unique<BVBroker>(std::make_shared<threadsafe_queue<BVMessage>>());

        discovery = std::make_unique<BVDiscovery_Bonjour>(
            hostData,
            ioContext,
            std::make_shared<threadsafe_queue<BVMessage>>(),
            std::make_shared<threadsafe_queue<BVMessage>>());
        discovery->SetLogger(logger);

        app = std::make_unique<BVApp_GuiClient>(
            hostData,
            std::make_shared<threadsafe_queue<BVMessage>>(),
            std::make_shared<threadsafe_queue<BVMessage>>(),
            ioContext);
        app->SetLogger(logger);
        app->GetConnectionManager().SetLogger(logger);
        app->GetConnectionManager().StartAcceptingConnections();
        app->SetObserver(std::move(cb));

        if (broker->Attach(*discovery) != BVStatus::BVSTATUS_OK ||
            broker->Attach(*app)       != BVStatus::BVSTATUS_OK)
        {
            return false;
        }

        const SubscriberID dsid = discovery->GetSubscriberId();
        const SubscriberID asid = app->GetSubscriberId();
        bool ok = true;
        ok &= broker->Subscribe(dsid, BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_START)    == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(dsid, BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_PAUSE)    == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(dsid, BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_RESUME)   == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(dsid, BVEventType::BVEVENTTYPE_TERMINATE_ALL)              == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(dsid, BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_SHUTDOWN) == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(dsid, BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_RESTART)  == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(dsid, BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_RESOLVE)  == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(asid, BVEventType::BVEVENTTYPE_APP_PUBLISHED_SERVICE)           == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(asid, BVEventType::BVEVENTTYPE_APP_DISCOVERY_SERVICE_RESOLVED)  == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(asid, BVEventType::BVEVENTTYPE_TERMINATE_ALL)                   == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(asid, BVEventType::BVEVENTTYPE_APP_DEREGISTERED_SERVICE)        == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(asid, BVEventType::BVEVENTTYPE_APP_MESSAGE_INCOMING)            == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(asid, BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_BEGIN)         == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(asid, BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_CHUNK_SENT)    == BVStatus::BVSTATUS_OK;
        ok &= broker->Subscribe(asid, BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_END)           == BVStatus::BVSTATUS_OK;
        if (!ok)
        {
            return false;
        }

        broker->LaunchWorkerThread();
        discovery->StartListeningOnMailbox();
        discovery->LaunchWorkingThread();
        app->StartListeningOnMailbox();

        running = true;
        ioThread = std::thread([this] { app->Run(); }); // app->Run() == ioContext.run()
        return true;
    }

    void stop()
    {
        if (!running)
        {
            return;
        }
        running = false;
        if (app)
        {
            app->Shutdown();
        }
        if (ioThread.joinable())
        {
            ioThread.join();
        }
        if (discovery)
        {
            discovery->TryJoinMailBoxThread();
            discovery->TryJoinWorkerThread();
        }
        if (app)
        {
            app->TryJoinMailBoxThread();
        }
        if (broker)
        {
            broker->TryJoinWorkerThread();
            if (discovery) { (void)broker->Detach(discovery->GetSubscriberId()); }
            if (app)       { (void)broker->Detach(app->GetSubscriberId()); }
        }
    }

    // ---- pass-through queries / actions (safe if not started) -------------
    std::vector<BVServiceBrowseInstance> services()
    {
        return app ? app->Services() : std::vector<BVServiceBrowseInstance>{};
    }
    std::vector<std::string> sessions()
    {
        return app ? app->Sessions() : std::vector<std::string>{};
    }
    std::vector<BVChatMessage> messages(const std::string& serviceName)
    {
        return app ? app->MessagesFor(serviceName) : std::vector<BVChatMessage>{};
    }
    bool sendText(const std::string& serviceName, const std::string& text)
    {
        return app ? app->SendText(serviceName, text) : false;
    }
    bool sendFile(const std::string& serviceName, const std::string& path)
    {
        return app ? app->SendFile(serviceName, path) : false;
    }
    std::string thisHostname()
    {
        return service ? service->GetHostData().hostname : std::string{};
    }
    std::vector<BVReceivedFile> takeReceivedFiles()
    {
        return app ? app->TakeReceivedFiles() : std::vector<BVReceivedFile>{};
    }

private:
    void SetupLogger()
    {
        if (logger) { return; }
        try
        {
            // A windowed app's working directory is NOT the project folder, so a
            // relative "logs/" path is unfindable (or unwritable). Log to a fixed
            // absolute path under $HOME instead.
            const char* home = std::getenv("HOME");
            const std::string logPath =
                std::string(home ? home : "/tmp") + "/localchat_gui.log";
            spdlog::drop("gui_logger");
            logger = spdlog::basic_logger_mt("gui_logger", logPath, true);
            logger->set_level(spdlog::level::trace);
            logger->flush_on(spdlog::level::trace);
        }
        catch (const std::exception&)
        {
            // Logging is best-effort; carry on without a file logger.
        }
    }

    std::atomic_bool running{false};
    boost::asio::io_context ioContext;            // must outlive the components below
    std::shared_ptr<spdlog::logger> logger;
    std::unique_ptr<BVService_Bonjour> service;
    std::unique_ptr<BVBroker> broker;
    std::unique_ptr<BVDiscovery_Bonjour> discovery;
    std::unique_ptr<BVApp_GuiClient> app;
    std::thread ioThread;
};

#endif // __APPLE__
