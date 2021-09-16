#!/bin/sh
set -e

OPTION=$1
BUILD_CONFIG=debug

if [ "$OPTION" = "release" ]; then
    BUILD_CONFIG=release
fi

clone_git()
{
    if [ ! -d $1 ]; then
        echo "Cloning $1."
        git clone $2 $1
    fi
}

dep_glfw()
{
    # GLFW requires CMake
    GLFW_GIT=https://github.com/glfw/glfw
    GLFW_LIB_FILE=libglfw3.a # MSYS seems to always output .a files

    if [ ! -d glfw ]; then
        echo "Cloning GLFW."
        git clone $GLFW_GIT
    fi

    if [ ! -f glfw_build/src/${GLFW_LIB_FILE} ]; then
        mkdir -p glfw_build
        pushd . &>/dev/null
        cd glfw_build
        cmake -G "$BUILD_PLATFORM_CMAKE_GENERATOR" ../glfw -DBUILD_SHARED_LIBS=OFF
        make glfw
        popd &>/dev/null
    fi
}

dep_sokol()
{
    # nothing to build, just clone it
    SOKOL_GIT=https://github.com/floooh/sokol
    if [ ! -d sokol ]; then
        echo "Cloning sokol."
        git clone $SOKOL_GIT
    fi
}

dep_rive()
{
    RIVE_GIT=https://github.com/rive-app/rive-cpp.git
    RIVE_LIB_FILE=${BUILD_PLATFORM_LIB_PREFIX}rive.${BUILD_PLATFORM_LIB_EXT}

    if [ ! -d rive-cpp ]; then
        echo "Cloning rive."
        git clone $RIVE_GIT
    fi

    if [ ! -f rive-cpp/build/bin/${BUILD_CONFIG}/${RIVE_LIB_FILE} ]; then
        echo "Building rive."
        pushd . &>/dev/null
        cd rive-cpp
        #git checkout .
        #git checkout low_level_rendering
        # git apply --ignore-space-change ../../../third-party/rive-cpp.patch

        cd build
        premake5 gmake2 --with-low-level-rendering
        make config=${BUILD_CONFIG} -j7
        popd &>/dev/null
    fi
}

dep_jc_containers()
{
    JC_CONTAINERS_GIT=https://github.com/JCash/containers.git
    if [ ! -d jc_containers ]; then
        echo "Cloning jc-containers."
        git clone $JC_CONTAINERS_GIT jc_containers
    fi
}

dep_libtess2()
{
    LIBTESS2_GIT=https://github.com/memononen/libtess2.git
    LIBTESS2_CONFIG=debug_native
    LIBTESS2_LIB_FILE_IN=${BUILD_PLATFORM_LIB_PREFIX}tess2.${BUILD_PLATFORM_LIB_EXT}
    LIBTESS2_LIB_FILE_OUT=${BUILD_PLATFORM_LIB_PREFIX}tess2_${BUILD_CONFIG}.${BUILD_PLATFORM_LIB_EXT}

    if [ "$BUILD_CONFIG" = "release" ]; then
        LIBTESS2_CONFIG=release_native
    fi

    if [ ! -d libtess2 ]; then
        echo "Cloning libtess2."
        git clone $LIBTESS2_GIT
    fi

    if [ ! -f libtess2/Build/${LIBTESS2_LIB_FILE_OUT} ]; then
        echo "Building libtess2."
        pushd . &>/dev/null
        cd libtess2
        premake5 gmake2
        cd Build
        make tess2 config=${LIBTESS2_CONFIG} -j7
        mv ${LIBTESS2_LIB_FILE_IN} ${LIBTESS2_LIB_FILE_OUT}
        popd &>/dev/null
    fi
}

dep_linmath()
{
    LINMATH_GIT=https://github.com/datenwolf/linmath.h.git
    clone_git linmath.h $LINMATH_GIT
}

dep_imgui()
{
    IMGUI_GIT=https://github.com/ocornut/imgui
    clone_git imgui $IMGUI_GIT
}

if [ "$OPTION" = "clean" ]; then
    rm -rf build/dependencies
    echo "Done cleaning dependencies."
else
    mkdir -p build/dependencies
    pushd build/dependencies &>/dev/null

    dep_glfw
    dep_sokol
    dep_rive
    dep_jc_containers
    dep_libtess2
    dep_linmath
    dep_imgui

    popd &>/dev/null

    echo "Done building dependencies."
fi
