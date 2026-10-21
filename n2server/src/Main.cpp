#include "n2server/Server.hpp"

// N2Server: the headless dedicated server. Everything it does is Server::RunServer, in the N2ServerCore library, so the
// parts that need no engine start (the arguments, the token generator) can be unit tested.
int main(int argc, char *argv[])
{
    return N2Engine::Server::RunServer(argc, argv);
}
