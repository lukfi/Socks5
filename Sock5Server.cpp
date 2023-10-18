#include "Sock5Server.h"
#include "utils/stringutils.h"

//#define ENABLE_SDEBUG
#define DEBUG_PREFIX "Sock5Server"
#include "utils/screenlogger.h"

#ifdef SDEB
#undef SDEB
#define SDEB(...)
#endif

bool Sock5Server::Start(uint16_t port)
{
    bool ret = false;
    if (mState == ServerStatus_t::eStopped)
    {
        mSock5ServerThread.Start();
        mSock5Server = std::make_shared<LF::net::SocketManager>(SocketServerType_t::TCPServer, &mSock5ServerThread, false);
        CONNECT(mSock5Server->SM_CLIENT_CONNECTED, Sock5Server, OnClientConnected);
        ret = mSock5Server->Listen(port);
        if (ret)
        {
            SetState(ServerStatus_t::eRunning);
        }
    }
    return ret;
}

void Sock5Server::Exec()
{
    mSock5ServerThread.Join();
}

void Sock5Server::OnClientConnected(LF::net::SocketManager* sock, ConnectionSocket* newSock)
{
#if 0
    if (!mClientList.empty()) 
    {
        delete newSock; return;
    }
#endif
    auto newClient = std::make_shared<ServerClient>(newSock);
    CONNECT(newClient->CLIENT_ERROR, Sock5Server, OnClientError);
    mClientList.insert(newClient);
    SDEB("Client count++: %d", mClientList.size());
}

void Sock5Server::OnClientError(ServerClient* client, uint32_t code)
{
    if (mSock5ServerThread.ThisThread())
    {
        for (auto i : mClientList)
        {
            if (i.get() == client)
            {
                mClientList.erase(i);
                break;
            }
        }
        SDEB("Client count--: %d", mClientList.size());
    }
    else
    {
        SCHEDULE_TASK(&mSock5ServerThread, &Sock5Server::OnClientError, this, client, code);
    }
}

