#pragma once

#include "net/socketmanager.h"

class StreamDebug
{
public:
    operator const char* ()
    {
        return mSS.str().c_str();
    }
private:
    std::stringstream mSS;
};

class Sock5Server
{
public:
    Sock5Server() {}
    bool Start(uint16_t port);

    enum class ServerStatus_t
    {
        eStopped,
        eRunning,
        eError
    };

    void Exec();
private:
    class ServerClient
    {
    public:
        ServerClient(ConnectionSocket* clientSocket);
        ~ServerClient();

        enum ClientStatus_t
        {
            eNew,
            eClientGreeting, // received
            eServerChoice,   // sent
            eClientConnectionRequest, // received
            eRequestGranted, // sent
            eRequestError,   // sent
            eInternalError
        };
        Signal <void(ServerClient*, uint32_t)> CLIENT_ERROR;
    private:
        enum RequestAddressType_t
        {
            eNone = 0,
            eIPv4 = 1,
            eDomainName = 3,
            eIPv6 = 4
        };
        void OnRead(LF::net::SocketManager* sock);
        void OnSend(LF::net::SocketManager* sock, uint32_t buffId);
        void OnError(LF::net::SocketManager* sock);
        void OnBOUpdated(LF::net::SocketManager* sock, int32_t count, bool direction);

        void OnPeerSocketConnected(LF::net::SocketManager* sock);

        bool HandleMessage();
        bool HandleClientGreeting(uint8_t* data);
        bool HandleClientRequest(uint8_t* data, uint32_t dataSize, uint32_t& messageSize, IP_Address& addr, RequestAddressType_t& addressType);
        bool DecodeDstAddress(uint8_t* data, uint32_t dataSize, IP_Address& addr, uint32_t& addressSize, RequestAddressType_t& addressType);
        bool SendServerResponse();
        bool EstablishPeerConnection(IP_Address addr);

        void SetError();

        void SetState(ClientStatus_t newState);
        ClientStatus_t mState{ ClientStatus_t::eNew };

        std::shared_ptr<LF::net::SocketManager> mClientSocket;
        std::shared_ptr<LF::net::SocketManager> mPeerSocket;
        LF::threads::IOThread mThread;
        LF::utils::CircularBuffer mBuffer;
        IP_Address mClientAddress;
        RequestAddressType_t mRequestAddressType{ eNone };

        bool mCanWriteClientSocket{ true };
        bool mCanWritePeerSocket{ true };

        uint32_t mBufferIdToSend{0};

        static const int OUT_BUFFER_OCCUPANCY_HIGH = 60;
        static const int OUT_BUFFER_OCCUPANCY_LOW = 32;
    };


    void OnClientConnected(LF::net::SocketManager* sock, ConnectionSocket* newSock);
    void OnClientError(ServerClient* client, uint32_t code);

    void SetState(ServerStatus_t newState);
    ServerStatus_t mState{ ServerStatus_t::eStopped };
    
    std::shared_ptr<LF::net::SocketManager> mSock5Server;
    LF::threads::IOThread mSock5ServerThread;

    std::set<std::shared_ptr<ServerClient>> mClientList;
};

