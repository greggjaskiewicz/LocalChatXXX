#include "BVApp_ConsoleClient.hpp"

BVApp_ConsoleClient::BVApp_ConsoleClient(const BVServiceData _thisMachineServiceData,
                                         std::shared_ptr<threadsafe_queue<BVMessage>> _outMbx,
                                         std::shared_ptr<threadsafe_queue<BVMessage>> _inMbx,
                                         boost::asio::io_context& _ioContext) :
BVApp(_ioContext, _thisMachineServiceData),
BVComponent(_outMbx, _inMbx)
{
    RegisterCallback(BVEventType::BVEVENTTYPE_APP_PUBLISHED_SERVICE,
                     std::bind(&BVApp_ConsoleClient::HandlePublishedServices, this, std::placeholders::_1));
    RegisterCallback(BVEventType::BVEVENTTYPE_TERMINATE_ALL,
                     std::bind(&BVApp_ConsoleClient::OnShutdown, this, std::placeholders::_1));
    RegisterCallback(BVEventType::BVEVENTTYPE_APP_DISCOVERY_SERVICE_RESOLVED,
                     std::bind(&BVApp_ConsoleClient::HandleResolvedServices, this, std::placeholders::_1));
    RegisterCallback(BVEventType::BVEVENTTYPE_APP_DEREGISTERED_SERVICE,
                     std::bind(&BVApp_ConsoleClient::HandleServiceDeregistration, this, std::placeholders::_1));
    RegisterCallback(BVEventType::BVEVENTTYPE_APP_MESSAGE_INCOMING,
                     std::bind(&BVApp_ConsoleClient::HandleMessageIncoming, this, std::placeholders::_1));
    RegisterCallback(BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_BEGIN,
                     std::bind(&BVApp_ConsoleClient::HandleFileTransferBegin, this, std::placeholders::_1));
    RegisterCallback(BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_CHUNK_SENT,
                     std::bind(&BVApp_ConsoleClient::HandleFileChunkSent, this, std::placeholders::_1));
    RegisterCallback(BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_END,
                     std::bind(&BVApp_ConsoleClient::HandleFileTransferEnd, this, std::placeholders::_1));

    // Set getter that returns a correct pointer to Apps inMailBox
    this->GetConnectionManager().SetMailboxGetterF(
        [this]() -> std::shared_ptr<threadsafe_queue<BVMessage>>
        {
            return this->GetInMailBox();
        }
    );
    // this->GetConnectionManager().SetAppInMailBoxP(_inMbx);
    // maybe set a callback for connection manager to be GetInMailBox

    // TODO: Create an auxhilary object which listens to messages
    //       coming from App to sessions and that should be routed from sessions
    //       to App AND route its traffic to global queue
    //       Or don't create other components - just make each session a component 
    //       and somehow redirect their produced messages into global queue
    //       We can just pass the pointer to the existing inMailbox_p
    // 
}

void BVApp_ConsoleClient::Run(void)
{
    // Put the terminal in raw mode: no canonical line buffering and no kernel
    // echo. We now read keystrokes one at a time and draw everything ourselves.
    this->terminal.SetNonCanonicalMode();

    // Drive stdin through the same io_context that already handles the network.
    // A keystroke and an incoming message then become two kinds of asio event
    // handled on one thread, so the screen updates the moment a message arrives -
    // no polling, and no locking around stdout.
    // dup() so asio owns its own descriptor; toggling O_NONBLOCK on it does not
    // disturb the tty shared with the rest of the process.
    this->stdinDescriptor.emplace(GetIoContext(), ::dup(STDIN_FILENO));

    this->uiState = UIState::MainMenu;
    this->Render();
    this->StartStdinRead();

    // Run the event loop on the main thread; it returns when StopIOContext() is
    // called on quit. Keys, network handlers and posted redraws all run here,
    // single-threaded.
    GetIoContext().run();

    this->terminal.Restore();
}

void BVApp_ConsoleClient::StartStdinRead(void)
{
    if (!this->stdinDescriptor.has_value())
    {
        return;
    }
    this->stdinDescriptor->async_read_some(
        boost::asio::buffer(&this->readCh, 1),
        [this](const boost::system::error_code& ec, std::size_t n)
        {
            if (ec)
            {
                if (ec != boost::asio::error::operation_aborted)
                {
                    LogError("App: stdin read error: {}", ec.message());
                }
                return;
            }
            if (n == 1)
            {
                this->OnKey(this->readCh);
                if (this->GetIsRunning())
                {
                    this->StartStdinRead();
                }
            }
        });
}

void BVApp_ConsoleClient::OnKey(char c)
{
    if (this->uiState == UIState::Chat)
    {
        this->HandleChatKey(c);
    }
    else
    {
        this->HandleMainMenuKey(c);
    }
}

