cmake_minimum_required(VERSION 3.23)
project(pqdos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

find_package(isa-l REQUIRED)
find_package(libpqxx REQUIRED)
find_package(botan REQUIRED)
find_package(lmdb REQUIRED)
find_package(msgpack REQUIRED)

add_library(pqdos
    core/ErasureCodec.cpp
    core/Crypto.cpp
)

target_include_directories(pqdos PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(pqdos PUBLIC
    isa-l::isa-l
    libpqxx::pqxx
    botan::botan
    lmdb::lmdb
    msgpack-cxx
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(pqdos_tests
        tests/ErasureCodecTest.cpp
        tests/CryptoTest.cpp
    )

    target_link_libraries(pqdos_tests PRIVATE
        pqdos
        GTest::gtest_main
    )

    add_test(NAME pqdos_tests COMMAND pqdos_tests)
endif()

install(TARGETS pqdos
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)