cmake_minimum_required(VERSION 3.23)
project(pqdos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

find_package(isa-l REQUIRED)
find_package(libpqxx REQUIRED)
find_package(botan REQUIRED)
find_package(lmdb REQUIRED)
find_package(msgpack REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(pqdos
    core/ErasureCodec.cpp
    core/Crypto.cpp
    core/MetadataStore.cpp
    core/StorageNode.cpp
    core/StorageNodeServer.cpp
    core/StorageClient.cpp
    core/StorageClientServer.cpp
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
    httplib::httplib
    nlohmann_json::nlohmann_json
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(pqdos_unit_tests
        tests/ErasureCodecTest.cpp
        tests/CryptoTest.cpp
    )

    target_link_libraries(pqdos_unit_tests PRIVATE
        pqdos
        GTest::gtest_main
    )

    add_test(NAME pqdos_unit_tests COMMAND pqdos_unit_tests)

    add_executable(pqdos_integration_tests
        tests/MetadataStoreTest.cpp
        tests/StorageNodeTest.cpp
        tests/StorageNodeServerTest.cpp
        tests/StorageClientTest.cpp
        tests/StorageClientServerTest.cpp
    )

    target_link_libraries(pqdos_integration_tests
        PRIVATE
            pqdos
            GTest::gtest_main
    )

    add_test(
        NAME pqdos_integration_tests
        COMMAND pqdos_integration_tests
    )
endif()

add_executable(storage_node cmd/StorageNodeMain.cpp)
target_link_libraries(storage_node PRIVATE pqdos)

add_executable(storage_client cmd/StorageClientMain.cpp)
target_link_libraries(storage_client PRIVATE pqdos)

install(TARGETS pqdos storage_node storage_client
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)