# syntax=docker/dockerfile:1
#
# The N2Engine editor host (N2EditorHost) on Ubuntu 24.04, for x86_64 and arm64 (docker buildx build --platform
# linux/amd64,linux/arm64 works; each architecture builds natively or under emulation).
#
# Build from the repository root:   docker build -f deploy/Dockerfile -t n2editorhost .
# Run it: see docs/editor.html#linux-host (a project volume, a token, a loopback-only published port).
#
# The build steps are the ones of the "linux (Release, x86_64, Clang)" CI job: Clang 20 from apt.llvm.org on the
# libstdc++ 14 headers (--gcc-install-dir). GCC-built hosts are unsupported until the sol2 same-signature-lambda bug is
# fixed. PhysX is OFF on Linux (no binaries yet); on arm64 the math library runs its scalar code (no NEON).

FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl git \
 && install -d -m 0755 /etc/apt/keyrings \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/apt.llvm.org.asc \
 && echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends clang-20 libstdc++-14-dev cmake ninja-build python3 \
        libvulkan-dev libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libasound2-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# uname -m is x86_64 or aarch64, the directory name GCC's libstdc++ 14 lives under on each
ENV CC=clang-20 CXX=clang++-20
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="--gcc-install-dir=/usr/lib/gcc/$(uname -m)-linux-gnu/14" \
        -DN2ENGINE_BUILD_TESTS=OFF -DN2ENGINE_USE_PHYSX=OFF \
        -DN2ENGINE_EDITOR_TLS=ON -DN2ENGINE_TEXT_FREETYPE=ON -DN2ENGINE_MODEL_UFBX=ON \
 && cmake --build build --parallel --target N2EditorHost

FROM ubuntu:24.04 AS runtime

# Shared libraries the host links (the X11, GL and ALSA ones come with the renderer and audio; with --renderer
# software nothing opens a window or a sound device). The ldd check below fails the build if one is missing.
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 libgl1 libvulkan1 libx11-6 libxrandr2 libxinerama1 libxcursor1 libxi6 libasound2t64 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/bin/N2EditorHost /usr/local/bin/N2EditorHost
RUN ldd /usr/local/bin/N2EditorHost | (! grep "not found")

# A non-root user. XDG_DATA_HOME is where the host keeps its TLS identity (<XDG_DATA_HOME>/n2engine/editor-tls); with
# neither XDG_DATA_HOME nor HOME set, --tls refuses to start. Both folders are volumes, owned by the user, so a new
# named volume starts out writable.
RUN useradd --system --uid 10001 --home-dir /var/lib/n2engine --shell /usr/sbin/nologin n2 \
 && install -d -o n2 -g n2 -m 0750 /project /var/lib/n2engine
ENV XDG_DATA_HOME=/var/lib/n2engine \
    ALSOFT_DRIVERS=null
VOLUME ["/project", "/var/lib/n2engine"]
USER n2
WORKDIR /project

# Inside a container the host must listen on 0.0.0.0 for a published port to reach it; keep that port on the Docker
# host's loopback (-p 127.0.0.1:9999:9999) and put an SSH tunnel or a TLS proxy in front. Without N2_EDITOR_TOKEN set
# (docker run -e N2_EDITOR_TOKEN, or --env-file) the host refuses to start rather than run without authentication.
# Do not add --exit-on-stdin-eof: a container's stdin is /dev/null or closed and the host would exit at once.
EXPOSE 9999
ENTRYPOINT ["N2EditorHost"]
CMD ["--bind", "0.0.0.0", "--port", "9999", "--project", "/project", "--renderer", "software", "--tls", "--token-env", "N2_EDITOR_TOKEN"]
