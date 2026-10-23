Accepted by the user 2026-10-13:
- GLFW, GoogleTest and OpenAL Soft copyright holders left out (not in repo)
- OpenAL licence stated as LGPL with COPYING in its tree, no version
- FreeType credit year 2024 kept and marked to be confirmed
- Added one disclaimer sentence 'not legal advice' to the intro
- Mbed TLS, mikktspace holders verified in repo and added
- Review fixes: PhysX row lists NVIDIA, AGEIA and NovodeX lines; FreeType release date dropped; OpenAL Windows DLL name marked to be confirmed
- Review fix: the sol2 row now says '3.2.3 with local patches' and names the two N2Engine patches in sol.hpp (optional<T&>::emplace body; GCC unique lambda type names); no other locally modified vendored file found in git history or by marker grep (not byte-compared against upstream); master (#150) merged into the branch.
