#include <fstream>

#include <renderer/common/IShader.hpp>
#include <renderer/common/Renderer.hpp>

#include "engine/io/Resources.hpp"
#include "engine/Application.hpp"

namespace N2Engine::IO
{
    bool Resources::ResourcePathIsValid()
    {
        return std::filesystem::exists(_resourcePath) && std::filesystem::is_directory(_resourcePath);
    }

    std::filesystem::path Resources::GetResourcePath()
    {
        return _resourcePath;
    }

    void Resources::SetResourcePath(const std::filesystem::path &newPath)
    {
        _resourcePath = newPath;
    }

    Renderer::Common::IShader *Resources::LoadShader(const std::string &vertexShaderPath, const std::string &fragmentShaderPath)
    {
        std::string vertexShaderPathLocal = vertexShaderPath;
        std::string fragmentShaderPathLocal = fragmentShaderPath;

        if (ResourcePathIsValid())
        {
            vertexShaderPathLocal = (GetResourcePath() / vertexShaderPath).string();
            fragmentShaderPathLocal = (GetResourcePath() / fragmentShaderPath).string();
        }

        if (!std::filesystem::exists(vertexShaderPathLocal) || !std::filesystem::exists(fragmentShaderPathLocal))
        {
            return nullptr;
        }

        std::ifstream vertexFile{vertexShaderPathLocal};
        std::ifstream fragmentFile{fragmentShaderPathLocal};

        if (!vertexFile.is_open() || !fragmentFile.is_open())
        {
            return nullptr;
        }

        std::string vertexSource{std::istreambuf_iterator<char>(vertexFile), std::istreambuf_iterator<char>()};
        std::string fragmentSource{std::istreambuf_iterator<char>(fragmentFile), std::istreambuf_iterator<char>()};

        return Application::GetInstance().GetWindow().GetRenderer()->CreateShaderProgram(vertexSource.c_str(), fragmentSource.c_str());
    }
}