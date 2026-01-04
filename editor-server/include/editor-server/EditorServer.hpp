#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>

namespace N2Engine::Editor
{
    class EditorServer
    {
    public:
        EditorServer() = default;
        ~EditorServer();

        EditorServer(const EditorServer &) = delete;
        EditorServer& operator=(const EditorServer &) = delete;

        void Start(int port);
        void Stop();
        [[nodiscard]] bool IsRunning() const { return _running; }

    private:
        void ServerLoop();
        void HandleClient(int clientSocket);
        void ProcessCommand(int clientSocket, uint8_t commandType, const std::vector<uint8_t> &payload);

        // Command handlers
        void HandleRenderFrame(int clientSocket);
        void HandleSetViewportSize(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetCameraPosition(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetCameraPosition(int clientSocket);
        void HandleLoadScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSaveScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleCreateEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleDestroyEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetAllEntities(int clientSocket);
        void HandleGetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload);

        // Network helpers
        bool Send(int socket, const void *data, size_t size);
        bool Receive(int socket, void *data, size_t size);
        void SendResponse(int clientSocket, const std::vector<uint8_t> &data);

        std::atomic<bool> _running{false};
        std::thread _serverThread;
        int _listenSocket{-1};

        int _viewportWidth{1280};
        int _viewportHeight{720};
        std::vector<uint8_t> _frameBuffer;
    };
}
