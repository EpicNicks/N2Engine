#pragma once

// The PhysX error callback of the web build: every message PhysX reports (invalid parameters, invalid operations,
// out of memory, internal errors, performance warnings) goes to the engine Logger instead of PxDefaultErrorCallback's
// stdout/stderr text. Native builds keep PxDefaultErrorCallback (their output is unchanged); see PhysXBackend.hpp.
//
// Header-only so a test can drive it without a PhysX scene. Not thread-safe by itself: PhysX may call it from any
// thread that runs a simulation task, and the Logger it forwards to is thread-safe.

#ifdef N2ENGINE_PHYSX_ENABLED

#include "engine/Logger.hpp"

#include <foundation/PxErrorCallback.h>
#include <foundation/PxErrors.h>

#include <string>

namespace N2Engine::Physics
{
    class PhysXLoggerErrorCallback final : public physx::PxErrorCallback
    {
    public:
        /// The text the Logger gets: "PhysX <kind>: <message> (<file>:<line>)"
        static std::string Describe(const physx::PxErrorCode::Enum code, const char *message, const char *file,
                                    const int line)
        {
            return std::string("PhysX ") + KindName(code) + ": " + (message ? message : "") + " (" +
                   (file ? file : "?") + ":" + std::to_string(line) + ")";
        }

        /// Informational messages are Info, warnings (debug and performance) are Warn, every error and abort is Error
        static Logger::LogLevel LevelOf(const physx::PxErrorCode::Enum code)
        {
            switch (code)
            {
            case physx::PxErrorCode::eNO_ERROR:
            case physx::PxErrorCode::eDEBUG_INFO:
                return Logger::LogLevel::Info;
            case physx::PxErrorCode::eDEBUG_WARNING:
            case physx::PxErrorCode::ePERF_WARNING:
                return Logger::LogLevel::Warn;
            default:
                return Logger::LogLevel::Error;
            }
        }

        void reportError(const physx::PxErrorCode::Enum code, const char *message, const char *file,
                         const int line) override
        {
            Logger::Log(Describe(code, message, file, line), LevelOf(code));
        }

    private:
        static const char *KindName(const physx::PxErrorCode::Enum code)
        {
            switch (code)
            {
            case physx::PxErrorCode::eNO_ERROR: return "no error";
            case physx::PxErrorCode::eDEBUG_INFO: return "info";
            case physx::PxErrorCode::eDEBUG_WARNING: return "warning";
            case physx::PxErrorCode::eINVALID_PARAMETER: return "invalid parameter";
            case physx::PxErrorCode::eINVALID_OPERATION: return "invalid operation";
            case physx::PxErrorCode::eOUT_OF_MEMORY: return "out of memory";
            case physx::PxErrorCode::eINTERNAL_ERROR: return "internal error";
            case physx::PxErrorCode::eABORT: return "abort";
            case physx::PxErrorCode::ePERF_WARNING: return "performance warning";
            default: return "error";
            }
        }
    };
}

#endif // N2ENGINE_PHYSX_ENABLED
