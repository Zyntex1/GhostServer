#include "networkmanager.h"

#include <memory>

#include <SFML/Network.hpp>

#include <QVector>

//DataGhost

sf::Packet& operator>>(sf::Packet& packet, Vector& vec)
{
    return packet >> vec.x >> vec.y >> vec.z;
}

sf::Packet& operator<<(sf::Packet& packet, const Vector& vec)
{
    return packet << vec.x << vec.y << vec.z;
}

sf::Packet& operator>>(sf::Packet& packet, DataGhost& dataGhost)
{
    return packet >> dataGhost.position >> dataGhost.view_angle;
}
sf::Packet& operator<<(sf::Packet& packet, const DataGhost& dataGhost)
{
    return packet << dataGhost.position << dataGhost.view_angle;
}

//HEADER

sf::Packet& operator>>(sf::Packet& packet, HEADER& header)
{
    sf::Uint8 tmp;
    packet >> tmp;
    header = static_cast<HEADER>(tmp);
    return packet;
}

sf::Packet& operator<<(sf::Packet& packet, const HEADER& header)
{
    return packet << static_cast<sf::Uint8>(header);
}

NetworkManager::NetworkManager()
    : isRunning(false)
    , serverPort(53000)
    , serverIP("localhost")
    , updateRate(2000)
    , lastID(1) //0 == server
{
}

Client* NetworkManager::GetClientByID(sf::Uint32 ID)
{
    for (auto& client : this->clients) {
        if (client.ID == ID) {
            return &client;
        }
    }

    return nullptr;
}

bool NetworkManager::StartServer(const int port)
{
    if (this->udpSocket.bind(port) != sf::Socket::Done) {
        this->udpSocket.unbind();
        this->listener.close();
        return false;
    }

    if (this->listener.listen(port) != sf::Socket::Done) {
        this->udpSocket.unbind();
        this->listener.close();
        return false;
    }

    this->serverPort = port;
    this->udpSocket.setBlocking(false);

    this->selector.add(this->listener);

    this->serverThread = std::thread(&NetworkManager::RunServer, this);
    this->serverThread.detach();

    emit this->OnNewEvent(QString("Server started on ") + QString::fromStdString(this->serverIP.toString())
        + "(public IP : " + QString::fromStdString(sf::IpAddress::getPublicAddress().toString()) + ") : " + QString::number(this->serverPort));

    return true;
}

void NetworkManager::StopServer()
{
    if (this->isRunning) {
        this->isRunning = false;
        return;
    }

    for (auto& client : this->clients) {
        this->DisconnectPlayer(client);
    }

    this->isRunning = false;
    this->clients.clear();

    emit this->OnNewEvent(QString("Server stopped !"));
}

void NetworkManager::DisconnectPlayer(Client& c)
{
    sf::Packet packet;
    packet << c.IP.toInteger() << HEADER::DISCONNECT;
    int id = 0;
    for (; id < this->clients.size(); ++id) {
        if (this->clients[id].IP != c.IP) {
            this->clients[id].tcpSocket->send(packet);
        } else {
            emit this->OnNewEvent("Player " + QString::fromStdString(this->clients[id].name) + " has disconnected !");
            this->selector.remove(*this->clients[id].tcpSocket);
            this->clients[id].tcpSocket->disconnect();
            this->clients.erase(this->clients.begin() + id);
        }
    }
}

void NetworkManager::DisconnectPlayer(sf::Uint32 ID)
{
    auto client = this->GetClientByID(ID);
    if (client) {
        this->DisconnectPlayer(*client);
    }
}

std::vector<sf::Uint32> NetworkManager::GetPlayerIDByName(std::string name)
{
    std::vector<sf::Uint32> matches;
    for (auto& client : this->clients) {
        if (client.name == name) {
            matches.push_back(client.ID);
        }
    }

    return matches;
}

void NetworkManager::StartCountdown(const std::string preCommands, const std::string postCommands, const int duration)
{
    sf::Packet packet;
    packet << HEADER::COUNTDOWN << sf::Uint32(0) << sf::Uint8(0) << sf::Uint32(duration) << preCommands << postCommands;
    for (auto& client : this->clients) {
        client.tcpSocket->send(packet);
    }
}

bool NetworkManager::IsAlreadyConnected(const sf::IpAddress& ip)
{
    if (std::find_if(this->clients.begin(), this->clients.end(), [&ip](const Client& c) { return ip == c.IP; }) == this->clients.end()) {
        return false;
    }

    return true;
}

