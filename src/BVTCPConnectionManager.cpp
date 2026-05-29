#include "BVTCPConnectionManager.hpp"

BVTCPConnectionManager::BVTCPConnectionManager(boost::asio::io_context& _ioContext,
                                               const BVServiceData _thisMachineServiceData):
ioContext(_ioContext),
thisMachineServiceData(_thisMachineServiceData),
acceptorSocket(_ioContext)
{
    for (NodeID _id = 0; _id < N_SERVICES_MAX; _id++)
    {
        this->outMailboxes_p.emplace(std::make_pair(_id, nullptr));
    }

    // boost::asio::

    // TODO: set host data - service name and hostname
    // thisMachineHostData.
    // downcast is ok.
    boost::asio::ip::tcp::resolver resolver{ioContext};
    boost::system::error_code ec;
    auto results = resolver.resolve(thisMachineServiceData.hostname, std::to_string(thisMachineServiceData.port), ec);
    thisMachineHostData = BVNode{ thisMachineServiceData.hostname,
                                  thisMachineServiceData.hostname,
                                  ntohs(thisMachineServiceData.port),
                                  results };

    if (ec)
    {
        LogError("BVTCPConnectionManager: Couldn't resolve {} {}", thisMachineServiceData.hostname.c_str(), ec.value());
#ifdef RESOLUTION_POLICY_HARD_FAIL
        throw std::runtime_error("Couldn't resolve this machine service.");
#endif
    } else
    {

    }

    // start listening
    // on what port?
    // on port 50001 (we announce our service on this port). And this is the port of the actual service,
    // not part of mDNS.
}

// Initiate a Client connection with a Node
BVStatus BVTCPConnectionManager::InitiateSessionWithNode(const BVNode nodeData)
{
    // Glare avoidance: with mutual discovery, both peers would dial each other at
    // the same instant and each then close the other's inbound as a "duplicate",
    // leaving two half-dead sockets (chat/file silently lost). Make only the
    // lexicographically-smaller service name dial out; the larger one waits to
    // accept. Both ends evaluate the same total ordering, so exactly one
    // connection is created regardless of who discovers whom first.
    if (!(thisMachineHostData.serviceName < nodeData.serviceName))
    {
        LogTrace("InitiateSessionWithNode: deferring outbound to {} (peer dials; glare avoidance)",
                 nodeData.serviceName);
        return BVStatus::BVSTATUS_OK;
    }
    // Wait - is there already a connection with this peer/node?
    if (IsSessionAlreadyPresent(nodeData))
    {
        LogTrace("Session for {} already present (probably we accepted it).", nodeData.serviceName);
        return BVStatus::BVSTATUS_OK;
    }
    std::shared_ptr<BVTCPNodeConnectionSessionData> sessionData_p;
    {
        std::lock_guard<std::mutex> l(session_m_mutex);
        sessionData_p = std::make_shared<BVTCPNodeConnectionSessionData>(
            nodeData, ioContext, currentSessionID, thisMachineHostData.serviceName);
        currentSessionID+=1;
    }
    // sessionData_p->appCommChannel_p = this->appInMailBox_p;
    // is NodeID really assigned here?
    BVStatus registerStatus =
        StartCommunicationSessionWithNode(sessionData_p->nodeData.id, sessionData_p->inMailbox_p);
    if (registerStatus == BVStatus::BVSTATUS_NOK)
    {
        LogInfo("BVTCPConnectionManager::InitiateSessionWithNode: Couldn't register communication channel");
        return registerStatus;
    }

    boost::asio::async_connect(*sessionData_p->sock, sessionData_p->nodeData.results,
        std::bind(&BVTCPConnectionManager::ConnectHandler, this, std::placeholders::_1, std::placeholders::_2, sessionData_p));
    LogTrace("App: Trying to connect asynchronously with service: {}. Trying to create Session ID: {}",
        sessionData_p->nodeData.serviceName, sessionData_p->sessionID);
    /*
        In P2P connection, we do not create two sessions - one for incoming traffic and one for outgoing.
        Instead, we try to initiate/try to connect: if the peer (service) has already connected to US (we accepted),
        we use this session with its socket. If the peer (service) has already accepted our connection trial (we connected),
        then we use this session and socket. WE DO NOT create one listening session and one writing session - this causes a collision.
        So one session handles EVERY and ANY traffic between two peers/nodes.
        And certainly, we cannot create two sessions simultaneously.
        TCP connection is already bidirectional.
    */
    return BVStatus::BVSTATUS_OK;
}

