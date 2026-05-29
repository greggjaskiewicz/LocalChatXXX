#pragma once
#include <string>
#include <iostream>
#include <boost/asio.hpp>
#include "BV.hpp"
#include "BVLoggable.hpp"
#include "BVService.hpp"
#include "threadsafequeue.hpp"
#include "BVMessage.hpp"
#include "BVTCPCommon.hpp"
#include "BVTCPSession.hpp"
#include "BVFileTransferContext.hpp"
#include <arpa/inet.h>
#include <map>
#include <vector>
#include <mutex>
#include <functional>

// // These are messages sent between hosts
// typedef enum class BVTCPMessageType
// {
//     BVTCPMESSAGETYPE_HANDSHAKE,
//     BVTCPMESSAGETYPE_DEREGISTRATION, // ?
//     BVTCPMESSAGETYPE_TEXT_MESSAGE,
//     BVTCPMESSAGETYPE_FILE,

// } BVTCPMessageType;

/*
 * BVTCPConnectionManager
 * As part of the BVTCP Suite.
 * This class manages all connections - these coming in (we accept them)
 * and those that we initiate (we connect to other).
 *
 *
*/

// Remember to open a different file to log the connections logs. <- needed?
class BVTCPConnectionManager : public BVLoggable // BVComponent?
{
private:
    SessionID currentSessionID = 0;

    // This machine is this Node in the network.
    const BVServiceData thisMachineServiceData;
    BVNode              thisMachineHostData;
    boost::asio::io_context& ioContext;

    boost::asio::ip::tcp::acceptor acceptorSocket;

    // Map of active connections to other Nodes.
    // Session can be created in one thread,
    // But removed in the other.
    std::map<SessionID, std::shared_ptr<BVTCPSession>> sessions_m;
    std::mutex session_m_mutex;

    // Incoming messages from sessions are put right into App's inMailbox_p
    MailboxGetter mailbox_F;

    // Outwards communication queue with App.
    // App just pushes to one of these, doesn't listen to them.
    // BVMessage is also used here as a payload.
    // SessionID, not nodeID as a key
    // TODO: Not needed, remove
    std::map<NodeID, std::shared_ptr<threadsafe_queue<BVMessage>>> outMailboxes_p;

    std::map<std::string, SessionID> service_sessionid_m;
    std::map<std::string, BVNode> nodesM;

    // std::map<std::string, NodeID> service_nodeid_m;
    // maybe share all BVNodes with app, and app updates them?

    // We have to also instantiate some object that will tie service with a nodeID.
    // or at least - provide an interface to App, so that it can just push message to
    // a certain service/node and be done with it.

    // File transfer context map.
    // It means that only one file per session can be sent at the same time
    std::map<uint32_t, std::unique_ptr<BVFileTransferContext>> fileTransferContext_m;

    // Offers sent but not yet accepted/rejected by the peer. The actual transfer
    // context is only created once the peer accepts (key -> session + file).
    std::map<uint32_t, std::pair<SessionID, std::filesystem::path>> pendingOutgoing_m;

    // Thread-safe progress slot for the current outgoing transfer, fed by the
    // file-transfer context's progress callback and read by the GUI.
    std::mutex outProgressMutex;
    std::string outProgressName;
    std::uint32_t outProgressSent{0};
    std::uint32_t outProgressTotal{0};

    NodeID GetNodeIDByServiceName(const std::string& _serviceName, BVStatus& status_out)
    {
        NodeID id = 0;
        BVStatus status = BVStatus::BVSTATUS_NOK;
        for (auto const& [k, v] : sessions_m)
        {
            if (_serviceName == v->GetSessionData()->nodeData.serviceName)
            {
                id = k;
                status = BVStatus::BVSTATUS_OK;
                break;
            }
        }
        status_out = status;
        return id;
    }

public:
    BVTCPConnectionManager(boost::asio::io_context& _ioContext,
                           const BVServiceData _thisMachineServiceData);