void NetworkManager::CheckConnection()
{
    Client client;
    client.tcpSocket = std::make_unique<sf::TcpSocket>();

    if (this->listener.accept(*client.tcpSocket) != sf::Socket::Done) {
        return;
    }

    if (this->IsAlreadyConnected(client.tcpSocket->getRemoteAddress())) {
        return;
    }

    sf::Packet connection_packet;
    client.tcpSocket->receive(connection_packet);

    HEADER header;
    unsigned short int port;
    std::string name;
    DataGhost data;
    std::string model_name;
    std::string level_name;
    bool TCP_only;

    connection_packet >> header >> port >> name >> data >> model_name >> level_name >> TCP_only;

    client.ID = this->lastID++;
    client.IP = client.tcpSocket->getRemoteAddress();
    client.port = port;
    client.name = name;
    client.data = data;
    client.modelName = model_name;
    client.currentMap = level_name;
    client.TCP_only = TCP_only;

    this->selector.add(*client.tcpSocket);

    sf::Packet packet_new_client;

    packet_new_client << client.ID; //Send Client's ID
    packet_new_client << sf::Uint32(this->clients.size()); //Send every players informations
    for (auto& c : this->clients) {
        packet_new_client << c.ID << c.name.c_str() << c.data << c.modelName.c_str() << c.currentMap.c_str();
    }
    client.tcpSocket->send(packet_new_client);

    sf::Packet packet_notify_all; // Notify every players of a new connection
    packet_notify_all << HEADER::CONNECT << client.ID << client.name.c_str() << client.data << client.modelName.c_str() << client.currentMap.c_str();

    for (auto& c : this->clients) {
        c.tcpSocket->send(packet_notify_all);
    }

    emit this->OnNewEvent(QString("New player : " + QString::fromStdString(client.name) + " (" + QString::number(client.port) + ")"));

    this->clients.push_back(std::move(client));
}

void NetworkManager::ReceiveUDPUpdates(std::vector<sf::Packet>& buffer)
{
    sf::Socket::Status status;
    do {
        sf::Packet packet;
        sf::IpAddress ip;
        unsigned short int port;
        status = this->udpSocket.receive(packet, ip, port);
        if (status == sf::Socket::Done) {
            buffer.push_back(packet);
        }
    } while (status == sf::Socket::Done);
}

void NetworkManager::TreatUDP(std::vector<sf::Packet>& buffer)
{
    for (auto& packet : buffer) {
        sf::Packet p;
        HEADER H;
        sf::Uint32 ID;
        p = packet;
        p >> H >> ID;
        for (auto& client : this->clients) {
            if (client.ID != ID) {
                if (!client.TCP_only) {
                    this->udpSocket.send(packet, client.IP, client.port);
                } else {
                    client.tcpSocket->send(packet);
                }
            }
        }
    }
}

void NetworkManager::TreatTCP(sf::Packet& packet)
{
    HEADER header;
    sf::Uint32 ID;
    packet >> header >> ID;

    switch (header) {
    case HEADER::NONE:
        break;
    case HEADER::PING: {
        sf::Packet ping_packet;
        ping_packet << HEADER::PING;
        auto client = this->GetClientByID(ID);
        if (client) {
            client->tcpSocket->send(ping_packet);
        }
    }
        break;
    case HEADER::DISCONNECT: {
        auto client = this->GetClientByID(ID);
        if (client) {
            this->DisconnectPlayer(*client);
        }
        break;
    }
    case HEADER::STOP_SERVER:
        this->StopServer();
        break;
    case HEADER::MAP_CHANGE: {
        for (auto& client : this->clients) {
            if (client.ID != ID) {
                client.tcpSocket->send(packet);
            }
        }
        auto client = this->GetClientByID(ID);
        if (client) {
            std::string map;
            packet >> map;
            emit this->OnNewEvent(QString::fromStdString(client->name) + " is now on " + QString::fromStdString(map));
        }

        break;
    }
    case HEADER::HEART_BEAT:
        break;
    case HEADER::MESSAGE: {
        for (auto& client : this->clients) {
            client.tcpSocket->send(packet);
        }
        break;
    }
    case HEADER::COUNTDOWN: {
        sf::Packet packet_confirm;
        packet_confirm << HEADER::COUNTDOWN << sf::Uint32(0) << sf::Uint8(1);
        auto client = this->GetClientByID(ID);
        if (client) {
            client->tcpSocket->send(packet_confirm);
        }
        break;
    }
    case HEADER::SPEEDRUN_FINISH: {
        for (auto& client : this->clients) {
            client.tcpSocket->send(packet);
        }
        break;
    }
    case HEADER::MODEL_CHANGE: {
        for (auto& client : this->clients) {
            client.tcpSocket->send(packet);
        }
        break;
    }
    case HEADER::UPDATE: {
        for (auto& client : this->clients) {
            if (client.ID != ID) {
                if (!client.TCP_only) {
                    this->udpSocket.send(packet, client.IP, client.port);
                } else {
                    client.tcpSocket->send(packet);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

//Threaded function
void NetworkManager::RunServer()
{
    this->isRunning = true;
    this->clock.restart();

    while (this->isRunning) {

        //UDP
        std::vector<sf::Packet> buffer;
        this->ReceiveUDPUpdates(buffer);
        this->TreatUDP(buffer);

        if (this->selector.wait(sf::milliseconds(50))) { // If a packet is received
            if (this->selector.isReady(this->listener)) {
                this->CheckConnection(); //A player wants to connect
            } else {
                for (int i = 0; i < this->clients.size(); ++i) {
                    if (this->selector.isReady(*this->clients[i].tcpSocket)) {
                        sf::Packet packet;
                        sf::Socket::Status status = this->clients[i].tcpSocket->receive(packet);
                        if (status == sf::Socket::Disconnected) {
                            this->DisconnectPlayer(this->clients[i]);
                            continue;
                        }
                        this->TreatTCP(packet);
                    }
                }
            }
        }
    }

    this->StopServer();
}