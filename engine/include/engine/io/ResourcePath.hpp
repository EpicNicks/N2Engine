#pragma once

#include <string>
#include <filesystem>
#include <ranges>
#include <algorithm>

#include "nlohmann/json.hpp"

namespace N2Engine::IO
{
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
            _path = std::filesystem::path(_path).lexically_normal().generic_string();
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
            if (_path.ends_with('/') && std::filesystem::path(_path).has_relative_path())
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
            else if (std::filesystem::path(pathStr).is_absolute())
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
            std::filesystem::path p(_path);
            return ResourcePath(_type, p.parent_path().string());
        }
        
        std::string GetFilename() const
        {
            std::filesystem::path p(_path);
            return p.filename().string();
        }
        
        std::string GetStem() const
        {
            std::filesystem::path p(_path);
            return p.stem().string();
        }
        
        std::string GetExtension() const
        {
            std::filesystem::path p(_path);
            return p.extension().string();
        }
        
        ResourcePath operator/(const std::string& child) const
        {
            std::filesystem::path p(_path);
            p /= child;
            return ResourcePath(_type, p.string());
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