void Sock5Server::SetState(ServerStatus_t newState)
{
    auto toStr = [](ServerStatus_t state) -> std::string
    {
        switch (state)
        {
        case ServerStatus_t::eStopped:
            return "Stopped";
        case ServerStatus_t::eRunning:
            return "Running";
        case ServerStatus_t::eError:
            return "Error";
        }
    };
    SDEB("State changed [%s] -> [%s]", toStr(mState).c_str(), toStr(newState).c_str());
    mState = newState;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Sock5Server::ServerClient::ServerClient(ConnectionSocket* clientSocket) :
    mBuffer(5000)
{
    clientSocket->GetRemoteAddress(mClientAddress);
    SINFO("New client connected: %s", mClientAddress.ToString().c_str());
    mThread.Start();
    mClientSocket = std::make_shared<LF::net::SocketManager>(reinterpret_cast<TCP_Socket*>(clientSocket), &mThread);
    CONNECT(mClientSocket->SM_READ, ServerClient, OnRead);
    CONNECT(mClientSocket->SM_WROTE, ServerClient, OnSend);
    CONNECT(mClientSocket->SM_ERROR, ServerClient, OnError);
    CONNECT(mClientSocket->SM_OUT_BUFFER_OCCUPANCY_NOTIFICATION, ServerClient, OnBOUpdated);
    mClientSocket->SetBufferOccupationNotification(OUT_BUFFER_OCCUPANCY_LOW);
}

Sock5Server::ServerClient::~ServerClient()
{
    SINFO("Client deleted");
}

void Sock5Server::ServerClient::OnRead(LF::net::SocketManager* sock)
{
    if (mState == ClientStatus_t::eRequestGranted)
    {
        std::shared_ptr<LF::net::SocketManager> dst = sock == mClientSocket.get() ? mPeerSocket : mClientSocket;
        bool& canWrite = dst == mClientSocket ? mCanWriteClientSocket : mCanWritePeerSocket;
        while (canWrite)// || !dst)
        {
            MessageBuffer* buf = sock->Read();
            //SDEB("DATA[%s:%d]: %s", sock == mClientSocket.get() ? "C" : "S", buf->Size(), buf->ToString().c_str());
            if (buf && buf->Size())
            {
                //SDEB("Read bytes: %d", buf->Size());
                if (dst)
                {
                    uint32_t bufId = dst->ScheduleBuffer(buf);
                    if (bufId == 0)
                    {
                        SERR("ERRROR!");
                    }
                    if (dst->GetOutBufferOccupancy() > OUT_BUFFER_OCCUPANCY_HIGH)
                    {
                        canWrite = false;
                        SWARN("CAN NOT write: SOCK %s", sock == mClientSocket.get() ? "Peer" : "Client");
                    }
                }
                else
                {
                    SERR("Destination socket is NULL");
                }
            }
            else
            {
                //SDEB("Read bytes: false");
                break;
            }
        }
    }
    else
    {
        if (sock == mPeerSocket.get())
        {
            ERROR("Received data from PeerSocket in wrong state");
        }
        sock->Read(mBuffer);
        while (mState < ClientStatus_t::eRequestGranted && HandleMessage())
        {

        }
    }
}

void Sock5Server::ServerClient::OnSend(LF::net::SocketManager* sock, uint32_t buffId)
{
    if (buffId == mBufferIdToSend)
    {
        switch (mState)
        {
        case ClientStatus_t::eClientGreeting:
            SetState(ClientStatus_t::eServerChoice);
            break;
        default:
            break;
        }
        mBufferIdToSend = 0;
    }
}

void Sock5Server::ServerClient::OnError(LF::net::SocketManager* sock)
{
    SERR("Disconnected");
    SetError();
}

void Sock5Server::ServerClient::OnBOUpdated(LF::net::SocketManager* sock, int32_t count, bool direction)
{
    if (direction)
    {
        //std::shared_ptr<LF::net::SocketManager> thisSocket = sm == mSocket1.get() ? mSocket1 : mSocket2;
        std::shared_ptr<LF::net::SocketManager> thatSocket = sock == mClientSocket.get() ? mPeerSocket : mClientSocket;

        bool& canWrite = sock == mClientSocket.get() ? mCanWriteClientSocket : mCanWritePeerSocket;
        canWrite = true;
        SINFO("CAN     write: SOCK [%s]", sock == mClientSocket.get() ? "C" : "P");
        OnRead(thatSocket.get());
    }
}

void Sock5Server::ServerClient::OnPeerSocketConnected(LF::net::SocketManager* sock)
{
    SUCC("Peer socket connected");
    MessageBuffer* msgBuf = new MessageBuffer();
    if (mRequestAddressType == eIPv4)
    {
        uint8_t buf[] = { 0x5, 0x0, 0x0, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };
        msgBuf->Put(buf, sizeof(buf));
        msgBuf->Rewind();
    }
    else if (mRequestAddressType == eIPv6)
    {
        uint8_t buf[] = { 0x5, 0x0, 0x0, 0x4, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };
        msgBuf->Put(buf, sizeof(buf));
        msgBuf->Rewind();
    }
    else
    {
        SERR("Wrong mRequestAddressType");
        SetError();
        return;
    }

    SetState(ClientStatus_t::eRequestGranted);
    if (mBuffer.Size())
    {
        SWARN("Buffer size still large: %d", mBuffer.Size());
    }
    mClientSocket->ScheduleBuffer(msgBuf);
    OnRead(mPeerSocket.get());
}

bool Sock5Server::ServerClient::HandleMessage()
{
    bool ret = false;

    if (mBuffer.Size())
    {
        uint8_t buf[256];

        switch (mState)
        {
        case ClientStatus_t::eNew:
        {
            if (mBuffer.Size() >= 2)
            {
                //SDEB(">>>[%d] %s", mBuffer.Size(), mBuffer.ToString().c_str());
                size_t read = mBuffer.CopyFromBufferNoMove(buf, 2);
                if (read == 2 && buf[0] == 0x5)
                {
                    if (mBuffer.Size() >= 2 + buf[1])
                    {
                        mBuffer.Get(buf, 2 + buf[1]);
                        ret = HandleClientGreeting(buf);
                        if (ret)
                        {
                            SetState(ClientStatus_t::eClientGreeting);
                            if (!SendServerResponse())
                            {
                                SERR("Failed to send ServerResponse");
                                SetError();
                            }
                        }
                        else
                        {
                            SetError();
                        }
                    }
                    else
                    {
                        SDEB("eNew: No enough data in buffer");
                    }
                }
                else
                {
                    SERR("Wrong SOCKS version: %02x, [%02x], read: %d bytes", buf[0], buf[1], read);
                    SetError();
                }
            }
            break;
        }
        case ClientStatus_t::eServerChoice:
        {
            if (mBuffer.Size() >= 10) // 10 bytes: minimal length of Client connection request 
            {
                uint32_t size = mBuffer.Size();
                size = mBuffer.CopyFromBufferNoMove(buf, min(size, sizeof(buf)));
                SDEB("BUF: %s", LF::utils::DataAsHex(buf, size).c_str());
                if (buf[0] == 0x5)
                {
                    uint32_t messageSize = 0;
                    IP_Address addr;
                    ret = HandleClientRequest(buf, size, messageSize, addr, mRequestAddressType);
                    if (ret)
                    {
                        mBuffer.AdvanceReadCursor(messageSize);
                        SetState(ClientStatus_t::eClientConnectionRequest);
                        SCHEDULE_TASK(&mThread, &Sock5Server::ServerClient::EstablishPeerConnection, this, addr);
                    }
                    //SDEB("Buffer size after = %d", mBuffer.Size());
                }
                else
                {
                    SERR("Wrong SOCKS version: %02x", buf[0]);
                    SetError();
                }
            }
            break;
        }
        default:
        {
            uint32_t size = mBuffer.Size();
            uint8_t* buf = new uint8_t[size];
            mBuffer.CopyFromBuffer(buf, size);

            SWARN("Handle message in unknown state, data: %s", LF::utils::DataAsHex(buf, size).c_str());
            delete[] buf;
        }
        }
    }
    
    return ret;
}

bool Sock5Server::ServerClient::HandleClientGreeting(uint8_t* data)
{
    auto toStr = [](uint8_t auth) -> std::string
    {
        switch (auth)
        {
        case 0:
            return "No authentication";
        case 1:
            return "GSSAPI";

        default:
            return "Unknown";
        }
    };

    for (int i = 0; i < data[1]; ++i)
    {
        SDEB("Client[%s] authentication supported: %d (%s)", mClientAddress.ToString().c_str(), data[2+i], toStr(data[2+1]).c_str());
    }
    return true;
}

bool Sock5Server::ServerClient::HandleClientRequest(uint8_t* data, uint32_t dataSize, uint32_t& messageSize, IP_Address& addr, RequestAddressType_t& addressType)
{
    bool ret = false;

    auto command2Str = [](uint8_t command) -> std::string
    {
        switch (command)
        {
        case 1:
            return "establish a TCP/IP stream connection";
        case 2:
            return "establish a TCP/IP port binding";
        case 3:
            return "associate a UDP port";
        default:
            return "Wrong command";
        }
    };

    uint32_t addressSize = 0;
    uint32_t pointer = 3;
    ret = DecodeDstAddress(&data[pointer], dataSize - pointer, addr, addressSize, addressType);
    pointer += addressSize;
    //SDEB("Address size: %d", addressSize);
    if (ret)
    {
        if (dataSize < pointer + 2)
        {
            SDEB("No data for port");
            ret = false;
        }
        else
        {
            uint16_t port = data[pointer++] << 8;
            port |= data[pointer++];
            addr.ChangePort(port);
            messageSize = pointer;
            SDEB("Client Request command: %d (%s): %s", data[1], command2Str(data[1]).c_str(), addr.ToString().c_str());
        }
    }
    return ret;
}

bool Sock5Server::ServerClient::DecodeDstAddress(uint8_t* data, uint32_t dataSize, IP_Address& addr, uint32_t& addressSize, RequestAddressType_t& addressType)
{
    bool ret = false;

    if (dataSize >= 5)
    {
        if (data[0] == 1) // IPv4
        {
            addr = IP_Address(data[1], data[2], data[3], data[4], 0);
            addressSize = 5;
            addressType = eIPv4;
            ret = true;
        }
        else if (data[0] == 3) // domain name
        {
            SERR("domain name not supported");
            SetError();
        }
        else if (data[0] == 4) // IPv6
        {
            if (dataSize >= 17)
            {
                IP_Address::Host host(data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16]);
                addr = IP_Address(host, 0);
                SWARN("IPv6 request: %s", host.ToString().c_str());
                addressType = eIPv6;
                ret = true;
            }
            //SERR("IPv6 not supported");
            //SetError();

        }
        else
        {
            SERR("Wrong Address type: %d", data[0]);
            SetError();
        }
    }

    return ret;
}

