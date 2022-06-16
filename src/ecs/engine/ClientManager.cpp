/*
** EPITECH PROJECT, 2022
** B-YEP-400-STG-4-1-indiestudio-mathieu.brujan
** File description:
** ClientManager
*/

#include <iostream>
#include <sstream>
#include "ecs/engine/Network.hpp"
#include "ecs/engine/EntityCommands.hpp"
#include "ecs/engine/SceneManager.hpp"
#include "ecs/engine/Clock.hpp"
#include "network/SocketError.hpp"
#include "raylib/Camera.hpp"

bool ecs::ClientManager::tryRead(void *buf, std::size_t size)
{
    if (!_client->isConnected())
        return true;
    if( _client->read(buf, size) != size) {
        _client->disconnect();
        return true;
    }
    return false;
}

void ecs::ClientManager::handleDisconnect(World &world)
{
    ecs::SceneManager &man = world.getRessource<ecs::SceneManager>();
    ClientNetworkSceneModule &scene = dynamic_cast<ClientNetworkSceneModule&>(man.getScene());

    scene.onDisconnect(world);
}

void ecs::ClientManager::attemptConnection(const std::string &ip, const std::string &port,
void *obj, ConnectionSuccessFct success, ConnectionFailedFct failed)
{
    _ip = ip;
    _port = port;
    _obj = obj;
    _success = success;
    _failed = failed;
    _connAttempted = true;
    _tryConnCount = 0;
    _lastTryDelta = 0;
}

void ecs::ClientManager::tryConnection(ecs::World &world)
{
    ecs::Clock &clock = world.getRessource<ecs::Clock>();
    auto now = std::chrono::system_clock::now();

    _lastTryDelta += clock.getDeltaSec();
    if (_lastTryDelta < 0.5)
        return;
    try {
        connectTo(_ip, _port);
        std::cout << "[CLIENT] Successfully connected to " << _ip << ":" << _port << std::endl;
        _connAttempted = false;
        _success(_obj, world);
    } catch(network::SocketError) {
        if (_tryConnCount == 5) {
            std::cout << "[CLIENT] Failed all connection attempts" << std::endl;
            _connAttempted = false;
            _failed(_obj, world);
            return;
        }
        _lastTryDelta = 0;
        _tryConnCount++;
        std::cout << "[CLIENT] Failed connection attempt (" << _tryConnCount << " / 5)" << std::endl;
    }
}

void ecs::ClientManager::initPlayers(std::uint32_t nbPlayers)
{
    NetworkCommand cmd = NetworkCommand::INIT_PLAYERS;

    _client->write(&cmd, sizeof(NetworkCommand));
    _client->write(&nbPlayers, sizeof(std::uint32_t));
}

void ecs::ClientManager::handleNetworkCommands(World &world)
{
    NetworkCommand cmd;

    _client->updateRWStates();
    while (_client->isConnected() && _client->canRead()) {
        if (!_client->canWrite())
            break;
        // std::cout << "[CLIENT] Read from server" << std::endl;
        if (tryRead(&cmd, sizeof(NetworkCommand)))
            break;
        // std::cout << "[CLIENT] CMD " << (int) cmd << " from server" << std::endl;
        switch (cmd) {
            case NetworkCommand::UPDATE_ENTITY:
            spawnOrUpdateServerEntity(world);
            break;
            case NetworkCommand::KILL_ENTITY:
            killServerEntity(world);
            break;
            case NetworkCommand::PLAYERS_CREATED:
            handlePlayersCreated(world);
            break;
            case NetworkCommand::MOVE_CAMERA:
            handleMoveCamera(world);
            break;
            case NetworkCommand::DISCONNECT_CLIENT:
            case NetworkCommand::PLAYERS_REJECTED:
            _client->disconnect();
            break;
        }
    }
    if (!_client->isConnected())
        handleDisconnect(world);
}

bool ecs::ClientManager::readServerEntityUpdate(std::stringbuf &buffer)
{
    std::uint32_t nbComponents;
    ComponentType componentType;
    std::uint32_t componentSize;

    if (tryRead(&nbComponents, sizeof(std::uint32_t)))
        return true;
    buffer.sputn((char*) &nbComponents, sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < nbComponents; i++) {
        if (tryRead(&componentType, sizeof(ComponentType))
        ||  tryRead(&componentSize, sizeof(std::uint32_t)))
            return true;

        std::string tmp;
        tmp.reserve(componentSize);

        if (tryRead(tmp.data(), componentSize))
            return true;
        buffer.sputn((char*) &componentType, sizeof(ComponentType));
        buffer.sputn((char*) &componentSize, sizeof(std::uint32_t));
        buffer.sputn(tmp.data(), componentSize);
    }
    return false;
}