void BVApp_ConsoleClient::HandleMainMenuKey(char key)
{
    auto action = ParseConsoleActionFromKey(key);
    if (!action.has_value())
    {
        return; // unbound key
    }
    switch (action->type)
    {
        case BVConsoleActionType::BVCONSOLEACTION_REPRINT:
            Render();
            break;
        case BVConsoleActionType::BVCONSOLEACTION_SENDMSG:
            if (action->num.has_value())
            {
                EnterChat(action->num.value());
            }
            break;
        case BVConsoleActionType::BVCONSOLEACTION_PAUSE_DISCOVERY:
            LogTrace("App: Pause discovery message sent.");
            SendMessage(BVMessage(BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_PAUSE, nullptr));
            break;
        case BVConsoleActionType::BVCONSOLEACTION_RESUME_DISCOVERY:
            LogTrace("App: Resume discovery message sent.");
            SendMessage(BVMessage(BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_RESUME, nullptr));
            break;
        case BVConsoleActionType::BVCONSOLEACTION_QUIT:
            LogTrace("App: quitting. Sending TERMINATE_ALL.");
            SendMessage(BVMessage(BVEventType::BVEVENTTYPE_TERMINATE_ALL, nullptr));
            SetIsRunning(false);
            StopIOContext(); // unblocks GetIoContext().run() in Run()
            break;
        case BVConsoleActionType::BVCONSOLEACTION_BLOCKHOST:
            // send blockhost event/message
            break;
    }
}

void BVApp_ConsoleClient::EnterChat(int selection)
{
    // 'selection' is the 1-based number shown next to an established session
    // (see PrintSessions). Map it to that session's service name.
    const std::vector<std::string> sessionNames =
        this->GetConnectionManager().GetSessionServiceNamesInDisplayOrder();
    const int idx = selection - 1;
    if (idx < 0 || idx >= static_cast<int>(sessionNames.size()))
    {
        LogWarn("App: No session at selection {}", selection);
        return;
    }
    this->activeChatService = sessionNames[static_cast<std::size_t>(idx)];
    this->uiState = UIState::Chat;
    this->inputLine.clear();
    this->lastNotification.clear(); // we're now viewing a chat; drop the banner
    LogDebug("App: Entering chat with {}", this->activeChatService);
    RenderChat();
}

void BVApp_ConsoleClient::HandleChatKey(char c)
{
    if (c == '\n' || c == '\r')
    {
        std::cout << '\n' << std::flush;
        const std::string line = this->inputLine;
        this->inputLine.clear();
        if (line == "|q")
        {
            this->uiState = UIState::MainMenu;
            this->activeChatService.clear();
            Render();
            return;
        }
        if (line.empty() || line == "|r")
        {
            RenderChat(); // explicit refresh / ignore empty line
            return;
        }
        SendChatLine(this->activeChatService, line);
        RenderChat();
        return;
    }
    if (c == 127 || c == 8) // backspace / delete
    {
        if (!this->inputLine.empty())
        {
            this->inputLine.pop_back();
            std::cout << "\b \b" << std::flush;
        }
        return;
    }
    if (std::isprint(static_cast<unsigned char>(c)))
    {
        this->inputLine.push_back(c);
        std::cout << c << std::flush; // echo as typed
    }
}

