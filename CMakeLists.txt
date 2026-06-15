cmake_minimum_required(VERSION 3.22)
project(AbramsAssetRegistrarMod)

# Clone RE-UE4SS as a sibling of this folder, checkout tag v3.0.1, then:
#   git submodule update --init --recursive
# From this directory:
#   cmake -B build -G "Visual Studio 17 2022"
#   cmake --build build --config Game__Shipping__Win64
# Copy build/AbramsAssetRegistrar/Game__Shipping__Win64/AbramsAssetRegistrar.dll
#   -> ../dlls/main.dll

add_subdirectory(../RE-UE4SS RE-UE4SS)
add_subdirectory(cpp)
