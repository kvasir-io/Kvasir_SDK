# Kvasir Build Environment

Docker container for building Kvasir firmware projects based on Arch Linux.

## Contents

- **Base**: Arch Linux (rolling release)
- **Compilers**: Clang, GCC
- **Build Tools**: CMake, Make, LLD
- **Development Tools**: Git, Python, Fish shell
- **Libraries**: fmt, nlohmann-json, pugixml, CLI11, Boost
- **Hardware Tools**: J-Link Software
- **Additional**: inja, ftxui, magic_enum

## Usage

This container is automatically built and published monthly or when the Dockerfile changes.

Pull the latest image:
```bash
docker pull kvasirio/build_environment:latest
```

## Tags

- `latest` - Most recent build
- `YYYY.MM.DD` - Date-based tags following Arch Linux ISO versioning

## Repository

Source code and build scripts: [Kvasir on GitHub](https://github.com/kvasir-io/kvasir)