void BVApp_ConsoleClient::SendChatLine(const std::string& serviceName, const std::string& line)
{
    SessionID sid;
    if (GetConnectionManager().GetSessionIDFromServiceName(serviceName, sid) != BVStatus::BVSTATUS_OK)
    {
        LogError("Couldn't get sid from {}", serviceName);
        return;
    }

    // "|f <path>" sends a file instead of a text message.
    if (line.rfind("|f ", 0) == 0)
    {
        std::string argStr = line.substr(3);
        // Terminal drag-and-drop tends to add a trailing space, wrap the path in
        // quotes, and/or backslash-escape spaces. Normalise all of that so a
        // dropped path works the same as a typed one.
        auto isSpace = [](char c) { return c == ' ' || c == '\t'; };
        while (!argStr.empty() && isSpace(argStr.front())) { argStr.erase(argStr.begin()); }
        while (!argStr.empty() && isSpace(argStr.back()))   { argStr.pop_back(); }
        if (argStr.size() >= 2 &&
            ((argStr.front() == '\'' && argStr.back() == '\'') ||
             (argStr.front() == '"'  && argStr.back() == '"')))
        {
            argStr = argStr.substr(1, argStr.size() - 2);
        }
        std::string unescaped; // turn "\ " (escaped space) back into " "
        unescaped.reserve(argStr.size());
        for (std::size_t i = 0; i < argStr.size(); ++i)
        {
            if (argStr[i] == '\\' && i + 1 < argStr.size() && argStr[i + 1] == ' ')
            {
                continue; // drop the backslash, keep the space
            }
            unescaped.push_back(argStr[i]);
        }
        argStr = unescaped;
        LogDebug("[BVApp_ConsoleClient]: Sending file, path: '{}'", argStr);
        std::filesystem::path filePath = argStr;
        std::error_code fsec;
        if (std::filesystem::is_regular_file(filePath, fsec))
        {
            LogDebug("[BVApp_ConsoleClient]: File {} exists!", argStr);
            GetConnectionManager().InitiateFileTransferWithSession(sid, filePath);
            LogDebug("[BVApp_ConsoleClient]: Initiated file transfer with session: {}", sid);
        }
        else
        {
            LogWarn("[BVApp_ConsoleClient]: {} is not a regular file - not sending.", argStr);
        }
        return;
    }

    std::unique_ptr<BVTCPMessage<BVChatMessagePayload>> chatMsg = ConstructChatMessageFromInput(line);
    const uint64_t timestamp = chatMsg->header.timestamp;
    if (GetConnectionManager().SendDataToNode(std::move(chatMsg), sid) == BVStatus::BVSTATUS_FATAL_ERROR)
    {
        LogError("App: No session for {} - message not sent.", serviceName);
        return;
    }
    {
        std::lock_guard<std::mutex> l(chatLogsMapMutex);
        const BVChatMessage mine(line, timestamp, GetThisMachineServiceData().hostname);
        try
        {
            chatLogsM.at(serviceName).AddMessage(mine);
        }
        catch (const std::out_of_range&)
        {
            chatLogsM.emplace(serviceName, BVChatMessageLog(serviceName, mine));
        }
    }
}

void BVApp_ConsoleClient::Render(void)
{
    if (this->uiState == UIState::Chat)
    {
        RenderChat();
    }
    else
    {
        PrintAll();
    }
}

void BVApp_ConsoleClient::RenderChat(void)
{
    PrintAll(); // PrintAll() clears the screen and prints services/sessions
    {
        std::lock_guard<std::mutex> l(chatLogsMapMutex);
        auto it = chatLogsM.find(this->activeChatService);
        if (it != chatLogsM.end())
        {
            std::cout << "Message log with " << this->activeChatService << std::endl;
            it->second.PrintNLastMessages(TEN_LAST_MESSAGES);
        }
        else
        {
            std::cout << "No messages with " << this->activeChatService << " :)" << std::endl;
        }
    }
    std::cout << "(|q quit chat) (|f <path> send file) (|r redraw)" << std::endl;
    std::cout << ">> " << this->inputLine << std::flush;
}

inline void BVApp_ConsoleClient::ClearScreen(void)
{
    for (int i = 0; i < 200; i++) {std::cout << std::endl;}
}

// I think that any event that needs to draw something
// must redraw everything
void BVApp_ConsoleClient::PrintAll(void)
{
    ClearScreen();
    std::cout << "LocalChat console client v0.4.0" << std::endl;
    std::cout << "Re(D)raw" << std::endl;
    std::cout << "(1-9) Chat with a session listed below" << std::endl;
    std::cout << "(P)ause discovery" << std::endl;
    std::cout << "(R)esume discovery" << std::endl;
    std::cout << "(Q)uit" << std::endl;
    std::cout << "-----------------------------" << std::endl;
    std::cout << "Discovered services:" << std::endl;
    this->PrintServices();
    // TODO: statuses like is discovery paused...
    std::cout << "-----------------------------" << std::endl;
    std::cout << "Sessions  --  press the number to chat:" << std::endl;
    this->GetConnectionManager().PrintSessions();
    if (!this->lastNotification.empty())
    {
        std::cout << "-----------------------------" << std::endl;
        std::cout << ">> " << this->lastNotification << std::endl;
    }
    std::cout << "=============================" << std::endl;
    std::cout << std::flush;
}

BVStatus BVApp_ConsoleClient::PrintServices(void)
{
    // std::lock_guard<std::mutex> l(this->serviceVectorMutex);
    BVStatus status = BVStatus::BVSTATUS_OK;
    if (this->serviceV.size() == 0)
    {
        std::cout << "None available apart from ours... :(" << std::endl;
        std::cout << this->GetThisMachineServiceData().hostname << std::endl;
    }
    // Not numbered on purpose: discovery is just status. You chat with the
    // numbered sessions list below, not with raw discovered services.
    for (BVServiceBrowseInstance& bI : this->serviceV)
    {
        std::cout << "  - ";
        bI.print();
    }
    return status;
}