    // Upon construction of objects other than for test purposes, instantiate acceptor socket and initialize connection.
    BVStatus StartAcceptingConnections(void);

    /*
        Establish a Node's communication channel with app and the manager
    */
    BVStatus InitiateSessionWithNode(const BVNode nodeData);

    // TODO: Ok. It is then changed
    // in BVBroker.
    // This is no longer valid after Broker attaching!
    // void SetAppInMailBoxP(std::shared_ptr<threadsafe_queue<BVMessage>> p)
    // {
    //     this->appInMailBox_p = p;
    // }

    // Set communication channel towards the Node. (their inMailbox_p)
    // TODO: Rename it to set mailbox p or something - this does not start a communication session!
    BVStatus StartCommunicationSessionWithNode(NodeID& nid, std::shared_ptr<threadsafe_queue<BVMessage>>& nodeInMailbox_p)
    {
        static NodeID current_id = 0;
        bool foundEmpty = false;
        BVStatus status = BVStatus::BVSTATUS_OK;
        for (NodeID _id = 0; _id < N_SERVICES_MAX; _id++)
        {
            if (outMailboxes_p.at(_id) == nullptr) // find any empty spot
            {
                nodeInMailbox_p = std::make_shared<threadsafe_queue<BVMessage>>();
                foundEmpty = true;
                break;
            }
        }
        if (!foundEmpty)
        {
            // there are more communication channels trying to be registered, and there is no place.
            status = BVStatus::BVSTATUS_NOK;
        }
        nid = current_id;
        current_id = (current_id % N_SERVICES_MAX) + 1;
        return status;
    }

    void PrintSessions(void)
    {
        {
            std::lock_guard<std::mutex> l(session_m_mutex);
            if (this->sessions_m.size() == 0) std::cout << "None." << std::endl;
            int menuNumber = 1; // 1-based selection number, see GetSessionServiceNamesInDisplayOrder()
            for (const auto& [k,v] : this->sessions_m)
            {
                std::cout << "  (" << menuNumber++ << ")  "
                        << v->GetSessionData()->nodeData.serviceName
                        << "   [session " << v->GetSessionID()
                        << ", node " << static_cast<uint16_t>(v->GetSessionData()->nodeData.id)
                        << "]" << std::endl;
            }
        }
    }

    bool IsSessionAlreadyPresent(const BVNode& nodeData)
    {
        bool found = false;
        {
            std::lock_guard<std::mutex> l(session_m_mutex);
            for (const auto& [k,v] : this->sessions_m)
            {
                if (v->GetSessionData()->nodeData.serviceName ==
                    nodeData.serviceName)
                {
                    found = true;
                    break;
                }
            }
        }
        return found;
    }

    bool IsSessionAlreadyPresent(const std::string& _serviceName)
    {
        bool found = false;
        {
            std::lock_guard<std::mutex> l(session_m_mutex);
            for (const auto& [k,v] : this->sessions_m)
            {
                if (v->GetSessionData()->nodeData.serviceName == _serviceName)
                {
                    found = true;
                    break;
                }
            }
        }
        return found;
    }

    // Send data to chosen node. This is an interface for App
    template<typename PayloadType>
    BVStatus SendDataToNode(std::unique_ptr<BVTCPMessage<PayloadType>> msg,
                            const SessionID& sid) // pass only service or NodeID.
    {
        BVStatus status = BVStatus::BVSTATUS_NOK;
        try
        {
            std::lock_guard<std::mutex> l(session_m_mutex);
            sessions_m.at(sid)->WriteMessageFrame(std::move(msg));
            status = BVStatus::BVSTATUS_OK;
        }
        catch(const std::out_of_range& ex)
        {
            LogError("No session for SessionID {}", sid);
            status = BVStatus::BVSTATUS_FATAL_ERROR;
        }
        return status;
    }