bool Sock5Server::ServerClient::SendServerResponse()
{
    uint8_t data[2] = { 0x5, 0x0 };
    MessageBuffer* buf = new MessageBuffer();
    buf->Put(data, 2);
    buf->Rewind();
    mBufferIdToSend = mClientSocket->ScheduleBuffer(buf);
    return mBufferIdToSend != 0;
}

bool Sock5Server::ServerClient::EstablishPeerConnection(IP_Address addr)
{
    bool ret = false;
    SINFO("Establishing connection to: %s", addr.ToString().c_str());
    mPeerSocket = std::make_shared<LF::net::SocketManager>(SocketType_t::TCPSocket, &mThread, false, true);
    CONNECT(mPeerSocket->SM_CONNECTED, ServerClient, OnPeerSocketConnected);
    CONNECT(mPeerSocket->SM_READ, ServerClient, OnRead);
    CONNECT(mPeerSocket->SM_OUT_BUFFER_OCCUPANCY_NOTIFICATION, ServerClient, OnBOUpdated);
    mPeerSocket->SetBufferOccupationNotification(OUT_BUFFER_OCCUPANCY_LOW);
    ret = mPeerSocket->Connect(addr);
    return ret;
}

void Sock5Server::ServerClient::SetError()
{
    // TOOD: report error to Server, close connection
    SetState(ClientStatus_t::eInternalError);
    CLIENT_ERROR.Emit(this, 0);
}

void Sock5Server::ServerClient::SetState(ClientStatus_t newState)
{
    auto toStr = [](ClientStatus_t state) -> std::string
    {
        switch (state)
        {
        case ClientStatus_t::eNew:
            return "New";
        case ClientStatus_t::eClientGreeting:
            return "ClientGreeting";
        case ClientStatus_t::eServerChoice:
            return "ServerChoice";
        case ClientStatus_t::eClientConnectionRequest:
            return "ClientConnectionRequest";
        case ClientStatus_t::eRequestGranted:
            return "RequestGranted";
        case ClientStatus_t::eInternalError:
            return "InternalError";
        default:
            return "Unknown";
        }
    };
    SDEB("Client State changed [%s] -> [%s]", toStr(mState).c_str(), toStr(newState).c_str());
    mState = newState;
}
