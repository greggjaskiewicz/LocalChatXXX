#pragma once
#include "BV.hpp"
#include <boost/asio.hpp>
#include "threadsafequeue.hpp"
#include "BVMessage.hpp"
#include "BVLoggable.hpp"
#include "BVTCPCommon.hpp"

/*
 * Each and every connection we want to handle separately.
 * This class should implement and define functionality
 * that handles the connection with one service, started by accepting that connection.
 * What class should be responsible when we are trying to initiate (connect) to another service?
 * This should handle both incoming and outgoing traffic
 * 
 * This class communicates with App to update its data regarding communication with other nodes
*/

using ReadWriteCallback = std::function<void(const boost::system::error_code&, std::size_t bytes_transferred)>;
// Sessions can call manager's callbacks
class BVTCPConnectionManager;
class BVTCPSession : public BVLoggable, public std::enable_shared_from_this<BVTCPSession>
{
private:
    boost::asio::io_context& ioContext;
    std::shared_ptr<BVTCPNodeConnectionSessionData> sessionData_p;
    BVSessionState state = BVSessionState::BVSESSIONSTATE_UNPREPARED;
    BVSessionOrigin origin;
    // raw, unowning pointer just for reference
    // session dies first, so it's ok.
    BVTCPConnectionManager* manager_p; 
    // std::thread worker_thread;

    void Read(void);
    void WriteSome(void); // data? or is it written in session Data
    void WriteSomeCallback(const boost::system::error_code& ec,
                           std::size_t bytes_transferred)
    {
        if (ec)
        {
            LogError("Session [{}]: Error while writing to a socket! {}, {}. Message: {}",  
                this->GetSessionData()->sessionID, ec.value(), ec.message(), sessionData_p->writeBuf);
            return;
        }
        this->sessionData_p->totalBytesWritten += bytes_transferred;

        if (this->sessionData_p->totalBytesWritten == this->sessionData_p->writeBuf.length())
        {
            // And of writing. Just return
            ClearWriteBuffer();
            return;
        }

        this->sessionData_p->sock->async_write_some(
            boost::asio::buffer(sessionData_p->writeBuf),
            std::bind(&BVTCPSession::WriteSomeCallback, shared_from_this(), std::placeholders::_1, std::placeholders::_2)
        );
    }

    void ReadSomeCallback(const boost::system::error_code& ec,
                          std::size_t bytes_transferred)
    {
        if (ec)
        {
            LogError("Session [{}]: Error in ReadSomeCallback callback: {}, {}",  
                this->GetSessionData()->sessionID, ec.value(), ec.message());
            return;
        }
        this->sessionData_p->totalBytesRead += bytes_transferred;
        if (this->sessionData_p->totalBytesRead == MAX_MESSAGE_SIZE_BYTES)
        {
            ClearReadBuffer();
            return;
        }
        this->sessionData_p->sock->async_read_some(
            boost::asio::buffer(this->sessionData_p->readBuf.get() + this->sessionData_p->totalBytesRead,
                MAX_MESSAGE_SIZE_BYTES - this->sessionData_p->totalBytesRead),
            std::bind(&BVTCPSession::ReadSomeCallback, this, std::placeholders::_1, std::placeholders::_2)
        );
    }

