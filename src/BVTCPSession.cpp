#include "BVTCPSession.hpp"
#include "BVTCPConnectionManager.hpp" // to call manager's functions

BVTCPSession::BVTCPSession(std::shared_ptr<BVTCPNodeConnectionSessionData> _sessionData_p,
                           boost::asio::io_context& _ioContext) :
sessionData_p(_sessionData_p),
ioContext(_ioContext)
{
    this->sessionData_p->alive = true;
    this->sessionData_p->readBuf = std::make_unique<char[]>(MESSAGE_FRAME_SIZE_BYTES);

    constexpr uint8_t ONE_BYTE = 1;
    this->sessionData_p->fileReadBuf = std::make_unique<char[]>(ONE_BYTE);
    std::memset(this->sessionData_p->fileReadBuf.get(), 0, ONE_BYTE);

    this->ClearReadBuffer();
    this->ClearWriteBuffer();
}

void BVTCPSession::Start(void)
{

}

void BVTCPSession::Shutdown(void)
{

}

void BVTCPSession::Read(void)
{


}

void BVTCPSession::WriteSome(void)
{
    this->sessionData_p->sock->async_write_some(
        boost::asio::buffer(this->sessionData_p->writeBuf),
        std::bind(&BVTCPSession::WriteSomeCallback, this, std::placeholders::_1, std::placeholders::_2)
    );
}

void BVTCPSession::RequestSomeWrite(const std::string& data)
{
    this->sessionData_p->writeBuf = data;
    this->WriteSome();
}

void BVTCPSession::OnReceiveHelloFrame(void)
{
    using CharPayload128B = std::array<char, 128>;
    using HelloMsg = BVTCPMessage<CharPayload128B>;
    const auto* msg = reinterpret_cast<const HelloMsg*>(this->sessionData_p->readBuf.get());

    if (msg->header.msgType != static_cast<uint8_t>(BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_HELLO))
    {
        LogError("Session [{}]: OnReceiveHelloFrame called for wrong msgType={}", this->GetSessionData()->sessionID,
            static_cast<int>(msg->header.msgType));
        return;
    }

    LogTrace("Session [{}]: Received BVSESSIONCONTROLMESSAGETYPE_HELLO. Sending _HELLOBACK",
        this->GetSessionData()->sessionID);

    // This is sending text data (from string) -> maybe put into function
    const std::string& serviceNameToCopy = this->sessionData_p->thisMachineServiceName;
    CharPayload128B payloadRaw;
    std::copy(serviceNameToCopy.begin(), serviceNameToCopy.end(), payloadRaw.data());
    BVTCPMessageHeader replyHeader = ConstructMessageHeader(BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_HELLOBACK);
    BVTCPMessage<CharPayload128B> replyMsg = ConstructMessage(replyHeader, payloadRaw);
    replyMsg.header.dataLen = serviceNameToCopy.length();
    WriteMessageFrame(replyMsg);
    this->sessionData_p->writeBuf.erase();
    StartReadingFrames();
}

void BVTCPSession::OnReceiveHelloBackFrame(void)
{
    BVTCPMessageHeader header = GetMsgHeader();

    if (header.msgType != static_cast<uint8_t>(
            BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_HELLOBACK))
    {
        LogError("Session [{}]: OnReceiveHelloBackFrame called for wrong msgType={}",
                 this->GetSessionData()->sessionID,
                 static_cast<int>(header.msgType));
        return;
    }

    if (header.dataLen > 128)
    {
        LogError("Session [{}]: Invalid HELLOBACK payload length={}",
                 this->GetSessionData()->sessionID,
                 static_cast<unsigned>(header.dataLen));
        return;
    }

    const char* payloadPtr = GetPayloadPtr();
    if (!payloadPtr)
    {
        LogError("Session [{}]: Payload pointer is null.",
                 this->GetSessionData()->sessionID);
        return;
    }

    // This gets the payload - might be useful to put that into function
    std::string payloadStr(payloadPtr, static_cast<std::size_t>(header.dataLen));

    LogTrace("Session [{}]: Received _HELLOBACK with payload='{}'. Calling Manager handler.",
             this->GetSessionData()->sessionID,
             payloadStr);

    manager_p->HandleSessionIdentification(payloadStr, shared_from_this());
}

void BVTCPSession::OnReceiveConfirmEstablished(void)
{
    LogTrace("Session [{}]: Received _CONFIRM_ESTABLISHED. Changing the state to BVSESSIONSTATE_ESTABLISHED",
            this->GetSessionData()->sessionID);
    SetState(BVSessionState::BVSESSIONSTATE_ESTABLISHED);
    StartReadingFrames();
}

// TODO: This is probably not needed,
// as deregistration can be handled from the mDNS side.
// void BVTCPSession::OnReceiveNodeGoodbyeFrame(void)
// {
//     BVTCPMessageHeader header = GetMsgHeader();
//     const char* payloadPtr = GetPayloadPtr();
//     // std::string payloadStr(payloadPtr, static_cast<std::size_t>(header.dataLen));
//     std::string payloadStr("GUUUUUUUUWNO");

