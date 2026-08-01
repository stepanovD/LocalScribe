cmake_minimum_required(VERSION 3.20)

project(LocalScribeCore LANGUAGES C CXX)

option(LOCALSCRIBE_BUILD_TESTS "Build portable LocalScribe core tests" ON)
option(
    LOCALSCRIBE_ENABLE_WHISPER
    "Build the optional local whisper.cpp ASR adapter"
    OFF)
option(
    LOCALSCRIBE_BUILD_SOAK_RUNNER
    "Build the deterministic portable core soak runner"
    ON)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

set(LOCALSCRIBE_CORE_SOURCES
    Core/src/abi/LocalScribeCore.cpp
    Core/src/inference/FixtureAsrBackend.cpp
    Core/src/inference/SourceDiarizationBackend.cpp
    Core/src/output/MarkdownRenderer.cpp
    Core/src/session/SessionStateMachine.cpp
    Core/src/storage/Migrations.cpp
    Core/src/storage/RecoveryJournal.cpp)

if(LOCALSCRIBE_ENABLE_WHISPER)
    list(APPEND
        LOCALSCRIBE_CORE_SOURCES
        Core/src/inference/WhisperCppBackend.cpp)
endif()

add_library(LocalScribeCore STATIC ${LOCALSCRIBE_CORE_SOURCES})
add_library(LocalScribe::Core ALIAS LocalScribeCore)

target_compile_features(LocalScribeCore PUBLIC cxx_std_20)
target_compile_definitions(
    LocalScribeCore
    PRIVATE LS_CORE_BUILDING=1)
target_include_directories(
    LocalScribeCore
    PUBLIC Core/include
    PRIVATE Core/src)
target_link_libraries(
    LocalScribeCore
    PRIVATE SQLite::SQLite3 Threads::Threads)

if(LOCALSCRIBE_ENABLE_WHISPER)
    target_compile_definitions(
        LocalScribeCore
        PRIVATE LOCALSCRIBE_ENABLE_WHISPER=1)
    if(APPLE)
        find_library(
            LOCALSCRIBE_WHISPER_FRAMEWORK
            NAMES whisper
            PATHS
                "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/whisper.xcframework/macos-arm64_x86_64"
            NO_DEFAULT_PATH)
    else()
        find_library(LOCALSCRIBE_WHISPER_FRAMEWORK NAMES whisper)
        find_path(
            LOCALSCRIBE_WHISPER_INCLUDE_DIR
            NAMES whisper/whisper.h)
        if(LOCALSCRIBE_WHISPER_INCLUDE_DIR)
            target_include_directories(
                LocalScribeCore
                PRIVATE "${LOCALSCRIBE_WHISPER_INCLUDE_DIR}")
        endif()
    endif()
    if(NOT LOCALSCRIBE_WHISPER_FRAMEWORK)
        message(
            FATAL_ERROR
            "LOCALSCRIBE_ENABLE_WHISPER requires a local whisper library")
    endif()
    target_link_libraries(
        LocalScribeCore
        PRIVATE "${LOCALSCRIBE_WHISPER_FRAMEWORK}")
endif()

if(LOCALSCRIBE_BUILD_TESTS)
    enable_testing()
    add_executable(
        LocalScribeCoreTests
        Core/tests/TestMain.cpp
        Core/tests/BackendTests.cpp
        Core/tests/ContractTests.cpp
        Core/tests/JournalRecoveryTests.cpp
        Core/tests/MarkdownGoldenTests.cpp
        Core/tests/StateMachineTests.cpp
        Core/tests/VoiceProfileTests.cpp)
    target_compile_features(LocalScribeCoreTests PRIVATE cxx_std_20)
    target_include_directories(
        LocalScribeCoreTests
        PRIVATE Core/src)
    target_compile_definitions(
        LocalScribeCoreTests
        PRIVATE
            LOCALSCRIBE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(
        LocalScribeCoreTests
        PRIVATE LocalScribe::Core SQLite::SQLite3 Threads::Threads)
    add_test(NAME LocalScribeCoreTests COMMAND LocalScribeCoreTests)
endif()

if(LOCALSCRIBE_BUILD_SOAK_RUNNER)
    add_executable(
        LocalScribeCoreSoak
        Tools/CoreSoak/main.cpp)
    target_compile_features(LocalScribeCoreSoak PRIVATE cxx_std_20)
    target_link_libraries(
        LocalScribeCoreSoak
        PRIVATE LocalScribe::Core SQLite::SQLite3 Threads::Threads)
endif()
