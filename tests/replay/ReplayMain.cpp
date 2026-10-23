// n2_replay: the headless replay runner (netcode C2). Replays each given replay file and prints a hash every
// hashInterval ticks, one "[REPLAY] <scene> tick <n> <hash>" line each. With --out it also writes the replay-hashes.json
// the CI legs upload (the file scripts/replay-determinism.py compares). Exit 0 when every replay ran, 1 otherwise.
//
//   n2_replay tests/golden/replays/lua_sim.n2replay.json [more files] [--leg <name>] [--out <hashes.json>]

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "engine/replay/GoldenReplay.hpp"

using namespace N2Engine::Replay;

int main(int argc, char **argv)
{
    std::vector<std::string> files;
    std::string leg = "local";
    std::string out;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--leg" && i + 1 < argc)
        {
            leg = argv[++i];
        }
        else if (arg == "--out" && i + 1 < argc)
        {
            out = argv[++i];
        }
        else
        {
            files.push_back(arg);
        }
    }
    if (files.empty())
    {
        std::fprintf(stderr, "usage: n2_replay <replay.json>... [--leg <name>] [--out <hashes.json>]\n");
        return 2;
    }

    int exitCode = 0;
    int ticks = 0;
    int interval = 0;
    std::vector<NamedResult> results;
    for (const std::string &path : files)
    {
        std::string error;
        const auto log = LoadReplayLog(path, error);
        if (!log)
        {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
            exitCode = 1;
            continue;
        }
        ticks = log->ticks;
        interval = log->hashInterval;
        NamedResult named;
        named.name = log->scene;
        named.result = RunReplay(*log);
        if (!named.result.ok)
        {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), named.result.error.c_str());
            exitCode = 1;
        }
        for (const ReplayHash &entry : named.result.hashes)
        {
            std::printf("[REPLAY] %s tick %d %s\n", named.name.c_str(), entry.tick, FormatHash(entry.hash).c_str());
        }
        results.push_back(std::move(named));
    }
    std::fflush(stdout);

    if (!out.empty())
    {
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        file << HashesToJson(leg, results, ticks, interval);
        file.flush();
        if (!file)
        {
            std::fprintf(stderr, "could not write %s\n", out.c_str());
            exitCode = 1;
        }
    }
    return exitCode;
}