void ecs::ClientManager::spawnOrUpdateServerEntity(World &world)
{
    Entity serverEntity;
    std::stringbuf buffer;

    if (tryRead(&serverEntity, sizeof(Entity))
    || readServerEntityUpdate(buffer))
        return;
    if (_serverToClient.find(serverEntity) != _serverToClient.end())
        updateServerEntity(serverEntity, buffer, world);
    else
        spawnServerEntity(serverEntity, buffer, world);
}

void ecs::ClientManager::spawnServerEntity(Entity serverEntity, std::stringbuf &buffer, World &world)
{
    EntityCommands entityCmds = world.spawn();
    Entity localEntity = entityCmds.getEntity();
    std::uint32_t nbComponents;
    ComponentType componentType;
    std::uint32_t componentSize;

    // std::cout << "[CLIENT] Creating entity from server" << std::endl;
    _serverToClient.insert({serverEntity, localEntity});
    buffer.sgetn((char*) &nbComponents, sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < nbComponents; i++) {
        buffer.sgetn((char*) &componentType, sizeof(ComponentType));
        buffer.sgetn((char*) &componentSize, sizeof(std::uint32_t));
        entityCmds.insertByType(componentType);
        buffer.sgetn((char*) world.getComponentManager().getComponentByType(componentType, localEntity), componentSize);
    }
    entityCmds.insert(MirroredEntity{0, serverEntity});
}

void ecs::ClientManager::updateServerEntity(Entity serverEntity, std::stringbuf &buffer, World &world)
{
    Entity localEntity = _serverToClient[serverEntity];
    std::uint32_t nbComponents;
    ComponentType componentType;
    std::uint32_t componentSize;

    // std::cout << "[CLIENT] Updating entity from server" << std::endl;
    buffer.sgetn((char*) &nbComponents, sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < nbComponents; i++) {
        buffer.sgetn((char*) &componentType, sizeof(ComponentType));
        buffer.sgetn((char*) &componentSize, sizeof(std::uint32_t));
        buffer.sgetn((char*) world.getComponentManager().getComponentByType(componentType, localEntity), componentSize);
    }
}

void ecs::ClientManager::killServerEntity(World &world)
{
    Entity serverEntity;
    Entity localEntity;

    if (tryRead(&serverEntity, sizeof(Entity)))
        return;
    // std::cout << "[CLIENT] Entity killed from server" << std::endl;
    localEntity = _serverToClient[serverEntity];
    world.getEntityCommands(localEntity).despawn();
    _serverToClient.erase(serverEntity);
}

void ecs::ClientManager::updateLocalEntity(Entity entity, World &world)
{
    std::stringbuf buffer;
    createUpdateLocalEntityBuffer(entity, world, buffer);
    std::string data = buffer.str();
    MirrorEntity &mirror = world.getComponent<MirrorEntity>(entity);

    if (mirror.prevData == data)
        return;
    // std::cout << "[CLIENT] Updating entity to server" << std::endl;
    // std::cout << "Buffer size: " << data.size() << std::endl;
    _client->write((void*) data.c_str(), data.size());
    mirror.prevData[data.copy(mirror.prevData, sizeof(mirror.prevData))] = 0;
}

void ecs::ClientManager::killLocalEntity(Entity entity, World &world)
{
    std::stringbuf buffer;
    createKillLocalEntityBuffer(entity, buffer);
    std::string data = buffer.str();

    _client->write((void*) data.c_str(), data.size());
}

void ecs::ClientManager::handlePlayersCreated(World &world)
{
    ecs::SceneManager &man = world.getRessource<ecs::SceneManager>();
    ClientNetworkSceneModule &scene = dynamic_cast<ClientNetworkSceneModule&>(man.getScene());
    int nbPlayers = scene.getNbPlayersOnClient();
    PlayerId id;

    std::cout << "[CLIENT] Players created" << std::endl;
    for (int i = 0; i < nbPlayers; i++) {
        if (tryRead(&id, sizeof(PlayerId)))
            return;
        scene.playerIdAssigned(id, world);
    }
}

void ecs::ClientManager::handleMoveCamera(ecs::World &world)
{
    raylib::Camera &cam = world.getRessource<raylib::Camera>();
    Vector3 pos;
    Vector3 target;

    std::cout << "[CLIENT] Move camera" << std::endl;
    _client->read(&pos, sizeof(Vector3));
    _client->read(&target, sizeof(Vector3));
    cam.setPosition(pos);
    cam.setTarget(target);
}

void ecs::ClientUpdateSystem::setSignature(ComponentManager &component)
{
    _signature = component.generateSignature<MirrorEntity>();
}

void ecs::ClientUpdateSystem::update(World &world)
{
    ClientManager &man = world.getRessource<ClientManager>();

    if (!man.isConnected()) {
        if (!man.isConnectionAttempt())
            return;
        man.tryConnection(world);
        if (!man.isConnected())
            return;
    }
    man.handleNetworkCommands(world);
    for (Entity entity : _entities)
        man.updateLocalEntity(entity, world);
}