//     LogTrace("Session [{}]: Received _NODESESSION_GOODBYE from {}",
//         this->GetSessionData()->sessionID, this->GetSessionData()->nodeData.serviceName);

//     // Maybe we have the serviceName here in nodeData here?
//     // assert(payloadStr == this->GetSessionData()->nodeData.serviceName); // ??? Yes!
//     // this will be the endpoint's serviceName? Yes! TODO: We don't need to send serviceName!

//     // manager_p->PutMessageIntoAppMailbox(BVEventType::BVEVENTTYPE_APP_SERVICE_DEREGISTERED,
//     //     std::make_unique<std::any>(std::make_any<std::string>(payloadStr)));
//     manager_p->RemoveSession(this->sessionData_p->sessionID);
//     // Put message in app mailbox so it can react
//     // BVTCPSession remove it from the map
//     // Close this session
// }

void BVTCPSession::OnReceiveChatMessageFrame(void)
{
    BVTCPMessageHeader header = GetMsgHeader();
    if (header.msgType != static_cast<uint8_t>(
            BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_CHATMESSAGE))
    {
        LogError("Session [{}]: OnReceiveChatMessageFrame called for wrong msgType={}",
                 this->GetSessionData()->sessionID,
                 static_cast<int>(header.msgType));
        return;
    }

    const char* payloadPtr = GetPayloadPtr();
    if (!payloadPtr)
    {
        LogError("Session [{}]: Payload pointer is null.",
                 this->GetSessionData()->sessionID);
        return;
    }

    BVChatMessagePayload payload;
    std::memcpy(&payload, payloadPtr, sizeof(BVChatMessagePayload));
    std::string payloadStr(payload.textData.data(), static_cast<std::size_t>(header.dataLen));

    LogTrace("Session [{}]: Received chat message: '{}'", this->GetSessionData()->sessionID, payloadStr);
    manager_p->PutMessageIntoAppMailbox(
        BVMessage(
            BVEventType::BVEVENTTYPE_APP_MESSAGE_INCOMING,
            std::make_unique<std::any>(std::make_any<BVChatMessage>(BVChatMessage(payloadStr, 
                header.timestamp, sessionData_p->nodeData.serviceName)))
        ));
    StartReadingFrames();
}

// TODO: Change that to void - always returns false
bool BVTCPSession::OnReceiveStandardFrame(void)
{
    // Parse
    // copy to buffer 10 bytes and read message type
    // needed?

    LogTrace(
        "Session [{}]: Received a standard frame, parsing...",
            this->GetSessionData()->sessionID);

    BVTCPMessageHeader header = GetMsgHeader();
    switch (header.msgType)
    {
        // case BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_NODESESSION_GOODBYE:
        // {
        //     break
        // }
        case BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_CHATMESSAGE:
        {
            LogTrace(
                "Session [{}]: Received BVSESSIONREGULARMESSAGETYPE_CHATMESSAGE",
            this->GetSessionData()->sessionID);
            OnReceiveChatMessageFrame();
            break;
        }
        default:
        {
            LogWarn(
                "Session [{}]: Received a standard, unrecognized frame. It maybe a file header.",
                    this->GetSessionData()->sessionID);
            break;
        }
    }

    // When beginning file transfer, first message is received as a standard frame.
    // So we're trying to get file header from the buffer destined for messages.
    BVTCPFileHeader fileHeader = GetFileHeader(this->sessionData_p->readBuf.get());
    switch (fileHeader.msgType)
    {
        case BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_TRANSFER_BEGIN:
        {
            LogTrace(
                "Session [{}]: Received BVSESSIONREGULARMESSAGETYPE_FILE_TRANSFER_BEGIN",
            this->GetSessionData()->sessionID);
            OnReceiveFileTransferBegin();
            return true; // DO NOT APPEND A JOB THAT READS INTO MSG BUFFER!
        }
        case BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_OFFER:
        {
            // A peer is asking permission to send us a file. Hand it to the app
            // (GUI prompts; CLI auto-accepts) and keep reading standard frames so
            // we receive the FILE_TRANSFER_BEGIN once we accept. Re-arming is the
            // handler's job (ClearReadBuffer does not re-arm).
            OnReceiveFileOffer();
            StartReadingFrames();
            return true;
        }
        case BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_ACCEPT:
        {
            // The receiver accepted our offer -> start the real transfer.
            this->manager_p->SignalFileTransfer(fileHeader.correlationKey, true);
            StartReadingFrames();
            return true;
        }
        case BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_REJECT:
        {
            // The receiver declined -> drop the pending transfer, nothing sent.
            this->manager_p->SignalFileTransfer(fileHeader.correlationKey, false);
            StartReadingFrames();
            return true;
        }
        default:
        {
            LogError(
                "Session [{}]: Received a standard, unrecognized frame. It isn't a file header.",
                    this->GetSessionData()->sessionID);
            break;
        }
    }
    return false;
}

