#include <iostream>
#include <boost/asio.hpp>
#include <thread>
#include <future>
#include <condition_variable>
#include <chrono>
#include <queue>
#include <memory>
#include <filesystem>
#include "BVDiscovery.hpp"
#include "BVBroker.hpp"
#include "BVMessage.hpp"
#include "threadsafequeue.hpp"
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE // TODO: Set this up based on build variable
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#if __APPLE__
#include "dns_sd.h"
#include "BVService_Bonjour.hpp"
#include "BVDiscovery_Bonjour.hpp"
#elif __linux__
#include "BVService_Avahi.hpp"
#include "BVDiscovery_Avahi.hpp"
#endif
#include "BVApp_ConsoleClient.hpp"

int main(int argc, char** argv)
{
#if __APPLE__
    uint32_t v;
    uint32_t size = sizeof(v);
    DNSServiceErrorType err = DNSServiceGetProperty(kDNSServiceProperty_DaemonVersion, &v, &size);
    if (err)
    {
        std::cerr << "DNSServiceGetProperty failed " << (long int)err << std::endl;
        return -1;
    } else
    {
        std::cout << "Currently running daemon is version "
                  << v / 10000 << "." << v / 100 % 100 << "." << v % 100 << std::endl;
    }
    /* */
#endif
    std::string hostname = boost::asio::ip::host_name();  // Use an appropriate host name or retrieve it
    std::string domain = "local.";

    /*
        Let's keep registration procedure synchronous, at least for now.
        It really is a crucial step in order for the application to function.
    */

    // This has to be a parent class BVService, not dependent on implementation
#if __APPLE__
    BVService_Bonjour service{hostname, domain, PORT};
#elif __linux__
    // AvahiSimplePoll will be used by avahi service registration and then service browsing
    // TODO: shouldn't it be unique ptr?
    // Even if service does not go out of scope/is destroyed,
    // it doesn't mean that there should be one simple poll shared by all of the objects.
    // fix it later...
    std::shared_ptr<AvahiSimplePoll> simple_poll_p = BVService_Avahi::MakeSimplePoll(avahi_simple_poll_new());
    if (simple_poll_p == nullptr)
    {
        std::cerr << "Failed to create simple poll object." << std::endl;
        std::exit(-1);
    }
    BVService_Avahi service{hostname, domain, PORT, simple_poll_p};
#endif
    BVStatus status = service.Register(); // blocks
    if (status == BVStatus::BVSTATUS_OK)
    {
        std::cout << "Setup successful" << std::endl;
    } else
    {
        std::cerr << "Setup failed" << std::endl;
        std::exit(-1);
    }
    // From this point, the context pointers of both implementations are alive, and the services are discoverable

    // BVDiscovery_XXX class doesn't need a pointer to the service class.
    // It needs only:
    // hostname, domain, port and context pointer:
    // In case of Bonjour, it's null, because DNSServiceRef is allocated upon browsing
    // In case of Avahi, a pointer to AvahiClient
    // Furthermore, Bonjour Discovery class is NOT utilizing any data that's specific to the BVService class,
    // because hostname, domain and type are known ahead of the creation of the object

    boost::asio::io_context ioContext;
    BVBroker broker{std::make_shared<threadsafe_queue<BVMessage>>()};

    std::shared_ptr<spdlog::logger> fileLogger;
    namespace fs = std::filesystem;
    spdlog::set_level(spdlog::level::trace);
    spdlog::set_pattern("[%H:%M:%S %z] [logger %n] [%l] [thread %t] %v");
    try 
    {
        fs::path logDir = "logs";
        fs::create_directories(logDir);

        std::string fileName = "trace.log";

        std::string loggerName = "main_logger";
        spdlog::drop(loggerName);

        fs::path logPath = logDir / fileName;

        fileLogger = spdlog::basic_logger_mt(
            loggerName,
            logPath.string(),
            true);
        fileLogger->set_level(spdlog::level::trace);
        fileLogger->flush_on(spdlog::level::trace);
        SPDLOG_LOGGER_TRACE(fileLogger, "Logger set up.");
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        std::cout << "Log init failed: " << ex.what() << std::endl;
        throw std::runtime_error("Cannot init logging handle...");
    }
    const BVServiceData thisMachineServiceData = service.GetHostData();
#if __APPLE__
    // Create a discovery object, that periodically performs DNS-SD functionality.
    std::shared_ptr<const BVService_Bonjour> service_p = // TODO: Redundant, remove!
        std::make_shared<const BVService_Bonjour>(service);
    BVDiscovery_Bonjour discovery{thisMachineServiceData,
                                  ioContext,
                                  std::make_shared<threadsafe_queue<BVMessage>>(),
                                  std::make_shared<threadsafe_queue<BVMessage>>()};
    discovery.SetLogger(fileLogger);
#endif
#if __linux__
    auto data = service.TransferClient();
    BVDiscovery_Avahi discovery{std::move(data),
                                ioContext,
                                service.GetHostData(),
                                simple_poll_p,
                                std::make_shared<threadsafe_queue<BVMessage>>(),
                                std::make_shared<threadsafe_queue<BVMessage>>()};
#endif
    BVApp_ConsoleClient consoleClient{thisMachineServiceData,
                                      std::make_shared<threadsafe_queue<BVMessage>>(),
                                      std::make_shared<threadsafe_queue<BVMessage>>(),
                                      ioContext};
    consoleClient.SetLogger(fileLogger);
    consoleClient.GetConnectionManager().SetLogger(fileLogger);
    // we Start Accepting Connections here, after setting the logger.
    consoleClient.GetConnectionManager().StartAcceptingConnections();

    // Broker setup
    BVStatus attachStatusDiscovery = broker.Attach(discovery);
    BVStatus attachStatusApp = broker.Attach(consoleClient);
    if (attachStatusDiscovery != BVStatus::BVSTATUS_OK ||
        attachStatusApp       != BVStatus::BVSTATUS_OK)
    {
        std::cerr << "Fatal error: Broker couldn't attach all components..." << std::endl;
        exit(-1);
    }

    BVStatus subStatusDiscoveryRequestStart = 
        broker.Subscribe(discovery.GetSubscriberId(), BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_START);
    BVStatus subStatusDiscoveryRequestPause = 
        broker.Subscribe(discovery.GetSubscriberId(), BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_PAUSE);
    BVStatus subStatusDiscoveryRequestResume = 
        broker.Subscribe(discovery.GetSubscriberId(), BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_RESUME);
    BVStatus subStatusDiscoveryTerminateAll = 
        broker.Subscribe(discovery.GetSubscriberId(), BVEventType::BVEVENTTYPE_TERMINATE_ALL);
    BVStatus subStatusDiscoveryRequestShutdown = 
        broker.Subscribe(discovery.GetSubscriberId(), BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_SHUTDOWN);
    BVStatus subStatusDiscoveryRequestRestart = 
        broker.Subscribe(discovery.GetSubscriberId(),  BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_RESTART);
    BVStatus subStatusDiscoveryRequestResolve = 
        broker.Subscribe(discovery.GetSubscriberId(), BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_RESOLVE);
    BVStatus subStatusAppRequestRestart = 
        broker.Subscribe(consoleClient.GetSubscriberId(), BVEventType::BVEVENTTYPE_APP_PUBLISHED_SERVICE);
    BVStatus subStatusAppServiceResolved = 
        broker.Subscribe(consoleClient.GetSubscriberId(), BVEventType::BVEVENTTYPE_APP_DISCOVERY_SERVICE_RESOLVED);
    // this is needed to exit listening on mailbox!
    BVStatus subStatusAppRequestTerminate = 
        broker.Subscribe(consoleClient.GetSubscriberId(), BVEventType::BVEVENTTYPE_TERMINATE_ALL);
    BVStatus subStatusAppServiceDeregistered = 
        broker.Subscribe(consoleClient.GetSubscriberId(), BVEventType::BVEVENTTYPE_APP_DEREGISTERED_SERVICE);
    BVStatus subStatusAppMessageIncoming =
        broker.Subscribe(consoleClient.GetSubscriberId(), BVEventType::BVEVENTTYPE_APP_MESSAGE_INCOMING);
    BVStatus subStatusAppFileTransferBegin =
        broker.Subscribe(consoleClient.GetSubscriberId(), BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_BEGIN);
    BVStatus subStatusAppFileChunkSent =
        broker.Subscribe(consoleClient.GetSubscriberId(), BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_CHUNK_SENT);
    BVStatus subStatusAppFileTransferEnd =
        broker.Subscribe(consoleClient.GetSubscriberId(), BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_END);
    if (subStatusDiscoveryRequestStart    != BVStatus::BVSTATUS_OK ||
        subStatusDiscoveryRequestPause    != BVStatus::BVSTATUS_OK ||
        subStatusDiscoveryRequestResume   != BVStatus::BVSTATUS_OK ||
        subStatusDiscoveryTerminateAll    != BVStatus::BVSTATUS_OK ||
        subStatusDiscoveryRequestShutdown != BVStatus::BVSTATUS_OK ||
        subStatusDiscoveryRequestRestart  != BVStatus::BVSTATUS_OK ||
        subStatusDiscoveryRequestResolve  != BVStatus::BVSTATUS_OK ||
        subStatusAppRequestRestart        != BVStatus::BVSTATUS_OK ||
        subStatusAppServiceResolved       != BVStatus::BVSTATUS_OK ||
        subStatusAppRequestTerminate      != BVStatus::BVSTATUS_OK ||
        subStatusAppServiceDeregistered   != BVStatus::BVSTATUS_OK ||
        subStatusAppMessageIncoming       != BVStatus::BVSTATUS_OK ||
        subStatusAppFileTransferBegin     != BVStatus::BVSTATUS_OK ||
        subStatusAppFileChunkSent         != BVStatus::BVSTATUS_OK ||
        subStatusAppFileTransferEnd       != BVStatus::BVSTATUS_OK
    )
    {
        std::cerr << "Fatal error: Broker couldn't subscribe to a crucial event" << std::endl;
        exit(-1);
    }

    broker.LaunchWorkerThread();
    discovery.StartListeningOnMailbox();
    discovery.LaunchWorkingThread();
    consoleClient.StartListeningOnMailbox();
    // Run() runs the io_context on this (main) thread, so we do not launch a
    // separate io thread for the console client - that keeps every UI action and
    // network handler on one thread and avoids stdout races.
    consoleClient.Run();

    // Join all
    discovery.TryJoinMailBoxThread();
    discovery.TryJoinWorkerThread();
    consoleClient.TryJoinMailBoxThread();
    consoleClient.TryJoinIOThread();
    broker.TryJoinWorkerThread();

    // Detach
    BVStatus dDiscoveryStatus = broker.Detach(discovery.GetSubscriberId());
    BVStatus dAppStatus = broker.Detach(consoleClient.GetSubscriberId());
    if (dDiscoveryStatus != BVStatus::BVSTATUS_OK || dAppStatus != BVStatus::BVSTATUS_OK)
    {
        std::cerr << "Fatal error: Broker couldn't detach all components..." << std::endl;
        exit(-1);
    }

    return 0;
}
