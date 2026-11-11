#include "PlayerOptions.hpp"

#include <charconv>
#include <format>

namespace N2Engine::Player
{
    namespace
    {
        std::optional<RenderBackend> ParseRenderBackend(const std::string_view name)
        {
            if (name == "opengl")
            {
                return RenderBackend::OPENGL;
            }
            if (name == "vulkan")
            {
                // A build with N2ENGINE_RENDERER_VULKAN=OFF still parses it: Window's init then explains there is no Vulkan
                return RenderBackend::VULKAN;
            }
            if (name == "software")
            {
                return RenderBackend::SOFTWARE;
            }
            return std::nullopt;
        }

        /// `--name value` or `--name=value`: the value, advancing i past it; an error when it is missing
        std::expected<std::string_view, std::string> OptionValue(const std::span<const std::string_view> args, std::size_t &i,
                                                                 const std::string_view name)
        {
            const std::string_view arg = args[i];
            if (arg.size() > name.size() && arg[name.size()] == '=')
            {
                return arg.substr(name.size() + 1);
            }
            if (i + 1 >= args.size())
            {
                return std::unexpected(std::format("{} needs a value", name));
            }
            return args[++i];
        }

        bool IsOption(const std::string_view arg, const std::string_view name)
        {
            return arg == name || (arg.starts_with(name) && arg.size() > name.size() && arg[name.size()] == '=');
        }
    }

    std::string PlayerUsage(const std::string_view programName)
    {
        return std::format(
            "Usage: {} [game folder] [--renderer opengl|vulkan|software] [--console-log] [--frames N]\n"
            "  game folder     the folder holding project.n2proj and assets/ (default: the 'game' folder beside the player)\n"
            "  --renderer      the renderer (default: software)\n"
            "  --console-log   print log lines on the console in a Release build too\n"
            "  --frames N      run N frames, then quit (smoke runs)",
            programName);
    }

    std::expected<PlayerOptions, std::string> ParsePlayerArguments(const std::span<const std::string_view> args)
    {
        PlayerOptions options;
        bool haveProject = false;
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            const std::string_view arg = args[i];
            if (arg == "--help" || arg == "-h")
            {
                options.help = true;
            }
            else if (IsOption(arg, "--renderer"))
            {
                const auto value = OptionValue(args, i, "--renderer");
                if (!value)
                {
                    return std::unexpected(value.error() + ": opengl, vulkan or software");
                }
                const auto parsed = ParseRenderBackend(*value);
                if (!parsed)
                {
                    return std::unexpected(std::format("Unknown renderer '{}': expected opengl, vulkan or software", *value));
                }
                options.renderBackend = *parsed;
            }
            else if (arg == "--console-log")
            {
                options.consoleLog = true;
            }
            else if (IsOption(arg, "--frames"))
            {
                const auto value = OptionValue(args, i, "--frames");
                if (!value)
                {
                    return std::unexpected(value.error() + ": a whole number of frames, 1 or more");
                }
                int frames = 0;
                const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), frames);
                if (error != std::errc{} || end != value->data() + value->size() || frames < 1)
                {
                    return std::unexpected(std::format("--frames needs a whole number of frames, 1 or more, not '{}'", *value));
                }
                options.frames = frames;
            }
            else if (arg.starts_with("-"))
            {
                return std::unexpected(std::format("Unknown option '{}'", arg));
            }
            else if (haveProject)
            {
                return std::unexpected(std::format("More than one game folder: '{}' and '{}'", options.projectDir.string(), arg));
            }
            else
            {
                options.projectDir = std::filesystem::path(arg);
                haveProject = true;
            }
        }
        return options;
    }

    std::string_view RenderBackendTitle(const RenderBackend backend)
    {
        switch (backend)
        {
        case RenderBackend::OPENGL: return "OpenGL";
        case RenderBackend::VULKAN: return "Vulkan";
        default: return "software";
        }
    }
}