    template<typename PayloadType>
    BVStatus SendDataToEveryone(const BVTCPMessage<PayloadType> msg)
    {
        // try? what if it fails?
        {
            std::lock_guard<std::mutex> l(session_m_mutex);
            for (auto& [k, v] : sessions_m)
            {
                v->WriteMessageFrame(msg);
            }
        }
        return BVStatus::BVSTATUS_OK;
    }

    void PutMessageIntoAppMailbox(const BVEventType& type, std::unique_ptr<std::any> dp)
    {
        // this->appInMailBox_p->push(BVMessage(type, std::move(dp)));
    }

    void PutMessageIntoAppMailbox(BVMessage&& msg)
    {
        // this->appInMailBox_p->push(std::move(msg));
        this->mailbox_F()->push(std::move(msg));
    }

    // // Would be better to assign an ID to a session
    // // And then remove session by ID
    // BVStatus RemoveSession(std::shared_ptr<BVTCPSession> sp)
    // {

    //     return BVStatus::BVSTATUS_OK;
    // }

    // BVStatus RemoveSession(const SessionID& sid)
    // {
    //     std::lock_guard<std::mutex> l(session_m_mutex);
    //     std::size_t n_erased = sessions_m.erase(sid);
    //     if (n_erased == 0) return BVStatus::BVSTATUS_NOK;
    //     LogTrace("BVTCPConnectionManager: Removed session with SessionID: {}", sid);
    //     return BVStatus::BVSTATUS_OK;
    // }

    BVStatus RemoveSession(const std::string& _serviceName)
    {
        std::lock_guard<std::mutex> l(session_m_mutex);
        auto sidIt = service_sessionid_m.find(_serviceName);
        if (sidIt == service_sessionid_m.end())
        {
            LogError("BVTCPConnectionManager: No session mapping for {}", _serviceName);
            return BVStatus::BVSTATUS_NOK;
        }

        const SessionID id = sidIt->second;
        auto sessIt = sessions_m.find(id);
        if (sessIt == sessions_m.end())
        {
            LogError("BVTCPConnectionManager: Stale mapping for {} -> session {}",
                    _serviceName, id);
            service_sessionid_m.erase(sidIt);
            return BVStatus::BVSTATUS_NOK;
        }
        sessions_m.erase(sessIt);
        service_sessionid_m.erase(sidIt);
        LogTrace("BVTCPConnectionManager: Removed session {} for {}",
                id, _serviceName);

        return BVStatus::BVSTATUS_OK;
    }

    void ConnectHandler(const boost::system::error_code& error,
                        const boost::asio::ip::tcp::endpoint ep,
                        std::shared_ptr<BVTCPNodeConnectionSessionData> sessionData_p)
    {
        if (error)
        {
            LogError("ConnectHandler Error: {} {} {}", error.value(), error.message(), error.category().name());
            return;
        }
        if (this->IsSessionAlreadyPresent(sessionData_p->nodeData))
        {
            LogInfo("ConnectHandler: Session associated with service {} already present.", sessionData_p->nodeData.serviceName);
            return;
        }
        {
            // We probably provide not the host machine, but the service name of the session that we are connecting to.
            // On the other machine it will be their name
            std::lock_guard<std::mutex> l(session_m_mutex);
            sessionData_p->nodeData.ep = ep;
            std::shared_ptr<BVTCPSession> session_p = std::make_shared<BVTCPSession>(sessionData_p, ioContext);
            session_p->SetLogger(GetLogger());
            session_p->SetManager_p(this);
            StartCommunicationSessionWithNode(session_p->GetSessionData()->nodeData.id, session_p->GetSessionData()->inMailbox_p);
            // session_p->SetState(BVSessionState::BVSESSIONSTATE_ESTABLISHED);
            // The accepting node will decide if this is a non-duplicate connection.
            // TODO: Send message os that this connection if it's ok, changes its state
            session_p->SetState(BVSessionState::BVSESSIONSTATE_UNPREPARED);
            session_p->SetOrigin(BVSessionOrigin::BVSESSIONORIGIN_OUTGOING);
            sessions_m[session_p->GetSessionData()->sessionID] = session_p;
            service_sessionid_m[sessionData_p->nodeData.serviceName] = session_p->GetSessionID();
            LogTrace("ConnectHandler: Successfuly connected to {}: {}:{} SessionID: {}",
                sessionData_p->nodeData.serviceName, sessionData_p->nodeData.ep.address().to_string(),
                    sessionData_p->nodeData.ep.port(), sessionData_p->sessionID);
            session_p->RequestReadingFrames();
            LogTrace("ConnectHandler: Current Sessions:");
            for (const auto& [k,v] : sessions_m)
            {
                LogTrace("ConnectHandler: ServiceName: {} Session ID: {}",
                   v->GetSessionData()->nodeData.serviceName, v->GetSessionID());
            }
        }
    }

