cmake_minimum_required(VERSION 3.22.1)

project("cynarranger")

find_package(oboe REQUIRED CONFIG)

# ==========================================
# 1. 头文件路径
# ==========================================
include_directories(
        fluidlite/include
        fluidlite/src
        # 确保子文件夹也能被搜到 (双重保险)
        fluidlite/include/fluidlite
)

# ==========================================
# 2. 收集源码
# ==========================================
file(GLOB_RECURSE FLUIDLITE_SOURCES
        "fluidlite/src/*.c"
        "fluidlite/src/*.cpp"
)

# ==========================================
# 3. >>>>> 关键修复：剔除黑名单 >>>>>
# ==========================================

# 剔除导致 fluidsynth.h 找不到的文件 (我们不需要这个兼容层/命令行入口)
list(REMOVE_ITEM FLUIDLITE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/fluidlite/src/fluidsynth.c")

# 剔除定点数 DSP (保留 float)
list(REMOVE_ITEM FLUIDLITE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/fluidlite/src/fluid_dsp_simple.c")

# 剔除 Windows DLL 入口
list(REMOVE_ITEM FLUIDLITE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/fluidlite/src/fluid_dll.c")

# 剔除 LASH (如果有)
file(GLOB LASH_SOURCES "fluidlite/src/fluid_lash.c")
if(LASH_SOURCES)
    list(REMOVE_ITEM FLUIDLITE_SOURCES ${LASH_SOURCES})
endif()

# ==========================================
# 4. >>>>> 关键修复：强行定义宏 >>>>>
# ==========================================
# 通过编译器参数直接告诉代码：不要用 Vorbis，不要用 OGG！
# 这比修改 fluid_config.h 更暴力、更有效
add_definitions(
        -DFLUID_DA_SUPPORT_OGG=0
        -DHAVE_LIBVORBIS=0
        -DHAVE_STRING_H=1
        -DHAVE_STDLIB_H=1
        -DHAVE_MATH_H=1
        -DFLUID_SAMPLE_FLOAT=1
)

# ==========================================
# 5. 编译与链接
# ==========================================
add_library(
        cynarranger
        SHARED
        native-lib.cpp
        AudioEngine.cpp
        MidiProcessor.cpp
        chord_detect.cpp
        ScaleDetector.cpp
        AutoSustainManager.cpp
        BassEnhancer.cpp
        StyleParser.cpp
        StylePlayer.cpp
        ChordDetector.cpp
        BassAssist.cpp
        ChordTransposer.cpp
        Humanizer.cpp
        BeatTracker.cpp
        RhythmAudioEngine.cpp
        CajonAssistant.cpp
        TempoDetector.cpp
        ${FLUIDLITE_SOURCES}
)

target_link_libraries(
        cynarranger
        oboe::oboe
        log
        android
        m
)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -ffast-math -mfpu=neon")