BVStatus BVTCPConnectionManager::StartAcceptingConnections(void)
{
    // We have to have the same local IP address that will be resolved.
    // synchronousously initialize an acceptor socket

    // get endpoint

    // Resolve first???
    // boost::asio::ip::tcp::resolver resolver{ioContext};
    // auto results = resolver.resolve(/*boost::asio::ip::tcp::v4()*/this->thisMachineServiceData.hostname, std::to_string(ntohs(thisMachineServiceData.port)), ec); // make that async

    boost::system::error_code ec;
    boost::asio::ip::tcp::endpoint ep{boost::asio::ip::address_v6::any(), ntohs(thisMachineServiceData.port)};
    this->acceptorSocket = boost::asio::ip::tcp::acceptor{ioContext};

    if (this->acceptorSocket.is_open())
    {
        this->acceptorSocket.close(ec);
        if (ec)
        {
            LogError("BVTCPConnectionManager: Couldn't close the acceptor socket. {} - {}", ec.value(), ec.message());
            throw std::runtime_error("Couldn't close the acceptor socket.");
        }
    }
    this->acceptorSocket.open(ep.protocol(), ec);
    if (ec)
    {
        LogError("BVTCPConnectionManager: Couldn't open the acceptor socket. {} - {}", ec.value(), ec.message());
        throw std::runtime_error("Couldn't open the acceptor socket.");
    }
    this->acceptorSocket.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec)
    {
        LogError("BVTCPConnectionManager: Couldn't set option 'reuse_address' for the acceptor socket. {} - {}", ec.value(), ec.message());
        throw std::runtime_error("Couldn't set option for the acceptor socket.");
    }
    this->acceptorSocket.set_option(boost::asio::ip::v6_only{false}, ec);
    if (ec)
    {
        LogError("BVTCPConnectionManager: Couldn't set option 'v6_only' for the acceptor socket. {} - {}", ec.value(), ec.message());
        throw std::runtime_error("Couldn't set option for the acceptor socket.");
    }
    this->acceptorSocket.bind(ep, ec);
    if (ec)
    {
        LogError("BVTCPConnectionManager: Couldn't bind the acceptor socket. {} - {}", ec.value(), ec.message());
        throw std::runtime_error("Couldn't bind the acceptor socket.");
    }
    LogTrace("BVTCPConnectionManager: Accepting connections on {}:{}... for service: {}",

        ep.address().to_string(), ep.port(), thisMachineServiceData.hostname);
    this->acceptorSocket.listen(N_SERVICES_MAX, ec);
    if (ec)
    {
        LogError("BVTCPConnectionManager: Acceptor couldn't perform listening: {} - {}", ec.value(), ec.message());
        throw std::runtime_error("Acceptor couldn't perform listening");
    }
    Accept();
    // std::shared_ptr<BVTCPNodeConnectionSessionData> sessionData_p =
    //     std::make_shared<BVTCPNodeConnectionSessionData>(BVNode{}, ioContext, currentSessionID, thisMachineHostData.serviceName);
    // sessionData_p->appCommChannel_p = this->appInMailBox_p;

    // // we pass the socket of this session
    // this->acceptorSocket.async_accept(*sessionData_p->sock.get(),
    //     [sessionData_p, this](const boost::system::error_code& error){
    //         if (!error)
    //         {
    //             // Wait - is there already a connection session with this peer/node?
    //             // Create a connection but not add it yet to the map.
    //             std::shared_ptr<BVTCPSession> session_p =
    //                 std::make_shared<BVTCPSession>(sessionData_p, this->ioContext);
    //             session_p->SetLogger(GetLogger());
    //             session_p->SetManager_p(this);

    //             // session_p now identifies socket with that socket.
    //             this->LogTrace("Accept successful. Requesting identification from the peer.");
    //             // session_p->
    //             // Construct message
    //             BVTCPMessageHeader header = ConstructMessageHeader(BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_HELLO);
    //             BVTCPMessage<std::array<char, 128>> helloMsg = ConstructMessage(header, std::array<char,128>()); // empty payload
    //             session_p->SetState(BVSessionState::BVSESSIONSTATE_UNPREPARED);
    //             session_p->WriteMessageFrame(helloMsg);
    //             session_p->SetOrigin(BVSessionOrigin::BVSESSIONORIGIN_INGOING);
    //             session_p->RequestReadingFrames();

    //             // TODO: Await async connections again!!!!

    //         } else
    //         {
    //             this->LogError("Accept failed.");
    //         }
    // });
    return BVStatus::BVSTATUS_OK;
}

BVTCPConnectionManager::~BVTCPConnectionManager()
{
    this->acceptorSocket.cancel();
}