    void Accept(void)
    {
        std::shared_ptr<BVTCPNodeConnectionSessionData> sessionData_p;
        {
            std::lock_guard<std::mutex> l(session_m_mutex);
            sessionData_p = std::make_shared<BVTCPNodeConnectionSessionData>(BVNode{}, ioContext, currentSessionID, thisMachineHostData.serviceName);
            currentSessionID+=1;
            // sessionData_p->appCommChannel_p = this->appInMailBox_p;
        }
        // we pass the socket of this session
        this->acceptorSocket.async_accept(*sessionData_p->sock.get(),
            [sessionData_p, this](const boost::system::error_code& error){
                if (!error)
                {
                    // Wait - is there already a connection session with this peer/node?
                    // Create a connection but not add it yet to the map.
                    std::shared_ptr<BVTCPSession> session_p =
                        std::make_shared<BVTCPSession>(sessionData_p, this->ioContext);
                    session_p->SetLogger(GetLogger());
                    session_p->SetManager_p(this);

                    // session_p now identifies socket with that socket.
                    this->LogTrace("Accept successful. Requesting identification from the peer.");
                    // Construct message
                    BVTCPMessageHeader header = ConstructMessageHeader(BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_HELLO);
                    BVTCPMessage<std::array<char, 128>> helloMsg = ConstructMessage(header, std::array<char,128>()); // empty payload
                    session_p->SetState(BVSessionState::BVSESSIONSTATE_UNPREPARED);
                    session_p->WriteMessageFrame(helloMsg);
                    session_p->SetOrigin(BVSessionOrigin::BVSESSIONORIGIN_INGOING);
                    session_p->RequestReadingFrames();
                    Accept();
                } else
                {
                    this->LogError("Accept failed.");
                }
        });
    }

