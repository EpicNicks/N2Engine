#include <engine/Application.hpp>
#include <engine/GameObject.hpp>
#include <engine/Positionable.hpp>
#include <engine/example/QuadRenderer.hpp>

#include <engine/GameObject.inl>

#include <engine/input/ActionMap.hpp>
#include <engine/input/InputBinding.hpp>
#include <engine/input/InputSystem.hpp>
#include <engine/input/InputMapping.hpp>
#include <engine/physics/BoxCollider.hpp>
#include <engine/physics/Rigidbody.hpp>
#include <engine/io/Resources.hpp>

#include <renderer/opengl/OpenGLRenderer.hpp>
#include <renderer/common/IShader.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/IMaterial.hpp>
#include <GLFW/glfw3.h>

#include <iostream>
#include <cmath>

#include "test_project/Spin.hpp"
#include "test_project/CameraController.hpp"
#include "test_project/StandardInputHandler.hpp"

// Simple vertex shader source
constexpr const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec4 aColor;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec4 vertexColor;

void main()
{
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
    vertexColor = aColor;
}
)";

// Simple fragment shader source
constexpr const char* fragmentShaderSource = R"(
#version 330 core
in vec4 vertexColor;
out vec4 FragColor;

void main()
{
    FragColor = vertexColor;
}
)";

// Window dimensions
constexpr uint32_t WINDOW_WIDTH = 800;
constexpr uint32_t WINDOW_HEIGHT = 600;

// Create identity matrix
void createIdentityMatrix(float* matrix)
{
    for (int i = 0; i < 16; i++)
    {
        matrix[i] = 0.0f;
    }
    for (int i = 0; i < 4; i++)
    {
        matrix[i * 4 + i] = 1.0f;
    }
}

// Create simple orthographic projection matrix
void createOrthographicMatrix(float* matrix, const float left, const float right, const float bottom, const float top,
                              const float nearB, const float farB)
{
    for (int i = 0; i < 16; i++)
    {
        matrix[i] = 0.0f;
    }

    matrix[0] = 2.0f / (right - left);
    matrix[5] = 2.0f / (top - bottom);
    matrix[10] = -2.0f / (farB - nearB);
    matrix[12] = -(right + left) / (right - left);
    matrix[13] = -(top + bottom) / (top - bottom);
    matrix[14] = -(farB + nearB) / (farB - nearB);
    matrix[15] = 1.0f;
}

// GLFW error callback
void errorCallback(int error, const char* description)
{
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

// GLFW framebuffer size callback
void framebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    if (auto* renderer = static_cast<Renderer::OpenGL::OpenGLRenderer*>(glfwGetWindowUserPointer(window)))
    {
        renderer->Resize(width, height);
    }
}

void TestRenderer()
{
    // Initialize GLFW
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return;
    }

    // Set GLFW error callback
    glfwSetErrorCallback(errorCallback);

    // Configure GLFW
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Create window
    GLFWwindow* window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Triangle Test", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return;
    }

    // Create renderer
    auto renderer = Renderer::OpenGL::CreateOpenGLRenderer();
    auto* openglRenderer = dynamic_cast<Renderer::OpenGL::OpenGLRenderer*>(renderer.get());

    // Set window user pointer for callbacks
    glfwSetWindowUserPointer(window, openglRenderer);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);

    // Initialize renderer
    if (!renderer->Initialize(window, WINDOW_WIDTH, WINDOW_HEIGHT))
    {
        std::cerr << "Failed to initialize renderer" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }

    std::cout << "Using: " << renderer->GetRendererName() << std::endl;

    // Create triangle mesh data
    Renderer::Common::MeshData triangleData;

    // Define triangle vertices (in normalized device coordinates)
    triangleData.vertices = {
        // Vertex 1 (top, red)
        {
            {0.0f, 0.5f, 0.0f}, // position
            {0.0f, 0.0f, 1.0f}, // normal (pointing toward camera)
            {0.5f, 1.0f}, // texture coordinates
            {1.0f, 0.0f, 0.0f, 1.0f} // color (red)
        },
        // Vertex 2 (bottom-left, green)
        {
            {-0.5f, -0.5f, 0.0f}, // position
            {0.0f, 0.0f, 1.0f}, // normal
            {0.0f, 0.0f}, // texture coordinates
            {0.0f, 1.0f, 0.0f, 1.0f} // color (green)
        },
        // Vertex 3 (bottom-right, blue)
        {
            {0.5f, -0.5f, 0.0f}, // position
            {0.0f, 0.0f, 1.0f}, // normal
            {1.0f, 0.0f}, // texture coordinates
            {0.0f, 0.0f, 1.0f, 1.0f} // color (blue)
        }
    };

    // Define triangle indices
    triangleData.indices = {0, 1, 2};

    // Create mesh
    Renderer::Common::IMesh* triangleMesh = renderer->CreateMesh(triangleData);
    if (triangleMesh == 0)
    {
        std::cerr << "Failed to create triangle mesh" << std::endl;
        renderer->Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }

    // Create shader program
    Renderer::Common::IShader* shaderProgram = renderer->CreateShaderProgram(vertexShaderSource, fragmentShaderSource);
    if (shaderProgram == 0)
    {
        std::cerr << "Failed to create shader program" << std::endl;
        renderer->DestroyMesh(triangleMesh);
        renderer->Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }

    // Create material
    Renderer::Common::IMaterial* material = renderer->CreateMaterial(shaderProgram, 0); // No texture
    if (material == nullptr)
    {
        std::cerr << "Failed to create material" << std::endl;
        renderer->DestroyShaderProgram(shaderProgram);
        renderer->DestroyMesh(triangleMesh);
        renderer->Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }

    // Set up matrices
    float modelMatrix[16];
    float viewMatrix[16];
    float projectionMatrix[16];

    createIdentityMatrix(modelMatrix);
    createIdentityMatrix(viewMatrix);
    createOrthographicMatrix(projectionMatrix, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);

    // Set view and projection matrices
    renderer->SetViewProjection(viewMatrix, projectionMatrix);

    // Set clear color to dark gray
    renderer->Clear(0.2f, 0.3f, 0.3f, 1.0f);

    std::cout << "Rendering triangle. Press ESC to exit." << std::endl;

    // Main render loop
    while (!glfwWindowShouldClose(window))
    {
        // Poll events
        glfwPollEvents();

        // Handle input
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(window, true);
        }

        // Rotate the triangle for some animation
        static float angle = 0.0f;
        angle += 0.01f;

        // Create rotation matrix for model
        float cosA = cosf(angle);
        float sinA = sinf(angle);
        createIdentityMatrix(modelMatrix);
        modelMatrix[0] = cosA;
        modelMatrix[1] = sinA;
        modelMatrix[4] = -sinA;
        modelMatrix[5] = cosA;

        // Begin frame
        renderer->BeginFrame();

        // Draw triangle
        renderer->DrawMesh(triangleMesh, modelMatrix, material);

        // End frame and present
        renderer->EndFrame();
        renderer->Present();
    }

    // Cleanup
    std::cout << "Cleaning up..." << std::endl;
    renderer->DestroyMaterial(material);
    renderer->DestroyShaderProgram(shaderProgram);
    renderer->DestroyMesh(triangleMesh);
    renderer->Shutdown();

    glfwDestroyWindow(window);
    glfwTerminate();
}