    void ReadFileChunkCallback(const boost::system::error_code& ec,
                               std::size_t bytes_transferred)
    {
        if (ec)
        {
            LogError("Session [{}]: Error in ReadFileChunkCallback callback: {}, {}",  
                this->GetSessionData()->sessionID, ec.value(), ec.message());
            
            LogDebug("Read buffer: {} Read buffer is a nullpointer: {} Bytes read: {} Bytes transferred: {} Address in nodeData: {} Endpoint address: {} State: {}", 
                this->sessionData_p->fileReadBuf.get(), this->sessionData_p->fileReadBuf == nullptr, this->sessionData_p->totalBytesRead, bytes_transferred, 
                    this->sessionData_p->nodeData.address.to_string(), this->sessionData_p->nodeData.ep.address().to_string(), static_cast<int>(this->state));
            LogDebug("Chunk size: {} File size: {}", this->sessionData_p->csize, this->sessionData_p->fsize);
            return;
        }
        this->sessionData_p->totalBytesRead += bytes_transferred;
        LogTrace("Session [{}]: Read file chunk of size: {}.", this->sessionData_p->sessionID, bytes_transferred);
        LogTrace("Session [{}]: Total {} out of expected {}.",  // this should be changed. We're not saving yet information of file progress
            this->sessionData_p->sessionID, 
            this->sessionData_p->totalBytesRead, 
            this->sessionData_p->fsize);
        BVTCPFileHeader header = GetFileHeader(this->sessionData_p->fileReadBuf.get());
        if (header.msgType == BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_TRANSFER_CHUNK_SENT)
        {
            LogTrace("[BVTCPSession (id:{})]: Received another file chunk...",
                this->sessionData_p->sessionID);
            OnReceiveFileChunkSent();
        } else if (header.msgType == BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_TRANSFER_END)
        {
            LogTrace("[BVTCPSession (id:{})]: Received last file chunk... (file transfer end).", 
                this->sessionData_p->sessionID);
            OnReceiveFileTransferEnd();
            ClearFileBuffer();
            this->ClearReadBuffer();
            this->sessionData_p->readBuf = std::make_unique<char[]>(MESSAGE_FRAME_SIZE_BYTES);
            this->sessionData_p->totalBytesRead = 0;
            StartReadingFrames();
            return;
        }
        else
        {
            LogError("[BVTCPSession (id:{})]: Received unrecognized msgType while receiving file chunks...",  
                this->sessionData_p->sessionID);
            // LogDebug("Read buffer: {} Read buffer is a nullpointer: {} Bytes read: {} Bytes transferred: {} Address in nodeData: {} Endpoint address: {} State: {}",
            // this->sessionData_p->fileReadBuf.get(), this->sessionData_p->fileReadBuf == nullptr, this->sessionData_p->totalBytesRead, bytes_transferred,
            //     this->sessionData_p->nodeData.address.to_string(), this->sessionData_p->nodeData.ep.address().to_string(), static_cast<int>(this->state));
            LogDebug("Header: msgType: {} timestamp: {} correlationKey: {} metadata: {}",
                header.msgType, header.timestamp, header.correlationKey, header.metadata);
        }
        this->sessionData_p->totalBytesRead = 0;
        std::memset(this->sessionData_p->fileReadBuf.get(), 0, this->sessionData_p->csize + FILE_HEADER_SIZE_BYTES);
        StartReadingChunks(this->sessionData_p->csize + FILE_HEADER_SIZE_BYTES);
    }