    void HandleSessionIdentification(const std::string& serviceName, std::shared_ptr<BVTCPSession> caller)
    {
        if (!IsSessionAlreadyPresent(serviceName))
        {
            // This session is not a duplicate - we accepted it, it wasn't added (we didn't know who it was)
            // Now we know - we can add it, didn't connect to it before.
            // We need to also send a message so that the other side knows
            // that they're ok and can change their state to BVSESSIONSTATE_ESTABLISHED
            caller->SetState(BVSessionState::BVSESSIONSTATE_ESTABLISHED);
            {
                std::lock_guard<std::mutex> l(session_m_mutex);
                // Set nodeData - this is not set when accepting!
                StartCommunicationSessionWithNode(caller->GetSessionData()->nodeData.id, caller->GetSessionData()->inMailbox_p);
                caller->GetSessionData()->nodeData.serviceName = serviceName;
                this->sessions_m[caller->GetSessionData()->sessionID] = caller;
                // We do not need an IP address of this service! We already have the socket!
                caller->GetSessionData()->nodeData.address = caller->GetSessionData()->sock->remote_endpoint().address();
                caller->GetSessionData()->nodeData.ep = caller->GetSessionData()->sock->remote_endpoint();
                service_sessionid_m[serviceName] = caller->GetSessionID();
                AddNodeToNodesM(serviceName, caller->GetSessionData()->nodeData);
                LogTrace("BVTCPConnectionManager: Sending BVSESSIONCONTROLMESSAGETYPE_CONFIRM_ESTABLISHED...");
                BVTCPMessageHeader header = ConstructMessageHeader(BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_CONFIRM_ESTABLISHED);
                BVTCPMessage<std::array<char, 128>> confirmEstablishedMessage = ConstructMessage(header, std::array<char,128>()); // empty payload
                caller->WriteMessageFrame(confirmEstablishedMessage);
            }
            LogTrace("BVTCPConnectionManager: Established connection with node: {} Address: {}",
                caller->GetSessionData()->nodeData.serviceName, caller->GetSessionData()->nodeData.address.to_string());

            // When we now have serviceName, we have to get the ip address with that service name
            // Although that's weird, because we should already have their IP.
            // We have their IP when we connect, but when we accept, we do not.
            // We should get that IP From app, or the node should send it themselves.
            // The session with which we talk, is the session to US.
            LogTrace("BVTCPConnectionManager: Current sessions:");
            {
                std::lock_guard<std::mutex> l(session_m_mutex);
                int sidx = 0;
                for (const auto& [k,v] : this->sessions_m)
                {
                    LogTrace("Session {} : ID: {}, service: {}", sidx, v->GetSessionData()->sessionID, v->GetSessionData()->nodeData.serviceName);
                    sidx++;
                }
            }
            caller->RequestReadingFrames();
        } else
        {
            // This session is a duplicate - we might've accepted and connected at the same time.
            // Now we know who it is; we have this peer as a session, so we can close this one.
            LogTrace("BVTCPConnectionManager: Found duplicate session for {}. Closing.", caller->GetSessionData()->nodeData.serviceName);
            caller->Close(); // close the duplicate session
        }
    }

    BVStatus GetSessionIDFromServiceName(const std::string& _s, SessionID& sid_out)
    {
        try
        {
            sid_out = service_sessionid_m.at(_s);
        }
        catch(const std::out_of_range& ex)
        {
            return BVStatus::BVSTATUS_NOK;
        }
        return BVStatus::BVSTATUS_OK;
    }

    // std::map<std::string, BVNode> nodesM;
    BVStatus AddNodeToNodesM(const std::string& _s, const BVNode _n)
    {
        try
        {
            nodesM.at(_s);
        }
        catch(const std::out_of_range& ex)
        {
            // node not present
            nodesM.emplace(_s, _n);
            return BVStatus::BVSTATUS_OK;
        }
        return BVStatus::BVSTATUS_NOK;
    }

    std::map<std::string, BVNode>& GetNodesM(void)
    {
        return this->nodesM;
    }

    // Service names of established sessions, in the same order PrintSessions
    // lists them. The menu shows a 1-based number per session, so menu number N
    // maps to index N-1 here. Both iterate sessions_m, which is ordered, so the
    // numbering stays consistent between display and selection.
    std::vector<std::string> GetSessionServiceNamesInDisplayOrder(void)
    {
        std::vector<std::string> names;
        std::lock_guard<std::mutex> l(session_m_mutex);
        names.reserve(sessions_m.size());
        for (const auto& [k, v] : sessions_m)
        {
            names.push_back(v->GetSessionData()->nodeData.serviceName);
        }
        return names;
    }

    void SetMailboxGetterF(MailboxGetter f)
    {
        this->mailbox_F = std::move(f);
    }
    
    // File utilities

