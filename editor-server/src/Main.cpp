#include "editor-server/Host.hpp"

// N2EditorHost: the stock editor host. Everything it does is Editor::RunHost, in the N2EditorServer library, so a game
// with its own C++ components can build its own host the same way: register them, then call RunHost.
int main(int argc, char *argv[])
{
    return N2Engine::Editor::RunHost(argc, argv);
}