    void ReadMessageFrameCallback(const boost::system::error_code& ec,
                                  std::size_t bytes_transferred)
    {
        if (ec)
        {
            LogError("Session [{}]: Error in ReadMessageFrame callback: {}, {}",  
                this->GetSessionData()->sessionID, ec.value(), ec.message());
            
            LogDebug("Read buffer: {} Read buffer is a nullpointer: {} Bytes read: {} Bytes transferred: {} Address in nodeData: {} Endpoint address: {} State: {}", 
                this->sessionData_p->readBuf.get(), this->sessionData_p->readBuf == nullptr, this->sessionData_p->totalBytesRead, bytes_transferred, 
                    this->sessionData_p->nodeData.address.to_string(), this->sessionData_p->nodeData.ep.address().to_string(), static_cast<int>(this->state));
            return;
        }
        this->sessionData_p->totalBytesRead += bytes_transferred;
        LogTrace("Session [{}]: Read {} bytes", this->sessionData_p->sessionID, bytes_transferred);

        switch (state)
        {
            case BVSessionState::BVSESSIONSTATE_UNPREPARED:
            {
                LogDebug("I'M UNPREPARED");
                break;
            }
            case BVSessionState::BVSESSIONSTATE_ESTABLISHED:
            {
                LogDebug("I'M ESTABLISHED");
                break;
            }
        }

        if (this->sessionData_p->totalBytesRead == MESSAGE_FRAME_SIZE_BYTES)
        {   
            // assert maybe?
            // Reset
            this->sessionData_p->totalBytesRead = 0;

            BVTCPMessageHeader header = GetMsgHeader();
            if (state == BVSessionState::BVSESSIONSTATE_UNPREPARED)
            {
                if (header.msgType == BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_HELLO)
                    OnReceiveHelloFrame();
                if (header.msgType == BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_HELLOBACK)
                    OnReceiveHelloBackFrame();
                if (header.msgType == BVTCPMessageType::BVSESSIONCONTROLMESSAGETYPE_CONFIRM_ESTABLISHED)
                    OnReceiveConfirmEstablished();
            } else
            {
                if (OnReceiveStandardFrame() == true)
                {
                    return;
                }
            }
            LogTrace("Session [{}]: Read all bytes {}", this->sessionData_p->sessionID, bytes_transferred);
            if (state != BVSessionState::BVSESSIONSTATE_CLOSED)
            {
                ClearReadBuffer();
            }
            return;
        }
        boost::asio::async_read(*this->sessionData_p->sock, 
            boost::asio::buffer(this->sessionData_p->readBuf.get() + this->sessionData_p->totalBytesRead,
                MESSAGE_FRAME_SIZE_BYTES - this->sessionData_p->totalBytesRead), 
            std::bind(&BVTCPSession::ReadMessageFrameCallback, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

    void StartReadingFrames(void)
    {
        boost::asio::async_read(*this->sessionData_p->sock, 
            boost::asio::buffer(this->sessionData_p->readBuf.get() + this->sessionData_p->totalBytesRead,
                MESSAGE_FRAME_SIZE_BYTES - this->sessionData_p->totalBytesRead), 
                  std::bind(&BVTCPSession::ReadMessageFrameCallback, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

    void StartReadingChunks(const uint32_t csize)
    {
        LogDebug("StartReadingChunks: requested={}, payloadSize={}, headerSize={}, totalBytesRead={}",
            csize,
            this->sessionData_p->csize,
            FILE_HEADER_SIZE_BYTES,
            this->sessionData_p->totalBytesRead);

        if (!this->sessionData_p->fileReadBuf)
        {
            LogError("fileReadBuf is null");
            return;
        }

        if (this->sessionData_p->totalBytesRead > csize)
        {
            LogError("totalBytesRead={} exceeds packetSize={}",
                this->sessionData_p->totalBytesRead,
                csize);
            return;
        }

        const std::size_t remaining =
            csize - this->sessionData_p->totalBytesRead;

        if (remaining == 0)
        {
            LogWarn("No remaining bytes to read for file packet");
            return;
        }

        boost::asio::async_read(
            *this->sessionData_p->sock,
            boost::asio::buffer(
                this->sessionData_p->fileReadBuf.get() + this->sessionData_p->totalBytesRead,
                remaining
            ),
            std::bind(
                &BVTCPSession::ReadFileChunkCallback,
                shared_from_this(),
                std::placeholders::_1,
                std::placeholders::_2
            )
        );
    }

    /*
        Problem:
        We're always reading MESSAGE_FRAME_SIZE_BYTES, and deciding what to do with the frame.
        Maybe reset new buffer UPON receiving FILETRANSFERSTATE_FIRST_CHUNK.
        Do NOT send any file data with FILETRANSFERSTATE_FIRST_CHUNK. <- not necessarily true
        1. Send fsize and csize with it as normal frame (socket has job async_read with MESSAGE_FRAME_SIZE_BYTES)
           (open file etc...)
        2. Socket receives MESSAGE_FRAME_SIZE_BYTES and allocates sessionData_p->fileReadBuf = [...](CSIZE)
        3. Session calls StartReadingChunks, which go to fileReadBuf.
        4. When Session receives FILETRANSFERSTATE_LAST_CHUNK, it deallocates (clears and nullifies) the fileReadBuf
           (close file etc...)
    */
    // TODO?

    void WriteFileChunkCallback(const boost::system::error_code& ec,
                                std::size_t bytes_transferred)
    {
        if (ec)
        {
            LogError("Session [{}]: Error while writing frame to a socket! {}, {}. Message: {}",  
                this->GetSessionData()->sessionID, ec.value(), ec.message(), sessionData_p->writeBuf);
            return;
        }
        LogTrace("Session [{}]: Written {} bytes", this->sessionData_p->sessionID, bytes_transferred);
        this->sessionData_p->totalBytesWritten = bytes_transferred; // this should be chunk size
        ClearWriteBuffer();
        // Can we send all chunk bytes at once?
        // Or we have to scramble the chunk bytes
        // and then the chunk bytes into files?
        // No - we can send up to 2MB at once, but we will need to load this into memory
    }

    // TODO: This callback actually
    //       writes all the bytes at once - it uses async_write not async_write_some
    //       Part when we checkif totalBytesWritten is equal to the writeBuf.length() (entire frame)
    //       might be redundant.
    void WriteMessageFrameCallback(const boost::system::error_code& ec,
                                   std::size_t bytes_transferred)
    {
        if (ec)
        {
            LogError("Session [{}]: Error while writing frame to a socket! {}, {}. Message: {}",  
                this->GetSessionData()->sessionID, ec.value(), ec.message(), sessionData_p->writeBuf);
                return;
            return;
        }
        this->sessionData_p->totalBytesWritten += bytes_transferred;
        LogDebug("Session [{}]: Writebuffer: {}", this->sessionData_p->sessionID, this->sessionData_p->writeBuf);
        LogTrace("Session [{}]: Written {} bytes", this->sessionData_p->sessionID, bytes_transferred);
        if (this->sessionData_p->totalBytesWritten == this->sessionData_p->writeBuf.length() || this->sessionData_p->writeBuf.empty() 
            || this->sessionData_p->totalBytesWritten == MESSAGE_FRAME_SIZE_BYTES)
        {
            LogTrace("Session [{}]: Written all bytes", this->sessionData_p->sessionID, bytes_transferred);
            if (state != BVSessionState::BVSESSIONSTATE_CLOSED)
            {
                ClearWriteBuffer();
            }
            return;
        }
        boost::asio::async_write(*this->sessionData_p->sock,
            boost::asio::buffer(sessionData_p->writeBuf),
                std::bind(&BVTCPSession::WriteMessageFrameCallback, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

    BVTCPMessageHeader GetMsgHeader(void)
    {
        BVTCPMessageHeader header{};
        const char* buf = this->sessionData_p->readBuf.get();
        header.dataLen = static_cast<uint8_t>(buf[0]);
        std::memcpy(&header.timestamp, buf+1, sizeof(header.timestamp));
        header.msgType = static_cast<uint8_t>(buf[9]);
        return header;
    }

    BVTCPFileHeader GetFileHeader(const char* buf)
    {
        BVTCPFileHeader header{};
        std::memcpy(&header.correlationKey, buf, sizeof(header.correlationKey)); // 0..3 4 bytes
        std::memcpy(&header.timestamp, buf + 4, sizeof(header.timestamp)); // 4..11 8 bytes
        header.msgType = static_cast<uint8_t>(buf[12]);
        std::memcpy(&header.metadata, buf + 13, sizeof(header.metadata));
        return header;
    }

    // BUG: This is wrong, as this will stop at first \0.
    // Other files than text will contain many null characters!
    std::vector<char> GetFileData(char* buf)
    {
        std::vector<char> d_v;
        for (char* c = buf + FILE_HEADER_SIZE_BYTES; *c != '\0'; c++)
        {
            d_v.push_back(*c);
        }
        return d_v;
    }

    std::vector<char> GetFileData(char* buf, const std::size_t csize)
    {
        return std::vector<char>(
            buf + FILE_HEADER_SIZE_BYTES,
            buf + FILE_HEADER_SIZE_BYTES + csize
        );
    }

public:
    BVTCPSession(std::shared_ptr<BVTCPNodeConnectionSessionData> _sessionData_p,
                 boost::asio::io_context& _ioContext);

    void Start(void);
    void Shutdown(void);

    BVTCPNodeConnectionSessionData* GetSessionData(void)
    {
        return this->sessionData_p.get();
    }

    void SetOrigin(const BVSessionOrigin& _origin)
    {
        this->origin = _origin;
    }

    BVSessionOrigin GetOrigin(void)
    {
        return this->origin;
    }

    void SetState(const BVSessionState& _state)
    {
        this->state = _state;
    }

    BVSessionState GetState(void)
    {
        return this->state;
    }

    void SetSessionID(const SessionID& sid)
    {
        this->sessionData_p->sessionID = sid;
    }

    SessionID GetSessionID(void)
    {
        return this->sessionData_p->sessionID;
    }

    // for now only text
    void RequestSomeWrite(const std::string& data);
    void OnReceiveHelloFrame(void);
    void OnReceiveHelloBackFrame(void);
    void OnReceiveConfirmEstablished(void);
    void OnReceiveChatMessageFrame(void);
    // Returns true if we have to return early.
    bool OnReceiveStandardFrame(void);
    void OnReceiveFileTransferBegin(void);
    void OnReceiveFileChunkSent(void);
    void OnReceiveFileTransferEnd(void);
    // void OnReceiveNodeGoodbyeFrame(void);
    // Upon receiving chat message,
    // call BVTCPConnectionManager function
    // or, construct BVMessage and directly put it in appCommChannel_p.

    void RequestReadingFrames(void)
    {
        StartReadingFrames();
    }

    // This function calls async_write to just send all the data
    void WriteFileChunk(const BVTCPFileChunk& chunk,
                        const std::size_t payloadBytes)
    {
        this->sessionData_p->totalBytesWritten = 0;
        constexpr std::size_t headerSize = FILE_HEADER_SIZE_BYTES;

        if (payloadBytes > chunk.payload.size() && 
            chunk.header.msgType != BVTCPMessageType::BVSESSIONREGULARMESSAGETYPE_FILE_TRANSFER_BEGIN)
        {
            LogError("Session [{}]: WriteFileChunk: payloadBytes={} but vector size={}",
                this->sessionData_p->sessionID,
                payloadBytes,
                chunk.payload.size());
            return;
        }

        if (payloadBytes > std::numeric_limits<uint32_t>::max())
        {
            LogError("Session [{}]: WriteFileChunk: payload too large: {}",
                this->sessionData_p->sessionID,
                payloadBytes);
            return;
        }
        const std::size_t frameSize = headerSize + payloadBytes;
        this->sessionData_p->writeBuf.resize(frameSize);
        char* buf = this->sessionData_p->writeBuf.data();
        std::size_t offset = 0;
        const uint32_t chunkSize = static_cast<uint32_t>(payloadBytes);

        std::memcpy(buf + offset, &chunk.header.correlationKey, sizeof(chunk.header.correlationKey));
        offset += sizeof(chunk.header.correlationKey);
        std::memcpy(buf + offset, &chunk.header.timestamp, sizeof(chunk.header.timestamp));
        offset += sizeof(chunk.header.timestamp);
        std::memcpy(buf + offset, &chunk.header.msgType, sizeof(chunk.header.msgType));
        offset += sizeof(chunk.header.msgType);
        std::memcpy(buf + offset, &chunk.header.metadata, sizeof(chunk.header.metadata));
        offset += sizeof(chunk.header.metadata);
        assert(offset == headerSize);

        if (payloadBytes > 0)
        {
            std::memcpy(
                buf + offset,
                chunk.payload.data(),
                payloadBytes
            );
        }
        LogDebug("SEND header bytes: correlationKey={}, msgType={}, frameSize={}, payloadBytes={}, byte12={}",
            chunk.header.correlationKey,
            static_cast<int>(chunk.header.msgType),
            frameSize,
            payloadBytes,
            static_cast<int>(static_cast<unsigned char>(buf[12])));
        auto self = shared_from_this();
        boost::asio::async_write(
            *this->sessionData_p->sock,
            boost::asio::buffer(
                self->sessionData_p->writeBuf.data(),
                self->sessionData_p->writeBuf.size()
            ),
            [self](const boost::system::error_code& ec,
                std::size_t bytesTransferred)
            {
                self->WriteFileChunkCallback(ec, bytesTransferred);
            }
        );
    }

    // Synchronous, blocking write of one file-chunk frame, used by the file
    // transfer worker thread. Sending chunks strictly one-at-a-time on a stable
    // local buffer avoids the overlapping async_write + reallocating shared
    // writeBuf that corrupted the stream (truncated/garbled transfers). TCP's
    // own flow control supplies the back-pressure, so no inter-chunk sleep is
    // needed. Returns false on error.
    bool WriteFileChunkSync(const BVTCPFileChunk& chunk, const std::size_t payloadBytes)
    {
        constexpr std::size_t headerSize = FILE_HEADER_SIZE_BYTES;
        if (payloadBytes > std::numeric_limits<uint32_t>::max())
        {
            LogError("Session [{}]: WriteFileChunkSync: payload too large: {}",
                this->sessionData_p->sessionID, payloadBytes);
            return false;
        }
        const std::size_t frameSize = headerSize + payloadBytes;
        std::vector<char> frame(frameSize, 0); // zero-init -> last chunk is zero-padded
        std::size_t offset = 0;
        std::memcpy(frame.data() + offset, &chunk.header.correlationKey, sizeof(chunk.header.correlationKey));
        offset += sizeof(chunk.header.correlationKey);
        std::memcpy(frame.data() + offset, &chunk.header.timestamp, sizeof(chunk.header.timestamp));
        offset += sizeof(chunk.header.timestamp);
        std::memcpy(frame.data() + offset, &chunk.header.msgType, sizeof(chunk.header.msgType));
        offset += sizeof(chunk.header.msgType);
        std::memcpy(frame.data() + offset, &chunk.header.metadata, sizeof(chunk.header.metadata));
        offset += sizeof(chunk.header.metadata);
        if (payloadBytes > 0)
        {
            const std::size_t toCopy =
                payloadBytes < chunk.payload.size() ? payloadBytes : chunk.payload.size();
            std::memcpy(frame.data() + offset, chunk.payload.data(), toCopy);
        }
        // Blocking, transfers ALL bytes (composed op). One read + one write may
        // run concurrently on a socket, which is the only overlap here.
        boost::system::error_code ec;
        boost::asio::write(*this->sessionData_p->sock,
            boost::asio::buffer(frame.data(), frame.size()), ec);
        if (ec)
        {
            LogError("Session [{}]: WriteFileChunkSync failed: {}, {}",
                this->sessionData_p->sessionID, ec.value(), ec.message());
            return false;
        }
        LogTrace("Session [{}]: Sync-wrote file frame {} bytes (payload {})",
            this->sessionData_p->sessionID, frame.size(), payloadBytes);
        return true;
    }

    template<typename PayloadType>
    void WriteMessageFrame(const BVTCPMessage<PayloadType>& message)
    {
        static_assert(std::is_trivially_copyable_v<BVTCPMessage<PayloadType>>,
                    "BVTCPMessage must be trivially copyable to send as raw bytes.");

        this->sessionData_p->totalBytesWritten = 0;
        
        constexpr std::size_t headerSize = 10;
        constexpr std::size_t payloadSize = MESSAGE_FRAME_SIZE_BYTES - headerSize;

        if (sizeof(PayloadType) > payloadSize)
        {
            LogError("Session [{}]: WriteMessageFrame: payload too large. payloadSize={}, max={}",
                this->sessionData_p->sessionID, sizeof(PayloadType), payloadSize);
            return;
        }

        LogDebug("WriteMessageFrame: data size: {}", sizeof(message.payload));
        LogDebug("WriteMessageFrame: dataLen: {}", message.header.dataLen);
        char* buf = this->sessionData_p->writeBuf.data();
        buf[0] = static_cast<char>(message.header.dataLen);
        std::memcpy(buf + 1,
            &message.header.timestamp,
            sizeof(message.header.timestamp));
        buf[9] = static_cast<char>(message.header.msgType);
        std::memcpy(buf + headerSize,
            &message.payload,
            sizeof(PayloadType));

        assert(this->sessionData_p->writeBuf.size() != 0);

        auto self = shared_from_this();
        boost::asio::async_write(
            *this->sessionData_p->sock,
            boost::asio::buffer(self->sessionData_p->writeBuf.data(),
                                self->sessionData_p->writeBuf.size()),
            [self](const boost::system::error_code& ec, std::size_t bytesTransferred)
            {
                self->WriteMessageFrameCallback(ec, bytesTransferred);
            });
    }

    template<typename PayloadType>
    void WriteMessageFrame(std::unique_ptr<BVTCPMessage<PayloadType>> message)
    {
        static_assert(std::is_trivially_copyable_v<BVTCPMessage<PayloadType>>,
                    "BVTCPMessage must be trivially copyable to send as raw bytes.");

        this->sessionData_p->totalBytesWritten = 0;
        
        constexpr std::size_t headerSize = 10;
        constexpr std::size_t payloadSize = MESSAGE_FRAME_SIZE_BYTES - headerSize;

        if (sizeof(PayloadType) > payloadSize)
        {
            LogError("Session [{}]: WriteMessageFrame: payload too large. payloadSize={}, max={}",
                this->sessionData_p->sessionID, sizeof(PayloadType), payloadSize);
            return;
        }

        LogDebug("WriteMessageFrame: data size: {}", sizeof(message->payload));
        LogDebug("WriteMessageFrame: dataLen: {}", message->header.dataLen);
        char* buf = this->sessionData_p->writeBuf.data();
        buf[0] = static_cast<char>(message->header.dataLen);
        std::memcpy(buf + 1,
            &message->header.timestamp,
            sizeof(message->header.timestamp));
        buf[9] = static_cast<char>(message->header.msgType);
        std::memcpy(buf + headerSize,
            &message->payload,
            sizeof(PayloadType));

        assert(this->sessionData_p->writeBuf.size() != 0);

        auto self = shared_from_this();
        boost::asio::async_write(
            *this->sessionData_p->sock,
            boost::asio::buffer(self->sessionData_p->writeBuf.data(),
                                self->sessionData_p->writeBuf.size()),
            [self](const boost::system::error_code& ec, std::size_t bytesTransferred)
            {
                self->WriteMessageFrameCallback(ec, bytesTransferred);
            });
    }

    void SetManager_p(BVTCPConnectionManager* p)
    {
        this->manager_p = p;
    }

    const char* GetPayloadPtr(void) const
    {
        if (!this->sessionData_p->readBuf)
        {
            return nullptr;
        }

        return this->sessionData_p->readBuf.get() + HEADER_SIZE_BYTES;
    }

    void ClearReadBuffer(void)
    {
        std::memset(this->sessionData_p->readBuf.get(), 0, MESSAGE_FRAME_SIZE_BYTES);
        this->sessionData_p->totalBytesRead = 0;
    }

    void ClearWriteBuffer(void)
    {
        this->sessionData_p->writeBuf.clear();
        this->sessionData_p->totalBytesWritten = 0;
        this->sessionData_p->writeBuf.assign(MESSAGE_FRAME_SIZE_BYTES, '\0');
    }

    void ClearFileBuffer(void)
    {
        constexpr uint8_t ONE_BYTE = 1;
        this->sessionData_p->fileReadBuf = std::make_unique<char[]>(ONE_BYTE);
        std::memset(this->sessionData_p->fileReadBuf.get(), 0, ONE_BYTE);
    }

    void Close(void)
    {
        this->state = BVSessionState::BVSESSIONSTATE_CLOSED;
        this->sessionData_p->sock->cancel();
        this->sessionData_p->readBuf.reset();
        this->sessionData_p->writeBuf.erase();
    }

    ~BVTCPSession() 
    {
        if (this->state != BVSessionState::BVSESSIONSTATE_CLOSED)
        {
            Close();
        }
    }
};
