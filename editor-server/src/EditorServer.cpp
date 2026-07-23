#include <format>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

#include "editor-server/EditorServer.hpp"
#include "editor-server/Protocol.hpp"
#include "editor-server/Commands.hpp"
#include "editor-server/Serialization.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_CODE WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#define INVALID_SOCKET -1
#define SOCKET_ERROR_CODE errno
#endif

namespace N2Engine::Editor
{
    using namespace Protocol;

    EditorServer::~EditorServer()
    {
        Stop();
    }

    void EditorServer::Start(int port)
    {
        if (_running) return;

#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            Logger::Error("WSAStartup failed");
            return;
        }
#endif

        _listenSocket = static_cast<int>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (_listenSocket == INVALID_SOCKET)
        {
            Logger::Error("Failed to create socket");
            return;
        }

        int opt = 1;
        setsockopt(_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (bind(_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            Logger::Error("Failed to bind to port " + std::to_string(port));
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
            return;
        }

        if (listen(_listenSocket, 1) < 0)
        {
            Logger::Error("Failed to listen");
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
            return;
        }

        _running = true;
        _serverThread = std::thread(&EditorServer::ServerLoop, this);
    }

    void EditorServer::Stop()
    {
        _running = false;

        if (_listenSocket != -1)
        {
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
        }

        if (_serverThread.joinable())
            _serverThread.join();

#ifdef _WIN32
        WSACleanup();
#endif
    }

    void EditorServer::ServerLoop()
    {
        while (_running)
        {
            Logger::Info("Waiting for editor connection...");

            int clientSocket = static_cast<int>(accept(_listenSocket, nullptr, nullptr));
            if (clientSocket == INVALID_SOCKET)
            {
                if (_running)
                    Logger::Warn("Accept failed: " + std::to_string(SOCKET_ERROR_CODE));
                continue;
            }

            Logger::Info("Editor connected");
            HandleClient(clientSocket);

            CLOSE_SOCKET(clientSocket);
            Logger::Info("Editor disconnected");
        }
    }

    void EditorServer::HandleClient(int clientSocket)
    {
        while (_running)
        {
            // Read command header: [type: 1 byte][length: 4 bytes]
            uint8_t cmdType;
            if (!Receive(clientSocket, &cmdType, 1))
                break;

            uint32_t payloadLength;
            if (!Receive(clientSocket, &payloadLength, sizeof(payloadLength)))
                break;

            // Read payload
            std::vector<uint8_t> payload(payloadLength);
            if (payloadLength > 0 && !Receive(clientSocket, payload.data(), payloadLength))
                break;

            if (static_cast<CommandType>(cmdType) == CommandType::Shutdown)
            {
                BufferWriter response;
                WriteOk(response);
                SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
                break;
            }

            ProcessCommand(clientSocket, cmdType, payload);
        }
    }

    void EditorServer::ProcessCommand(int clientSocket, uint8_t commandType, const std::vector<uint8_t> &payload)
    {
        if (commandType != 0x1)
        {
            Logger::Info("Command Issued: " + std::format("0x{:X}", commandType));
        }
        auto cmd = static_cast<CommandType>(commandType);

        switch (cmd)
        {
        case CommandType::RenderFrame:
            HandleRenderFrame(clientSocket);
            break;
        case CommandType::SetViewportSize:
            HandleSetViewportSize(clientSocket, payload);
            break;
        case CommandType::SetCameraPosition:
            HandleSetCameraPosition(clientSocket, payload);
            break;
        case CommandType::GetCameraPosition:
            HandleGetCameraPosition(clientSocket);
            break;
        case CommandType::CreateScene:
            HandleCreateScene(clientSocket, payload);
            break;
        case CommandType::LoadScene:
            HandleLoadScene(clientSocket, payload);
            break;
        case CommandType::SaveScene:
            HandleSaveScene(clientSocket);
            break;
        case CommandType::DeleteScene:
            HandleDeleteScene(clientSocket, payload);
            break;
        case CommandType::GetCurrentScene:
            HandleGetCurrentScene(clientSocket);
            break;
        case CommandType::CreateEntity:
            HandleCreateEntity(clientSocket, payload);
            break;
        case CommandType::DestroyEntity:
            HandleDestroyEntity(clientSocket, payload);
            break;
        case CommandType::SetEntityTransform:
            HandleSetEntityTransform(clientSocket, payload);
            break;
        case CommandType::GetEntityTransform:
            HandleGetEntityTransform(clientSocket, payload);
            break;
        case CommandType::GetAllEntities:
            HandleGetAllEntities(clientSocket);
            break;
        case CommandType::CreateScript:
            HandleCreateScript(clientSocket, payload);
            break;
        case CommandType::RescanAssets:
            HandleRescanAssets(clientSocket);
            break;
        default:
            Logger::Warn("Unknown command: " + std::to_string(commandType));
            BufferWriter response;
            WriteError(response, "Unknown command");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            break;
        }
    }

    void EditorServer::HandleRenderFrame(int clientSocket)
    {
        auto &app = Application::GetInstance();

        app.RenderEditorFrame();

        // Resize frame buffer if needed
        size_t bufferSize = _viewportWidth * _viewportHeight * 4;
        if (_frameBuffer.size() != bufferSize)
            _frameBuffer.resize(bufferSize);

        // Read pixels from renderer
        // You'll need to implement ReadFramebuffer in your renderer
        app.GetWindow().GetRenderer()->ReadFramebuffer(_frameBuffer.data(), _viewportWidth, _viewportHeight);

        BufferWriter response;
        WriteFrameData(response,
                       static_cast<uint32_t>(_viewportWidth),
                       static_cast<uint32_t>(_viewportHeight),
                       _frameBuffer);

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSetViewportSize(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = SetViewportSizeCmd::Deserialize(reader);

        _viewportWidth = cmd.width;
        _viewportHeight = cmd.height;

        Application::GetInstance().OnWindowResize(_viewportWidth, _viewportHeight);

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSetCameraPosition(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = SetCameraPositionCmd::Deserialize(reader);

        auto *camera = Application::GetInstance().GetMainCamera();
        if (camera)
            camera->SetPosition({cmd.x, cmd.y, cmd.z});

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetCameraPosition(int clientSocket)
    {
        auto *camera = Application::GetInstance().GetMainCamera();

        BufferWriter response;
        if (camera)
        {
            auto pos = camera->GetPosition();
            WriteCameraPosition(response, pos[0], pos[1], pos[2]);
        }
        else
        {
            WriteError(response, "No camera");
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleCreateScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateSceneCmd::Deserialize(reader);

        BufferWriter response;

        try
        {
            auto newScene = Scene::Create(cmd.name);

            // Serialize it before moving
            nlohmann::json sceneJson = newScene->Serialize();
            std::string sceneJsonString = sceneJson.dump();

            SceneManager::AddScene(std::move(newScene), false);

            WriteSceneData(response, sceneJsonString);
            Logger::Info("Created scene: " + cmd.name);
        }
        catch (const std::exception &e)
        {
            Logger::Error("Failed to create scene: " + std::string(e.what()));
            WriteError(response, std::string("Failed to create scene: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleLoadScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        std::string sceneJsonString = reader.ReadString();

        Logger::Info("Loading scene from scene data: " + sceneJsonString);

        nlohmann::json sceneJson = nlohmann::json::parse<std::string>(std::move(sceneJsonString));

        auto scene = Scene::FromJSON(sceneJson, true);
        if (scene == nullptr)
        {
            BufferWriter response;
            WriteError(response, "The scene passed was invalid or corrupt");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        if (int sceneIndex = SceneManager::GetSceneIndex(scene->sceneName); sceneIndex != -1)
        {
            SceneManager::UpdateScene(sceneIndex, sceneJson);
            SceneManager::LoadScene(sceneIndex);
        }
        else
        {
            SceneManager::AddScene(sceneJson);
            SceneManager::LoadScene(scene->sceneName);
        }
        SceneManager::ProcessAnyPendingSceneChange();

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSaveScene(int clientSocket)
    {
        BufferWriter response;

        if (SceneManager::GetCurScene() == nullptr)
        {
            Logger::Warn("No scene loaded to save");
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});

            return;
        }
        nlohmann::json sceneJson = SceneManager::GetCurSceneRef().Serialize();
        SceneManager::UpdateScene(SceneManager::GetCurSceneIndex(), sceneJson);

        WriteSceneData(response, sceneJson);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleDeleteScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = DeleteSceneCmd::Deserialize(reader);

        BufferWriter response;

        try
        {
            // Delete scene file from disk
            if (std::filesystem::exists(cmd.path))
            {
                std::filesystem::remove(cmd.path);
                WriteOk(response);
                Logger::Info("Deleted scene: " + cmd.path);
            }
            else
            {
                WriteError(response, "Scene file not found: " + cmd.path);
            }
        }
        catch (const std::exception &e)
        {
            WriteError(response, std::string("Failed to delete scene: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetCurrentScene(int clientSocket)
    {
        BufferWriter response;

        if (SceneManager::GetCurScene() == nullptr)
        {
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        try
        {
            Scene &scene = SceneManager::GetCurSceneRef();
            nlohmann::json sceneJson = scene.Serialize();

            WriteSceneData(response, sceneJson.dump());
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
        }
        catch (const std::exception &e)
        {
            Logger::Error("Failed to get current scene: " + std::string(e.what()));
            WriteError(response, "Failed to get current scene: " + std::string(e.what()));
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
        }
    }

    void EditorServer::HandleCreateEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateEntityCmd::Deserialize(reader);

        BufferWriter response;

        // Create entity in current scene
        if (SceneManager::GetCurSceneIndex() != -1)
        {
            auto gameObject = GameObject::Create(cmd.name);
            SceneManager::GetCurSceneRef().AddRootGameObject(gameObject);
            WriteEntityCreated(response, gameObject->GetUUID().ToString());
        }
        else
        {
            Logger::Warn("No scene present");
            WriteError(response, "No scene present");
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleDestroyEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        std::string entityId = reader.ReadString();

        bool entityDestroyed = false;
        if (const auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
        {
            if (const auto foundGameObject = SceneManager::GetCurSceneRef().FindGameObjectByUUID(uuid.value()))
            {
                if (SceneManager::GetCurSceneRef().RemoveRootGameObject(foundGameObject))
                {
                    entityDestroyed = true;
                }
            }
        }

        BufferWriter response;
        if (entityDestroyed)
        {
            WriteOk(response);
        }
        else
        {
            WriteError(response, "No game object found with entity id " + entityId);
        }
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = EntityTransformCmd::Deserialize(reader);

        // Apply transform to entity
        if (auto uuid = Math::UUID::FromString(cmd.entityId); uuid.has_value())
        {
            auto entity = SceneManager::GetCurSceneRef().FindGameObjectByUUID(uuid.value());
            if (entity != nullptr && entity->HasPositionable())
            {
                entity->GetPositionable()->SetPositionAndRotation(
                    cmd.position,
                    Math::Quaternion::FromEulerAngles(cmd.rotation)
                );
                entity->GetPositionable()->SetScale(cmd.scale);
            }
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetAllEntities(int clientSocket)
    {
        BufferWriter response;

        if (SceneManager::GetCurScene() == nullptr)
        {
            // No scene, return empty list
            response.WriteU8(static_cast<uint8_t>(ResponseType::EntityList));
            response.WriteU32(4); // payload size
            response.WriteU32(0); // count = 0
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        Scene &scene = SceneManager::GetCurSceneRef();
        auto gameObjects = scene.GetAllGameObjects();
        Logger::Info("HandleGetAllEntities: Found " + std::to_string(gameObjects.size()) + " game objects");

        // Build payload
        BufferWriter payload;
        payload.WriteU32(static_cast<uint32_t>(gameObjects.size()));

        for (const auto &go : gameObjects)
        {
            payload.WriteString(go->GetUUID().ToString());
            payload.WriteString(go->GetName());
        }

        response.WriteU8(static_cast<uint8_t>(ResponseType::EntityList));
        response.WriteU32(static_cast<uint32_t>(payload.Size()));
        response.WriteBytes(payload.Data());

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        std::string entityId = reader.ReadString();

        BufferWriter response;

        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f;
        float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;

        if (auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
        {
            auto entity = SceneManager::GetCurSceneRef().FindGameObjectByUUID(uuid.value());
            if (entity && entity->HasPositionable())
            {
                auto &transform = entity->GetPositionable()->GetGlobalTransform();
                posX = transform.GetPosition().x;
                posY = transform.GetPosition().y;
                posZ = transform.GetPosition().z;

                auto eulerRotation = transform.GetRotation().ToEulerAngles();

                rotX = eulerRotation.x;
                rotY = eulerRotation.y;
                rotZ = eulerRotation.z;

                scaleX = transform.GetScale().x;
                scaleY = transform.GetScale().y;
                scaleZ = transform.GetScale().z;
            }
        }

        // Write response
        response.WriteU8(static_cast<uint8_t>(ResponseType::EntityTransform));
        response.WriteU32(36); // 9 floats = 36 bytes

        response.WriteF32(posX);
        response.WriteF32(posY);
        response.WriteF32(posZ);

        response.WriteF32(rotX);
        response.WriteF32(rotY);
        response.WriteF32(rotZ);

        response.WriteF32(scaleX);
        response.WriteF32(scaleY);
        response.WriteF32(scaleZ);

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleCreateScript(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateScriptCmd::Deserialize(reader);

        BufferWriter response;

        try
        {
            std::string scriptTemplate = GenerateScriptTemplate(cmd.name);
            WriteScriptData(response, scriptTemplate);
            Logger::Info("Generated script template for: " + cmd.name);
        }
        catch (const std::exception &e)
        {
            WriteError(response, std::string("Failed to create script: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    std::string EditorServer::GenerateScriptTemplate(const std::string &scriptName)
    {
        std::string className = scriptName;
        if (!className.empty())
        {
            className[0] = std::toupper(className[0]);
        }

        size_t dotPos = className.find('.');
        if (dotPos != std::string::npos)
        {
            className = className.substr(0, dotPos);
        }

        for (char &c : className)
        {
            if (!std::isalnum(c))
            {
                c = '_';
            }
        }

        return FillTemplate(GetScriptTemplate(), className);
    }

    std::string EditorServer::GetScriptTemplate()
    {
        return R"(
-- {C} Script
-- Auto-generated by N2Engine Editor

local {C} = {}
{C}.__index = {C}

-- Serializable fields that appear in the inspector
{C}.SerializableFields = {
    speed = { type = "float", default = 5.0 },
    enabled = { type = "bool", default = true },
    -- Reference fields
    -- target = { type = "GameObject", default = nil },
    -- rigidBody = { type = "RigidBodyComponent", default = nil },
}

function {C}:OnAttach()
    -- Initialize component here
end

function {C}:OnUpdate()
    -- Update logic here
end

function {C}:OnFixedUpdate()
    -- Physics update logic here
end

function {C}:OnDestroy()
    -- Cleanup here
end

return {C}
)";
    }

    std::string EditorServer::FillTemplate(std::string templ, const std::string &className)
    {
        size_t pos = 0;
        while ((pos = templ.find("{C}", pos)) != std::string::npos)
        {
            templ.replace(pos, 3, className);
            pos += className.size();
        }
        return templ;
    }

    void EditorServer::HandleRescanAssets(int clientSocket)
    {
        IO::ResourceLoader::Instance().RescanAssets();

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    bool EditorServer::Send(int socket, const void *data, size_t size)
    {
        size_t sent = 0;
        auto *bytes = static_cast<const char*>(data);

        while (sent < size)
        {
            int result = send(socket, bytes + sent, static_cast<int>(size - sent), 0);
            if (result <= 0) return false;
            sent += result;
        }
        return true;
    }

    bool EditorServer::Receive(int socket, void *data, size_t size)
    {
        size_t received = 0;
        auto *bytes = static_cast<char*>(data);

        while (received < size)
        {
            int result = recv(socket, bytes + received, static_cast<int>(size - received), 0);
            if (result <= 0) return false;
            received += result;
        }
        return true;
    }

    void EditorServer::SendResponse(int clientSocket, const std::vector<uint8_t> &data)
    {
        Send(clientSocket, data.data(), data.size());
    }
}
