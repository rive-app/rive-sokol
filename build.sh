#!/bin/sh

OPTION1=$1
OPTION2=$2

. ./setup.sh

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
    $SHDC_CMD --input assets/shaders.glsl --output src/shaders.glsl.h --slang glsl330
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
    platform_setup
    ./build-dependencies.sh $OPTION1
    build_assets
    make config=release -j7
else
    platform_setup
    ./build-dependencies.sh $OPTION1
    if [ "$OPTION1" != "deps" ]; then
        build_assets
        make -j7
    fi
fi

echo Done building.
