cmake_minimum_required(VERSION 3.25)
project(Sparkles VERSION 1.0.0)

if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../iPlug2" CACHE PATH "iPlug2 root directory")
endif()

# DAW plugin folder used for manual testing. Deploy.cmake (pulled in below) only applies its own
# default when this cache var is unset, so this wins over the stock $LOCALAPPDATA/.../VST3 path.
# Override on the cmake command line with -DIPLUG_VST3_DEPLOY_PATH=... if you need a different target.
if(WIN32)
  set(IPLUG_VST3_DEPLOY_PATH "C:/Users/TheDre/Music/plugins/Jeti" CACHE PATH "Override default VST3 deployment path")
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)

find_package(iPlug2 REQUIRED)

iplug_add_plugin(${PROJECT_NAME}
  SOURCES
    Sparkles.cpp
    Sparkles.h
    resources/resource.h
    core/DetectionParams.h
    core/NoteMatrix.h
    core/SparkleGenerator.h
    params/ParamList.h
    params/ParamRanges.h
    params/ParamSnapshot.h
  RESOURCES
    resources/fonts/Roboto-Regular.ttf
  FORMATS
    MINIMAL_PLUGINS APP WASM
)

# Standalone test binary, deliberately kept free of iPlug2/IGraphics dependencies so it
# builds fast and can run outside a DAW. See tests/test_framework.h.
add_executable(sparkle_tests
  tests/test_main.cpp
  tests/test_note_matrix.cpp
  tests/test_sparkle_generator.cpp
  tests/test_event_scheduler.cpp
  core/NoteMatrix.h
  core/SparkleGenerator.h
  core/EventScheduler.h
)
target_compile_features(sparkle_tests PRIVATE cxx_std_17)

enable_testing()
add_test(NAME sparkle_tests COMMAND sparkle_tests)