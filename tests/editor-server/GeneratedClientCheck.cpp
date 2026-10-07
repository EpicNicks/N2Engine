// Compiled, never linked: the generated C++ client (editor-server/clients/cpp/Protocol.generated.hpp) isn't used by
// the server, so without this nothing would notice it stop compiling. It declares structs with the same names as the
// server's Commands.hpp (both in N2Engine::Editor::Protocol), so it can't share a program with it.
#include <Protocol.generated.hpp>

#include <editor-server/Protocol.hpp>

#include <string_view>

namespace Generated = N2Engine::Editor::Protocol;
namespace Server = N2Engine::Editor;

static_assert(std::string_view(Generated::ProtocolVersion) == Server::ProtocolVersion);
static_assert(static_cast<int>(Generated::CommandType::Hello) == static_cast<int>(Server::CommandType::Hello));
static_assert(static_cast<int>(Generated::ResponseType::ServerInfo) == static_cast<int>(Server::ResponseType::ServerInfo));
static_assert(static_cast<int>(Generated::CommandType::PollEvents) == static_cast<int>(Server::CommandType::PollEvents));
static_assert(static_cast<int>(Generated::ResponseType::Events) == static_cast<int>(Server::ResponseType::Events));
static_assert(sizeof(Generated::Quat) == 4 * sizeof(float));
static_assert(sizeof(Generated::ServerInfoData::capabilities) > 0);