void BVTCPSession::OnReceiveFileOffer(void)
{
    BVTCPFileHeader   header  = GetFileHeader(this->sessionData_p->readBuf.get());
    std::vector<char> payload = GetFileData(this->sessionData_p->readBuf.get()); // "service|name"
    const uint32_t correlationKey = header.correlationKey;
    const uint32_t fsize          = static_cast<uint32_t>(header.metadata & 0xFFFFFFFF);
    LogTrace("[BVTCPSession ({})]: Received FILE_OFFER key={} size={}",
             this->GetSessionData()->sessionID, correlationKey, fsize);
    this->manager_p->PutMessageIntoAppMailbox(
        BVMessage(
            BVEventType::BVEVENTTYPE_APP_FILE_OFFER,
            std::make_unique<std::any>(std::make_any<BVTCPFileData>(
                BVTCPFileData(correlationKey, 0, fsize, payload)))));
}

void BVTCPSession::OnReceiveFileTransferBegin(void)
{
    BVTCPFileHeader   header = GetFileHeader(this->sessionData_p->readBuf.get());
    std::vector<char> payload = GetFileData(this->sessionData_p->readBuf.get());
    if (header.msgType != static_cast<uint8_t>(
            BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_TRANSFER_BEGIN))
    {
        LogError("Session [{}]: OnReceiveFileTransferBegin called for wrong msgType={}",
                 this->GetSessionData()->sessionID,
                 static_cast<int>(header.msgType));
        return;
    }
    const uint32_t correlationKey = header.correlationKey;
    const uint64_t metadata = header.metadata;
    // Here we save chunk size from metadata
    this->sessionData_p->csize = (metadata & 0xFFFFFFFF00000000) >> 32;
    this->sessionData_p->fsize = (uint32_t)(metadata & 0x0000000FFFFFFFFF);
    LogTrace("[BVTCPSession ({})]: Got OnReceiveFileTransferBegin. Chunk size: {} File size: {}", 
        this->GetSessionData()->sessionID,
        this->sessionData_p->csize,
        this->sessionData_p->fsize);

    manager_p->PutMessageIntoAppMailbox(
        BVMessage(
            BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_BEGIN,
            std::make_unique<std::any>(std::make_any<BVTCPFileData>(BVTCPFileData(correlationKey, this->sessionData_p->csize, 
                this->sessionData_p->fsize, payload)))
        ));
    
    ClearReadBuffer();
    this->sessionData_p->fileReadBuf = std::make_unique<char[]>(this->sessionData_p->csize + FILE_HEADER_SIZE_BYTES);
    std::memset(this->sessionData_p->fileReadBuf.get(), 0, this->sessionData_p->csize + FILE_HEADER_SIZE_BYTES);
    this->sessionData_p->totalBytesRead = 0;
    StartReadingChunks(this->sessionData_p->csize + FILE_HEADER_SIZE_BYTES);
}

void BVTCPSession::OnReceiveFileChunkSent(void)
{
    BVTCPFileHeader   header = GetFileHeader(this->sessionData_p->fileReadBuf.get());
    std::vector<char> payload = GetFileData(this->sessionData_p->fileReadBuf.get(), this->sessionData_p->csize);
    if (header.msgType != static_cast<uint8_t>(
            BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_TRANSFER_CHUNK_SENT))
    {
        LogError("Session [{}]: OnReceiveFileChunkSent called for wrong msgType={}",
                 this->GetSessionData()->sessionID,
                 static_cast<int>(header.msgType));
        return;
    }
    LogTrace("[BVTCPSession ({})]: Got OnReceiveFileChunkSent. Chunk size: {} File size: {}", 
        this->GetSessionData()->sessionID,
        this->sessionData_p->csize,
        this->sessionData_p->fsize);

    const uint32_t correlationKey = header.correlationKey;
    manager_p->PutMessageIntoAppMailbox(
        BVMessage(
            BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_CHUNK_SENT,
            std::make_unique<std::any>(std::make_any<BVTCPFileData>(BVTCPFileData(correlationKey, this->sessionData_p->csize, 
                this->sessionData_p->fsize, payload)))
        ));
}

void BVTCPSession::OnReceiveFileTransferEnd(void)
{
    BVTCPFileHeader   header = GetFileHeader(this->sessionData_p->fileReadBuf.get());
    std::vector<char> payload = GetFileData(this->sessionData_p->fileReadBuf.get(), this->sessionData_p->csize);
    if (header.msgType != static_cast<uint8_t>(
            BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_TRANSFER_END))
    {
        LogError("Session [{}]: OnReceiveChatMessageFrame called for wrong msgType={}",
                 this->GetSessionData()->sessionID,
                 static_cast<int>(header.msgType));
        return;
    }
    LogTrace("[BVTCPSession ({})]: Got OnReceiveFileTransferEnd. Chunk size: {} File size: {}", 
        this->GetSessionData()->sessionID,
        this->sessionData_p->csize,
        this->sessionData_p->fsize);

    const uint32_t correlationKey = header.correlationKey;
    manager_p->PutMessageIntoAppMailbox(
        BVMessage(
            BVEventType::BVEVENTTYPE_APP_FILE_TRANSFER_END,
            std::make_unique<std::any>(std::make_any<BVTCPFileData>(BVTCPFileData(correlationKey, this->sessionData_p->csize, 
                this->sessionData_p->fsize, payload)))
        )); 
}