void TestEngine()
{
    using namespace N2Engine;

    Application& application = Application::GetInstance();

    // TO MOVE TO GUI SCENE EDITOR
    auto testScene = Scene::Create("Test Scene");
    application.Init(std::move(testScene));
    application.GetWindow().clearColor = Common::Color::Magenta();

    application.GetWindow().SetWindowMode(WindowMode::Windowed);

    auto quadObject = GameObject::Create("TestQuad");
    auto* quadRenderer = quadObject->AddComponent<Example::QuadRenderer>();
    auto* boxCollider = quadObject->AddComponent<Physics::BoxCollider>();
    auto* rigidbody = quadObject->AddComponent<Physics::Rigidbody>();
    rigidbody->SetBodyType(Physics::BodyType::Kinematic);

    auto spinComponent = quadObject->AddComponent<Spin>();
    spinComponent->degreesPerSecond = -2.0f;

    auto cameraControlObject = GameObject::Create("Camera Controller");
    cameraControlObject->AddComponent<CameraController>();

    auto standardInputHandler = GameObject::Create("Standard Input Handler");
    standardInputHandler->AddComponent<StandardInputHandler>();

    SceneManager::GetCurSceneRef().AddRootGameObjects(
        {
            quadObject,
            cameraControlObject,
            standardInputHandler
        });

    auto connectedGamepads = application.GetWindow().GetInputSystem()->GetConnectedGamepads();
    if (!connectedGamepads.empty())
    {
        std::cout << "Connected Gamepads: {\n";
        for (const auto& gamepadInfo : connectedGamepads)
        {
            std::cout << "\tName: " << gamepadInfo.name << ", Id: " << gamepadInfo.gamepadId << "\n";
        }
        std::cout << "}\n";
    }

    // TO MOVE TO INPUT SYSTEM, PARSING INPUT OBJECT
    application
        .GetWindow()
        .GetInputSystem()
        ->MakeActionMap(
            "Main Controls",
            [&](Input::ActionMap* actionMap)
            {
                actionMap
                    ->MakeInputAction(
                        "Camera Move",
                        [&](Input::InputAction* inputAction)
                        {
                            inputAction
                                ->AddBinding(std::make_unique<Input::Vector2CompositeBinding>(
                                    application.GetWindow(), Input::Key::W, Input::Key::S, Input::Key::A,
                                    Input::Key::D))
                                .AddBinding(std::make_unique<Input::GamepadStickBinding>(
                                    application.GetWindow(), Input::GamepadAxis::LeftX, Input::GamepadAxis::LeftY, 0,
                                    0.25f, false, true));
                        })
                    .MakeInputAction(
                        "Camera Rotate",
                        [&](Input::InputAction* inputAction)
                        {
                            inputAction
                                ->AddBinding(std::make_unique<Input::Vector2CompositeBinding>(
                                    application.GetWindow(), Input::Key::Up, Input::Key::Down, Input::Key::Left,
                                    Input::Key::Right))
                                .AddBinding(std::make_unique<Input::GamepadStickBinding>(
                                    application.GetWindow(), Input::GamepadAxis::RightX, Input::GamepadAxis::RightY, 0,
                                    0.25f));
                        })
                    .MakeInputAction(
                        "Quit",
                        [&](Input::InputAction* inputAction)
                        {
                            inputAction->AddBinding(
                                std::make_unique<Input::ButtonBinding>(application.GetWindow(), Input::Key::Escape));
                        });
            })
        .LoadActionMap("Main Controls");

    application.Run();
}

int main()
{
    // TestRenderer();
    TestEngine();
    return 0;
}
