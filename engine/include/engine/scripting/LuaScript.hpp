#pragma once

#include <string>
#include <filesystem>
#include <fstream>

#include "engine/base/Asset.hpp"
#include "nlohmann/json.hpp"

namespace N2Engine
{
    class LuaScript : public Base::Asset
    {
    private:
        std::string _sourceCode;
        
    public:
        LuaScript() = default;
        explicit LuaScript(std::string sourceCode) : _sourceCode(std::move(sourceCode)) {}
        
        std::string GetResourceType() const override { return "LuaScript"; }
        
        const std::string& GetSourceCode() const { return _sourceCode; }
        void SetSourceCode(const std::string& code) { _sourceCode = code; }
        
        nlohmann::json Serialize() const override
        {
            auto j = Asset::Serialize();
            j["sourceCode"] = _sourceCode;
            return j;
        }
        
        void Deserialize(const nlohmann::json& j) override
        {
            Asset::Deserialize(j);
            if (j.contains("sourceCode"))
            {
                _sourceCode = j["sourceCode"];
            }
        }
        
        bool Load(const std::filesystem::path& path) override
        {
            std::ifstream file(path);
            if (!file.is_open())
            {
                return false;
            }
            
            _sourceCode = std::string{
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()
            };
            
            return !_sourceCode.empty();
        }
    };
}