    // BVFileTransferContext    
    // Progress of the current outgoing transfer for the UI, e.g.
    // "Sending x.png… 45%". Empty when nothing is actively sending.
    std::string CurrentOutgoingTransferStatus(void)
    {
        std::lock_guard<std::mutex> l(outProgressMutex);
        if (outProgressTotal == 0 || outProgressSent >= outProgressTotal)
        {
            return std::string{};
        }
        const std::uint32_t pct = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(outProgressSent) * 100) / outProgressTotal);
        return "Sending " + outProgressName + "\xE2\x80\xA6 " + std::to_string(pct) + "%";
    }

    // Step 1 of a send: offer the file and remember it as pending. The transfer
    // only really starts (BEGIN + chunks) once the peer sends FILE_ACCEPT, which
    // lands in SignalFileTransfer(). On FILE_REJECT the pending entry is dropped.
    BVStatus InitiateFileTransferWithSession(const SessionID& sid,
                                             std::filesystem::path& _fpath)
    {
        static uint32_t ftcid = 0;
        const uint32_t key = ftcid++;
        std::error_code ec;
        const uint32_t fsize = static_cast<uint32_t>(std::filesystem::file_size(_fpath, ec));
        const std::string name = _fpath.filename().string();

        pendingOutgoing_m[key] = std::make_pair(sid, _fpath);
        sessions_m.at(sid)->SendFileOffer(key, fsize, name);
        LogTrace("[BVTCPConnectionManager]: Sent FILE_OFFER key={} '{}' ({} bytes) on session {}",
                 key, name, fsize, sid);
        return BVStatus::BVSTATUS_OK;
    }

    // Peer's verdict on one of our offers. Accept -> build the real transfer;
    // reject -> forget it (nothing leaves this machine).
    void SignalFileTransfer(uint32_t correlationKey, bool accept)
    {
        auto it = pendingOutgoing_m.find(correlationKey);
        if (it == pendingOutgoing_m.end())
        {
            LogWarn("[BVTCPConnectionManager]: SignalFileTransfer: no pending offer key={}", correlationKey);
            return;
        }
        const SessionID sid = it->second.first;
        std::filesystem::path fpath = it->second.second;
        pendingOutgoing_m.erase(it);

        if (!accept)
        {
            LogTrace("[BVTCPConnectionManager]: Offer key={} rejected by peer; not sending.", correlationKey);
            return;
        }
        std::unique_ptr<BVFileTransferContext> ftcp =
            std::make_unique<BVFileTransferContext>(sessions_m.at(sid), fpath, correlationKey, mailbox_F,
                [this](std::uint32_t sent, std::uint32_t total, const std::string& nm)
                {
                    std::lock_guard<std::mutex> l(outProgressMutex);
                    outProgressName  = nm;
                    outProgressSent  = sent;
                    outProgressTotal = total;
                });
        ftcp->SetLogger(GetLogger());
        RemoveFileTransferContext(correlationKey);
        fileTransferContext_m.emplace(correlationKey, std::move(ftcp));
        LogTrace("[BVTCPConnectionManager]: Offer key={} accepted; starting transfer.", correlationKey);
    }

    // Receiver side: reply to an offer (and tell the sender to stream or stop).
    void RespondToFileOffer(const std::string& serviceName, uint32_t correlationKey, bool accept)
    {
        SessionID sid;
        if (GetSessionIDFromServiceName(serviceName, sid) != BVStatus::BVSTATUS_OK)
        {
            LogWarn("[BVTCPConnectionManager]: RespondToFileOffer: no session for {}", serviceName);
            return;
        }
        std::lock_guard<std::mutex> l(session_m_mutex);
        auto it = sessions_m.find(sid);
        if (it == sessions_m.end())
        {
            return;
        }
        it->second->SendFileControl(correlationKey,
            accept ? BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_ACCEPT
                   : BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_REJECT);
    }

    // Remove after file transfer ended.
    BVStatus RemoveFileTransferContext(const uint32_t correlationKey)
    {
        try
        {
            fileTransferContext_m.erase(correlationKey);
            LogTrace("Removed file transfer context (session) associated with correlation key: {}", correlationKey);
        }
        catch(const std::out_of_range& ex)
        {
            LogError("Couldn't find the correlated data for this file transfer!");
            return BVStatus::BVSTATUS_NOK;
        }
        return BVStatus::BVSTATUS_OK;
    }

    ~BVTCPConnectionManager();
};
