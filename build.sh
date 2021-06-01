#!/bin/sh

OPTION1=$1
OPTION2=$1
export TOOLS_DIR=build/tools
export PLATFORM_OSX="macos"
export PLATFORM_WINDOWS="win32"

if [ "$(uname)" == "Darwin" ]; then
    export BUILD_PLATFORM=$PLATFORM_OSX
    export BUILD_PLATFORM_LIB_PREFIX="lib"
    export BUILD_PLATFORM_LIB_EXT="a"
    export BUILD_PLATFORM_CMAKE_GENERATOR="Unix Makefiles"
elif [ "$(expr substr $(uname -s) 1 10)" == "MINGW64_NT" ]; then
    export BUILD_PLATFORM=$PLATFORM_WINDOWS
    export BUILD_PLATFORM_LIB_EXT="lib"
    export BUILD_PLATFORM_CMAKE_GENERATOR="MSYS Makefiles"
    export CC=gcc
    export CXX=g++
fi

tools_setup()
{
    mkdir -p ${TOOLS_DIR}/${BUILD_PLATFORM}
    export PATH=$PATH:${PWD}/${TOOLS_DIR}/${BUILD_PLATFORM}
    if [ "$BUILD_PLATFORM" = ${PLATFORM_OSX} ]; then
        if [ ! -f ${TOOLS_DIR}/${BUILD_PLATFORM}/premake5 ]; then
            wget https://github.com/premake/premake-core/releases/download/v5.0.0-alpha16/premake-5.0.0-alpha16-macosx.tar.gz -O ${TOOLS_DIR}/${BUILD_PLATFORM}/premake-5.0.0-alpha16-macosx.tar.gz
            tar -xzf ${TOOLS_DIR}/${BUILD_PLATFORM}/premake-5.0.0-alpha16-macosx.tar.gz -C ${TOOLS_DIR}/${BUILD_PLATFORM}/
            chmod +x ${TOOLS_DIR}/${BUILD_PLATFORM}/premake5
        fi

        if [ ! -f ${TOOLS_DIR}/${BUILD_PLATFORM}/sokol-shdc ]; then
            wget https://github.com/floooh/sokol-tools-bin/raw/master/bin/osx/sokol-shdc -O ${TOOLS_DIR}/${BUILD_PLATFORM}/sokol-shdc
            chmod +x ${TOOLS_DIR}/${BUILD_PLATFORM}/sokol-shdc
        fi

        SHDC_CMD=${TOOLS_DIR}/${BUILD_PLATFORM}/sokol-shdc

    elif [ "$BUILD_PLATFORM" = ${PLATFORM_WINDOWS} ]; then
        if [ ! -f ${TOOLS_DIR}/${BUILD_PLATFORM}/premake5.exe ]; then
            curl -L https://github.com/premake/premake-core/releases/download/v5.0.0-alpha16/premake-5.0.0-alpha16-windows.zip --output ${TOOLS_DIR}/${BUILD_PLATFORM}/premake-5.0.0-alpha16-windows.zip
            unzip ${TOOLS_DIR}/${BUILD_PLATFORM}/premake-5.0.0-alpha16-windows.zip -d ${TOOLS_DIR}/${BUILD_PLATFORM}/
            chmod +x ${TOOLS_DIR}/${BUILD_PLATFORM}/premake5.exe
        fi

        if [ ! -f ${TOOLS_DIR}/${BUILD_PLATFORM}/sokol-shdc.exe ]; then
            curl -L https://github.com/floooh/sokol-tools-bin/raw/master/bin/win32/sokol-shdc.exe --output ${TOOLS_DIR}/${BUILD_PLATFORM}/sokol-shdc.exe
            chmod +x ${TOOLS_DIR}/${BUILD_PLATFORM}/sokol-shdc.exe
        fi

        SHDC_CMD=${TOOLS_DIR}/${BUILD_PLATFORM}/sokol-shdc.exe
    fi
}

platform_setup()
{
    if [ "$BUILD_PLATFORM" = ${PLATFORM_OSX} ]; then
        export PLATFORM_LIBS="-framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL"
    elif [ "$BUILD_PLATFORM" = ${PLATFORM_WINDOWS} ]; then
        export PLATFORM_LIBS="-lglfw3 -lgdi32 -lopengl32 -lImm32"
        export PLATFORM_LDFLAGS="-static-libgcc -static-libstdc++"
    fi
}

build_assets()
{
    echo "Building assets"
    $SHDC_CMD --input assets/shader.glsl --output src/rive/shader.glsl.h --slang glsl330
}

if [ "$OPTION1" = "clean" ]; then
    if [ "$OPTION2" = "deps" ]; then
        echo "Cleaning dependencies"
        ./build-dependencies.sh $OPTION1
    else
        echo "Cleaning project"
        make clean
    fi
elif [ "$OPTION1" = "release" ]; then
    tools_setup
    platform_setup
    ./build-dependencies.sh $OPTION1
    build_assets
    make config=release -j7
else
    tools_setup
    platform_setup
    ./build-dependencies.sh $OPTION1
    build_assets
    make -j7
fi

echo Done building.
