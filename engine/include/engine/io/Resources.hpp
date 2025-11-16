#pragma once

#include <string_view>
#include <type_traits>
#include <memory>
#include <fstream>

#include <renderer/common/IShader.hpp>
#include <renderer/opengl/OpenGLShader.hpp>

#include "engine/Application.hpp"
#include <filesystem>

namespace N2Engine::IO
{
    template <typename T>
    concept ShaderType = std::is_base_of_v<Renderer::Common::IShader, T>;

    class Resources
    {
    public:
        template <ShaderType T>
        static std::shared_ptr<T> LoadShader(const std::string &vertexShaderPath, const std::string &fragmentShaderPath)
        {
            if (!std::filesystem::exists(vertexShaderPath) || !std::filesystem::exists(fragmentShaderPath))
            {
                return nullptr;
            }

            std::ifstream vertexFile{vertexShaderPath.data()};
            std::ifstream fragmentFile{fragmentShaderPath.data()};

            std::string vertexSource{std::istreambuf_iterator<char>(vertexFile), std::istreambuf_iterator<char>()};
            std::string fragmentSource{std::istreambuf_iterator<char>(fragmentFile), std::istreambuf_iterator<char>()};

            auto shader = std::make_shared<T>();
            if (!shader->LoadFromStrings(vertexSource, fragmentSource))
            {
                return nullptr;
            }
            return shader;
        }
    };
}