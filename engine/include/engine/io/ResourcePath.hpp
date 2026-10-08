#pragma once

#include <exception>
#include <string>
#include <filesystem>
#include <ranges>
#include <algorithm>

#include "nlohmann/json.hpp"

namespace N2Engine::IO
{
    /// A filesystem path from UTF-8 text. Resource paths, .meta keys and the editor protocol hold UTF-8; a path built
    /// from a std::string directly would read it in the Windows code page instead (and fail for what that can't spell)
    /// Text that isn't valid UTF-8 (which the conversion refuses) is read as before, in the code page, rather than
    /// throwing out of every ResourcePath built from it.
    inline std::filesystem::path PathFromUtf8(const std::string &utf8)
    {
        try
        {
            return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }
        catch (const std::exception &)
        {
            return std::filesystem::path(utf8);
        }
    }

    /// A filesystem path as UTF-8 text with forward slashes (never throws for characters the code page lacks; a path
    /// that can't be converted at all, such as one holding an unpaired surrogate, gives "")
    inline std::string PathToUtf8(const std::filesystem::path &path)
    {
        try
        {
            const std::u8string text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }
        catch (const std::exception &)
        {
            return {};
        }
    }

    enum class PathType
    {
        Resource,   // res://
        User,       // user://
        Absolute,
        Invalid
    };
    
    class ResourcePath
    {
    private:
        PathType _type = PathType::Invalid;
        std::string _path;

        // One spelling per file ("a/./b", "a//b", "a/x/../b" and "a\\b" are all "a/b"), so cache and
        // metadata lookups agree with Resolve. A leading ".." is kept; Resolve rejects escapes.
        void Normalize()
        {
            std::ranges::replace(_path, '\\', '/');
            _path = PathToUtf8(PathFromUtf8(_path).lexically_normal());
            if (_path == ".")
            {
                _path.clear();
            }
            // res:// and user:// paths are relative to their root ("res:///a" is "a"). An absolute path
            // keeps its root: stripping the '/' of a POSIX path ("/home/x") made it relative.
            if (_type != PathType::Absolute)
            {
                while (!_path.empty() && _path[0] == '/')
                {
                    _path.erase(0, 1);
                }
            }
            // "a/b/" and "a/b" are the same directory, but a bare root ("/", "C:/") keeps its separator
            if (_path.ends_with('/') && PathFromUtf8(_path).has_relative_path())
            {
                _path.pop_back();
            }
        }
        
    public:
        ResourcePath() = default;
        
        explicit ResourcePath(const std::string& pathStr)
        {
            if (pathStr.starts_with("res://"))
            {
                _type = PathType::Resource;
                _path = pathStr.substr(6);
            }
            else if (pathStr.starts_with("user://"))
            {
                _type = PathType::User;
                _path = pathStr.substr(7);
            }
            else if (PathFromUtf8(pathStr).is_absolute())
            {
                _type = PathType::Absolute;
                _path = pathStr;
            }
            else
            {
                _type = PathType::Resource;
                _path = pathStr;
            }
            
            Normalize();
        }

        ResourcePath(PathType type, std::string path)
            : _type(type), _path(std::move(path))
        {
            Normalize();
        }
        
        PathType GetType() const { return _type; }
        const std::string& GetPath() const { return _path; }
        bool IsValid() const { return _type != PathType::Invalid && !_path.empty(); }
        
        std::string ToString() const
        {
            switch (_type)
            {
                case PathType::Resource: return "res://" + _path;
                case PathType::User: return "user://" + _path;
                case PathType::Absolute: return _path;
                case PathType::Invalid: return "";
            }
            return "";
        }
        
        std::string ToPathString() const { return _path; }
        
        ResourcePath GetParent() const
        {
            return ResourcePath(_type, PathToUtf8(PathFromUtf8(_path).parent_path()));
        }
        
        std::string GetFilename() const
        {
            return PathToUtf8(PathFromUtf8(_path).filename());
        }
        
        std::string GetStem() const
        {
            return PathToUtf8(PathFromUtf8(_path).stem());
        }
        
        std::string GetExtension() const
        {
            return PathToUtf8(PathFromUtf8(_path).extension());
        }
        
        ResourcePath operator/(const std::string& child) const
        {
            std::filesystem::path p = PathFromUtf8(_path);
            p /= PathFromUtf8(child);
            return ResourcePath(_type, PathToUtf8(p));
        }
        
        bool operator==(const ResourcePath& other) const
        {
            return _type == other._type && _path == other._path;
        }
        
        bool operator!=(const ResourcePath& other) const
        {
            return !(*this == other);
        }
        
        bool operator<(const ResourcePath& other) const
        {
            if (_type != other._type)
                return _type < other._type;
            return _path < other._path;
        }
        
        struct Hash
        {
            size_t operator()(const ResourcePath& path) const
            {
                size_t h1 = std::hash<int>{}(static_cast<int>(path._type));
                size_t h2 = std::hash<std::string>{}(path._path);
                return h1 ^ (h2 << 1);
            }
        };
    };
}

template <>
struct nlohmann::adl_serializer<N2Engine::IO::ResourcePath>
{
    static void to_json(json& j, const N2Engine::IO::ResourcePath& path)
    {
        j = path.ToString();
    }
        
    static void from_json(const json& j, N2Engine::IO::ResourcePath& path)
    {
        path = N2Engine::IO::ResourcePath(j.get<std::string>());
    }
};
