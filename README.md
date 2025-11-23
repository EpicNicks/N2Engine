# N2Engine
A C++ Game Engine I am building for learning
- Software Architecture + Game Engine Architecture
- Advanced C++
- Asset management + Serialization + Versioning
- Graphics backend programming + Graphics Programming
- Integration and wrapping of Physics Engines

## Currently Implemented
- Component System
- Main Loop Scheduling
- Scene Management and Execution System
- Coroutine Scheduling
- Advanced Input System
- Scene Serialization/Deserialization
- Math types with SIMD

## Objectives
- create a functioning game engine in C++
    - develop my software architecture skills
    - develop my C++ skills
    - develop my Math knowledge
    - improve upon my C# engine's architecture
- create game engine systems and APIs for:
    - Component System
    - Asset Management, Serialization/Deserialization
    - Coroutines
    - Object Structure Prefabs
    - Scene Management
    - Advanced Input System
    - Math types with SIMD
- leverage and integrate solutions for
    - Rendering
    - Physics
    - Input and Windowing
  - Scripting languages
    - Lua

### Stretch Goals
- create a functioning game engine editor
- create a graphics backend using vulkan

## Requirements
- [The Vulkan SDK](https://vulkan.lunarg.com/)
- CMake
  - [Windows](https://cmake.org/download/)
  - Linux: Install your distribution's package
    - Debian/Ubuntu: ```sudo apt install cmake```
    - Arch: ```sudo pacman -S cmake```
    - Gentoo: ```emerge dev-build/cmake```
- Building PhysX
  - CMake should download the binaries this project uses for PhysX
  - if you wish to build your own binaries you will need:
    - [Cuda SDK](https://developer.nvidia.com/cuda-toolkit-archive) (version 12.X, 13 was unsupported at last build)
    - [Nvidia PhysX SDK](https://github.com/NVIDIA-Omniverse/PhysX/releases/tag/107.3-omni-and-physx-5.6.1)
    - Visual Studio Visual C++ compiler
  - Install the Cuda SDK
  - Generate the project files in the Nvidia SDK folder \<root>/physx/generate_projects.bat
    - select GPU support to enable GPU support in N2Engine, choose Visual Studio project
    - Build the project with Visual Studio
    - place the built files (bin and replace include folder headers) in N2Engine project folder external/PhysX/physx
- G++ -> GCC 11
  - Windows: w64devkit or some other environment which provides a version of G++
  - Linux: Install your distribution's package
    - Debian/Ubuntu: ```sudo apt install build-essential```
    - Arch: ```sudo pacman -S base-devel```
    - Gentoo: you already have it