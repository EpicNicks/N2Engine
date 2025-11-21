#pragma once

#include <string>
#include <filesystem>

// Forward declarations
namespace Renderer::Common
{
    class IShader;
}

namespace N2Engine::IO
{
    class Resources
    {
    private:
        static inline std::filesystem::path _resourcePath{};

        static bool ResourcePathIsValid();

    public:
        static std::filesystem::path GetResourcePath();

        /// @brief Set the base resource path for the application
        /// @note For use from editor for projects or main with a user-defined project structure
        static void SetResourcePath(const std::filesystem::path &newPath);

        /// @brief Load a shader from vertex and fragment shader files
        /// @param vertexShaderPath Path to vertex shader (relative to resource path if set)
        /// @param fragmentShaderPath Path to fragment shader (relative to resource path if set)
        /// @return Pointer to created shader, or nullptr on failure
        static Renderer::Common::IShader *LoadShader(const std::string &vertexShaderPath, const std::string &fragmentShaderPath);
    };
}