BVStatus BVApp_ConsoleClient::HandlePublishedServices(std::unique_ptr<std::any> dp)
{
    using BVServiceBrowseInstanceList = std::list<BVServiceBrowseInstance>;
    if (dp == nullptr)
    {
        LogError("App: No new services received.");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    BVServiceBrowseInstanceList newServicesList;
    try
    {
        newServicesList = std::any_cast<BVServiceBrowseInstanceList>(*dp);    
    }
    catch(const std::bad_any_cast& e)
    {
        LogError("App: Bad cast in BVEventType::BVEVENTTYPE_APP_PUBLISHED_SERVICE callback.");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    // Update service vector.
    // Does it need to be guarded? I think so, because here we are modifying it.
    // Mock client will periodically read it, but real user in the product implementation
    // will try to read it and they might do it when this is updated here

    std::vector<BVServiceBrowseInstance> toResolve;
    LogTrace("App: HandlePublishedServices is called.");
    {
        std::lock_guard<std::mutex> l(this->serviceVectorMutex);
        for (auto& lElem : newServicesList)
        {
            if ((std::find(this->serviceV.begin(), this->serviceV.end(), lElem) == this->serviceV.end()))
            {
                const BVServiceData& thisMachineServiceData = GetThisMachineServiceData();
                // LogDebug("This machine: domain: {}", thisMachineServiceData.domain.c_str());
                // LogDebug("This machine: regtype: {}", thisMachineServiceData.regtype.c_str());
                // LogDebug("This machine: hostname: {}", thisMachineServiceData.hostname.c_str());
                // LogDebug("Found domain: {}", lElem.replyDomain.c_str());
                // LogDebug("Found regtype: {}", lElem.regType.c_str());
                // LogDebug("Found hostname/servicename: {}", lElem.serviceName.c_str());
                // if (lElem.regType == thisMachineServiceData.regtype &&
                //     lElem.serviceName == thisMachineServiceData.hostname &&
                //     lElem.replyDomain == thisMachineServiceData.domain)
                if (lElem.serviceName == thisMachineServiceData.hostname)
                {
                    continue; // do not resolve service on the same machine
                }
                this->serviceV.push_back(lElem);
                toResolve.push_back(lElem);
                LogTrace("App, HandlePublishedServices: Added {} to serviceV", lElem.serviceName);
                // Send request to resolve
                // Should we exchange messages here or just resolve 
                // in Discovery after browsing there?
                // And report once we have everything (browsing + resolved hostname)
                // First - let's try exchanging messages.
                // Also - if we do Resolution straight in the BVDiscovery,
                // we might do repeat it for the same service.
                // Maybe also take note in BVDiscovery
            }
        }
    }
    {
        for (auto& lElem : toResolve)
        {
            SendMessage(BVMessage(
                    BVEventType::BVEVENTTYPE_DISCOVERY_REQUEST_RESOLVE,
                        std::make_unique<std::any>(std::make_any<BVServiceBrowseInstance>(lElem))));
            LogTrace("App: Sending request to Discovery to resolve {}", lElem.serviceName);
        }
    }
    // this is called from different thread
    PrintAll();

    // Should we resolve here? Maybe just send a request to Discovery to resolve
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::HandleResolvedServices(std::unique_ptr<std::any> dp)
{
    BVStatus status = BVStatus::BVSTATUS_OK;
    LogTrace("App: HandleResolvedServices ENTER");
    if (dp == nullptr)
    {
        LogError("App: Error - HandleResolvedServices, data pointer is null!");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    DNSResolutionResult* res;
    try
    {
        res = std::any_cast<DNSResolutionResult*>(*dp);
    }
    catch(const std::bad_any_cast& e)
    {
        std::cerr << "Bad cast in BVEventType::BVEVENTTYPE_APP_DISCOVERY_SERVICE_RESOLVED callback. " 
                    << e.what() << std::endl;
        LogError("App: Bad cast in HandleResolvedServices! Error details: {}", e.what());
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }

    std::string hosttarget  = res->hosttarget;
    std::string serviceName = res->serviceName;
    int         port        = res->port;

    LogTrace("App: Resolved {} to hosttarget: {}", serviceName, hosttarget);
    LogTrace("App: on port {}", port);

    BVNode node = ResolveServiceToEndpoint(hosttarget, serviceName, port);
    if (node.serviceName == "ERROR")
    {
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }

    auto _nodesM = this->GetConnectionManager().GetNodesM();
    auto it = _nodesM.find(serviceName);
    if (it == _nodesM.end())
    {
        GetConnectionManager().AddNodeToNodesM(serviceName, node);
        LogTrace("App: Added node representing service {} to nodesM", serviceName);
    } else
    {
        LogWarn("App: Node representing service {} already present in nodesM", serviceName);
        ::free(res);
        return BVStatus::BVSTATUS_OK;
    }

    // Initiate connection (session)
    // Open socket.
    // Will this connection listen to anything that other endpoint says?
    // Will these connections be persistent?
    // Start with initiating connection to an endpoint

    status = this->GetConnectionManager().InitiateSessionWithNode(node);

    if (status == BVStatus::BVSTATUS_FATAL_ERROR)
    {
        LogError("Couldn't Initiate Session with a node! {}:{} [{}]", node.hostname, node.port, node.address.to_string());
    }

    // Very important, as we manually allocate DNSResolutionResult in C_ResolveReply!!!
    ::free(res);
    return status;
}

BVStatus BVApp_ConsoleClient::HandleServiceDeregistration(std::unique_ptr<std::any> dp)
{
    LogTrace("BVApp_ConsoleClient: HandleServiceDeregistration called");

    using BVServiceBrowseInstanceList = std::list<BVServiceBrowseInstance>;
    if (dp == nullptr)
    {
        LogError("App: No new services received.");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    BVServiceBrowseInstanceList newServicesList;
    try
    {
        newServicesList = std::any_cast<BVServiceBrowseInstanceList>(*dp);    
    }
    catch(const std::bad_any_cast& e)
    {
        LogError("App: Bad cast in BVEventType::BVEVENTTYPE_APP_PUBLISHED_SERVICE callback.");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    {
        std::lock_guard<std::mutex> l(serviceVectorMutex);
        for (auto& lElem : newServicesList)
        {
            const auto oldSize = serviceV.size();
            serviceV.erase(
                std::remove_if(serviceV.begin(),
                               serviceV.end(),
                               [&](const BVServiceBrowseInstance& s)
                               {
                                    return s.serviceName == lElem.serviceName;
                               }),
                               serviceV.end()
            );
            if (serviceV.size() < oldSize)
            {
                GetConnectionManager().GetNodesM().erase(lElem.serviceName);
                LogTrace("App, HandleServiceDeregistration: removed {}.", lElem.serviceName);
                this->GetConnectionManager().RemoveSession(lElem.serviceName);
            } else
            {
                LogWarn("App, HandleServiceDeregistration: {} not found in serviceV!", lElem.serviceName);
            }
            LogInfo("App, HandleServiceDeregistration: Currently: {} Services in serviceV:", serviceV.size());
            int idx = 1;
            for (const auto& s : serviceV)
            {
                LogInfo("{}: {}", idx, s.serviceName);
                idx++;
            }
        }
    }
    PrintAll();
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::HandleMessageIncoming(std::unique_ptr<std::any> dp)
{
    LogTrace("[BVApp_ConsoleClient]: Received HandleMessageIncoming");
    if (dp == nullptr)
    {
        LogError("App: Error - HandleResolvedServices, data pointer is null!");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    BVChatMessage res;
    try
    {
        res = std::any_cast<BVChatMessage>(*dp);
    }
    catch(const std::bad_any_cast& e)
    {
        std::cerr << "Bad cast in BVEventType::BVEVENTTYPE_APP_MESSAGE_INCOMING callback. "
                    << e.what() << std::endl;
        LogError("[BVApp_ConsoleClient]: Bad cast in HandleMessageIncoming! Error details: {}", e.what());
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }

    const std::string textData  = res.textData;  
    const std::string sender    = res.sender;
    const uint64_t    timestamp = res.timestamp;

    LogTrace("[BVApp_ConsoleClient]: Received message: {} from: {} at: {}",
        textData, sender, timestamp);
    {
        std::lock_guard<std::mutex> l(chatLogsMapMutex);
        try
        {
            chatLogsM.at(sender).AddMessage(res);
        }
        catch(const std::out_of_range& e)
        {
            BVChatMessageLog log{sender, res};
            chatLogsM.emplace(sender, log);
        }
    }

    // This handler runs on the mailbox thread. Hand the redraw to the io_context
    // so it happens on the UI thread, in order with keystrokes. If we are in the
    // chat with this sender it shows up in the log; otherwise we surface it as a
    // notification on whatever screen is currently up - so a message always
    // appears without the user pressing anything.
    boost::asio::post(GetIoContext(), [this, sender, textData]()
    {
        if (this->uiState == UIState::Chat && this->activeChatService == sender)
        {
            this->lastNotification.clear();
            RenderChat();
        }
        else
        {
            this->lastNotification = "New message from " + sender + ": " + textData;
            Render();
        }
    });
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::HandleFileTransferBegin(std::unique_ptr<std::any> dp)
{
    LogTrace("[BVApp_ConsoleClient]: Received HandleFileTransferBegin");
    if (dp == nullptr)
    {
        LogError("[BVApp_ConsoleClient]: Error - HandleFileTransferBegin, data pointer is null!");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    BVTCPFileData res;
    try
    {
        res = std::any_cast<BVTCPFileData>(*dp);
    }
    catch(const std::bad_any_cast& e)
    {
        std::cerr << "Bad cast in BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_BEGIN callback. "
                    << e.what() << std::endl;
        LogError("[BVApp_ConsoleClient]: Bad cast in HandleFileTransferBegin! Error details: {}", e.what());
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }

    const uint32_t    correlationKey = res.correlationKey;
    const uint32_t    csize          = res.csize;  
    const uint32_t    fsize          = res.fsize;
    const std::vector<char> fdata  = res.fdata; // service name and filename!
    const std::string fdataStr{fdata.begin(), fdata.end()};
    const std::string serviceName = fdataStr.substr(0, fdataStr.find('|'));
    const std::string fname       = fdataStr.substr(fdataStr.find("|")+1);

    // TODO: We have to save this somehow and correlate incoming chunks!
    // Maybe in BVTCPConnectionManager - find serviceName data and save file.
    // Or we just receive one file at a time...
    // We do not send the file name and service name in the other chunks...
    // Other chunks aren't able to be correlated with the file!!!!
    // Or - we send a key that is a correlation key (e.g. instead of chunkSize)
    // We correlate serviceName/filename with a 32 bit correlation key.
    // We save it in the BVFileTransferContext. (it can be the ftcid of the file transfer context!)
    // And we save it to the console client/BVTCPConnectionManager class as a map.
    // Then, we only have to lookup a correlation key in order to retrieve path.

    LogTrace("[BVApp_ConsoleClient]: File size: {} Chunk size: {} From: {} Name: {}",
        fsize, csize, serviceName, fname);

    const std::filesystem::path rootdir = std::filesystem::current_path();
    try
    {
        const std::filesystem::path dirpath  = rootdir / "data" / serviceName;
        const std::filesystem::path filepath = dirpath / fname;
        if (std::filesystem::is_directory(dirpath))
        {
            LogTrace("[BVApp_ConsoleClient]: Directory already exists: {}", dirpath.string());
        } else
        {
            if (std::filesystem::create_directories(rootdir / "data" / serviceName))
            {
                LogTrace("[BVApp_ConsoleClient]: Directory created at: ", rootdir.string());
                if (std::filesystem::is_regular_file(filepath))
                {
                    LogTrace("[BVApp_ConsoleClient]: File already exists: {}", filepath.string());
                } else
                {
                    std::ofstream incomingFile(filepath, std::ios::binary | std::ios::out);
                    LogTrace("[BVApp_ConsoleClient]: Created file at: {}", filepath.string());
                }
            } else
            {
                LogError("[BVApp_ConsoleClient]: Directory not created.");
                return BVStatus::BVSTATUS_FATAL_ERROR;
            }
        }
    }
    catch(const std::exception& e)
    {
        LogError("[BVApp_ConsoleClient]: Error while creating directory and/or file: {}", e.what());
        return BVStatus::BVSTATUS_NOK;
    }

    // Register serviceName and fileName at the correlationKey.
    fileTransferData[correlationKey] = std::make_tuple(serviceName, fname);
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::HandleFileChunkSent(std::unique_ptr<std::any> dp)
{
    LogTrace("[BVApp_ConsoleClient]: Received HandleFileChunkSent");
    if (dp == nullptr)
    {
        LogError("[BVApp_ConsoleClient]: Error - HandleFileChunkSent, data pointer is null!");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    BVTCPFileData res;
    try
    {
        res = std::any_cast<BVTCPFileData>(*dp);
    }
    catch(const std::bad_any_cast& e)
    {
        std::cerr << "Bad cast in BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_CHUNK_SENT callback. "
                    << e.what() << std::endl;
        LogError("[BVApp_ConsoleClient]: Bad cast in HandleFileChunkSent! Error details: {}", e.what());
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }

    const uint32_t    correlationKey = res.correlationKey;
    const uint32_t    csize          = res.csize;  
    const uint32_t    fsize          = res.fsize;
    const std::vector<char> fdata    = res.fdata; // service name and filename!
    const std::string fdataStr{fdata.begin(), fdata.end()};

    // Get servicename and fname:
    std::string serviceName;
    std::string fname;
    try
    {
        std::tuple<std::string, std::string> fdata_t = fileTransferData.at(correlationKey);
        serviceName = std::get<0>(fdata_t);
        fname       = std::get<1>(fdata_t);
    }
    catch(const std::out_of_range& ex)
    {
        LogError("Couldn't find the correlated data for this file transfer!");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    LogTrace("[BVApp_ConsoleClient]: File size: {} Chunk size: {} From: {} Name: {}",
        fsize, csize, serviceName, fname);

    const std::filesystem::path rootdir = std::filesystem::current_path();
    try
    {
        const std::filesystem::path dirpath  = rootdir / "data" / serviceName;
        const std::filesystem::path filepath = dirpath / fname;
        std::ofstream incomingFile(filepath, std::ios::binary | std::ios::app);
        if (!incomingFile)
        {
            LogError("Couldn't open an out stream for: {}", filepath.string());
            return BVStatus::BVSTATUS_FATAL_ERROR;
        }
        LogTrace("[BVApp_ConsoleClient]: Opened file to appending at: {}", filepath.string());
        incomingFile.write(fdata.data(), static_cast<std::streamsize>(fdata.size()));
    }
    catch(const std::exception& e)
    {
        LogError("[BVApp_ConsoleClient]: Error while writing to file: {}", e.what());
        return BVStatus::BVSTATUS_NOK;
    }
    std::cout << "File transmission in progress..." << std::endl;
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::HandleFileTransferEnd(std::unique_ptr<std::any> dp)
{
    LogTrace("[BVApp_ConsoleClient]: Received HandleFileTransferEnd");
    if (dp == nullptr)
    {
        LogError("[BVApp_ConsoleClient]: Error - HandleFileTransferEnd, data pointer is null!");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }
    BVTCPFileData res;
    try
    {
        res = std::any_cast<BVTCPFileData>(*dp);
    }
    catch(const std::bad_any_cast& e)
    {
        std::cerr << "Bad cast in BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_END callback. "
                    << e.what() << std::endl;
        LogError("[BVApp_ConsoleClient]: Bad cast in HandleFileTransferEnd! Error details: {}", e.what());
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }

    const uint32_t    correlationKey = res.correlationKey;
    const uint32_t    csize          = res.csize;  
    const uint32_t    fsize          = res.fsize;
    const std::vector<char> fdata    = res.fdata; // service name and filename!
    const std::string fdataStr{fdata.begin(), fdata.end()};

    // Get servicename and fname:
    std::string serviceName;
    std::string fname;
    try
    {
        std::tuple<std::string, std::string> fdata_t = fileTransferData.at(correlationKey);
        serviceName = std::get<0>(fdata_t);
        fname       = std::get<1>(fdata_t);
    }
    catch(const std::out_of_range& ex)
    {
        LogError("Couldn't find the correlated data for this file transfer!");
        return BVStatus::BVSTATUS_FATAL_ERROR;
    }

    LogTrace("[BVApp_ConsoleClient]: File size: {} Chunk size: {} From: {} Name: {}",
        fsize, csize, serviceName, fname);
        const std::filesystem::path rootdir = std::filesystem::current_path();
    try
    {
        const std::filesystem::path dirpath  = rootdir / "data" / serviceName;
        const std::filesystem::path filepath = dirpath / fname;
        std::ofstream incomingFile(filepath, std::ios::binary | std::ios::app);
        if (!incomingFile)
        {
            LogError("Couldn't open an out stream for: {}", filepath.string());
            return BVStatus::BVSTATUS_FATAL_ERROR;
        }
        LogTrace("[BVApp_ConsoleClient]: Opened file to appending at: {}", filepath.string());
        incomingFile.write(fdata.data(), static_cast<std::streamsize>(fdata.size()));
        incomingFile.close(); // flush before trimming

        // Chunks are fixed-size and the final one is zero-padded, so the file on
        // disk is rounded up to a multiple of the chunk size. Trim it back to the
        // exact size advertised in FILE_TRANSFER_BEGIN.
        std::error_code rec;
        std::filesystem::resize_file(filepath, fsize, rec);
        if (rec)
        {
            LogError("[BVApp_ConsoleClient]: Couldn't trim {} to {} bytes: {}",
                filepath.string(), fsize, rec.message());
        }
    }
    catch(const std::exception& e)
    {
        LogError("[BVApp_ConsoleClient]: Error while writing to file: {}", e.what());
        return BVStatus::BVSTATUS_NOK;
    }
    std::cout << "File saved: " << fname << " (" << fsize << " bytes)" << std::endl;

    // Delete data at correlationKey
    fileTransferData.erase(correlationKey);
    // GetConnectionManager -> remove fileTransferContext
    // But we have to send it to another host...
    // TODO: There should be 

    return BVStatus::BVSTATUS_OK;
}

void BVApp_ConsoleClient::PrintNewServicesNotification(void)
{
    std::lock_guard stdoutlk{this->stdoutMutex};
    std::cout << "New services received!" << std::endl;
}

BVStatus BVApp_ConsoleClient::OnStart(std::unique_ptr<std::any>)
{
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::OnResume(std::unique_ptr<std::any>)
{
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::OnShutdown(std::unique_ptr<std::any>)
{
    StopIOContext();
    LogTrace("App: Shutting down...");
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::OnRestart(std::unique_ptr<std::any>)
{
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVApp_ConsoleClient::OnPause(std::unique_ptr<std::any>)
{
    return BVStatus::BVSTATUS_OK;
}

std::optional<ParsingResult> BVApp_ConsoleClient::ParseConsoleActionFromKey
(char key)
{
    unsigned char ukey = static_cast<unsigned char>(key);
    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(key))))
    {
        case 'd':
            return ParsingResult{BVConsoleActionType::BVCONSOLEACTION_REPRINT, std::nullopt};
        // case 'm':
        //     return ParsingResult{BVConsoleActionType::BVCONSOLEACTION_SENDMSG, std::nullopt};
        case 'p':
            return ParsingResult{BVConsoleActionType::BVCONSOLEACTION_PAUSE_DISCOVERY, std::nullopt};
        case 'r':
            return ParsingResult{BVConsoleActionType::BVCONSOLEACTION_RESUME_DISCOVERY, std::nullopt};
        case 'q':
            return ParsingResult{BVConsoleActionType::BVCONSOLEACTION_QUIT, std::nullopt};
        case 'b':
            return ParsingResult{BVConsoleActionType::BVCONSOLEACTION_BLOCKHOST, std::nullopt};
        default:
            break;
    }
    if (std::isdigit(ukey))
    {
        return ParsingResult{BVConsoleActionType::BVCONSOLEACTION_SENDMSG, key - '0'};
    }
    return std::nullopt;
}

BVNode BVApp_ConsoleClient::ResolveServiceToEndpoint(const std::string& hosttarget, const std::string& serviceName, const int port)
{
    LogTrace("BVApp_ConsoleClient::ResolveServiceToEndpoint: Resolving host {} on port: {}", hosttarget, port);
    BVNode nodeData{};
    boost::system::error_code ec;
    boost::asio::ip::tcp::resolver resolver{GetIoContext()};
    
    boost::asio::ip::tcp::resolver::results_type results;
    for (int attempt = 0; attempt < 5; attempt++)
    {
        ec.clear();
        results = resolver.resolve(/*boost::asio::ip::tcp::v4()*/hosttarget, std::to_string(port), ec); // make that async
        if (!ec && !results.empty())
        {
            break;
        }
        LogWarn("App: Resolve attempt failed... Retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (ec && results.empty())
    {
        LogError("App: Error while resolving to... {}", ec.to_string());
        LogError("App: Error while resolving info {} {}", ec.message(), ec.category().name());
        nodeData.serviceName = "ERROR";
        return nodeData;
    }

    // auto results = resolver.resolve(/*boost::asio::ip::tcp::v4()*/hosttarget, std::to_string(port), ec); // make that async
    // TODO: There's a problem with this resolution!! Probably

    /* 
        This sometimes fail.
        We have to use DNSServiceGetAddrInfo...
        For now, this workaround is ok.
    */

    // if (ec)
    // {
    //     LogWarn("App: Error while resolving to... {}", ec.to_string());
    //     LogWarn("App: Error while resolving info {} {}", ec.message(), ec.category().name());
    // }
    // if (results.empty())
    // {
    //     LogError("App: Endpoints empty...");
    //     nodeData.serviceName = "ERROR";
    //     return nodeData;
    // }
    boost::asio::ip::tcp::endpoint endpoint = results.begin()->endpoint(); // try first endpoint
    LogTrace("Successfuly resolved {} to {}", serviceName, endpoint.address().to_string());
    nodeData.ep = endpoint;
    nodeData.address = endpoint.address();
    nodeData.hostname = hosttarget;
    nodeData.serviceName = serviceName; 
    nodeData.results = results;
    nodeData.port = port;
    return nodeData;
}

std::unique_ptr<BVTCPMessage<BVChatMessagePayload>> BVApp_ConsoleClient::ConstructChatMessageFromInput(
    const std::string& inputString)//, const NodeID nodeID)
{
    std::unique_ptr<BVTCPMessage<BVChatMessagePayload>> msg = 
        std::make_unique<BVTCPMessage<BVChatMessagePayload>>();
    std::chrono::milliseconds ts = 
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
    msg->header.timestamp = ts.count();
    msg->header.msgType = 
        static_cast<uint8_t>(BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_CHATMESSAGE);
    msg->payload.textData.fill('\0');

    const std::size_t maxLen = msg->payload.textData.size();
    const std::size_t copyLen = std::min(inputString.size(), maxLen);
    std::memcpy(msg->payload.textData.data(), inputString.data(), copyLen);

    msg->header.dataLen = static_cast<uint8_t>(copyLen);

    return msg;
}
