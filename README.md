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
- G++ -> GCC 11
  - Windows: w64devkit or some other environment which provides a version of G++
  - Linux: Install your distribution's package
    - Debian/Ubuntu: ```sudo apt install build-essential```
    - Arch: ```sudo pacman -S base-devel```
    - Gentoo: you already have it