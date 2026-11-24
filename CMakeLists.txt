cmake_minimum_required(VERSION 3.22)
set(SC_MODULE s3)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-s3" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_S3_BUILD_TESTS "Build simply-cpp-s3 tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc::config and sc::params; libcurl (curl license) for HTTP(S); OpenSSL's libcrypto for SHA-256, HMAC and MD5.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 4.0.0)
find_or_install_package(CURL libcurl4-openssl-dev curl)
find_or_install_package(OpenSSL libssl-dev openssl 3.0)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(bytes LINK_LIBRARIES sc::sc-core PUBLIC_LINK_LIBRARIES sc::sc-core)
add_sc_object(checksum LINK_LIBRARIES sc::sc-core OpenSSL::Crypto PUBLIC_LINK_LIBRARIES sc::sc-core OpenSSL::Crypto)
add_sc_object(xml)
add_sc_object(signer)
add_sc_object(settings)
# libcurl by name, as sc-http links it: CURL::libcurl's include directory (the macOS SDK's usr/include) would come
# before libc++'s headers in every module using sc-s3.
add_sc_object(client SOURCES src/http.cpp src/http.h LINK_LIBRARIES sc::sc-core curl PUBLIC_LINK_LIBRARIES sc::sc-core curl)
add_sc_object(sts LINK_LIBRARIES curl)

add_sc_libraries(DESCRIPTION "simply-cpp S3 client on libcurl: SigV4, put and streaming multipart upload from a pipe, get, list, head, delete, Glacier/Deep Archive restore, STS AssumeRole, rate limit")
target_compile_features(sc-s3 PUBLIC cxx_std_20)
target_compile_features(sc-s3-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-s3
        COMPONENT development)

if (SC_S3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-s3-test)
endif ()

# sc-s3, the program: put (a file or standard input, of any length), get, ls, head, restore, rm.
add_executable(sc-s3-tool apps/sc-s3.cpp)
set_target_properties(sc-s3-tool PROPERTIES OUTPUT_NAME sc-s3)
target_link_libraries(sc-s3-tool PRIVATE sc::sc-s3-shared)
target_compile_definitions(sc-s3-tool PRIVATE SC_S3_VERSION="${SC_VERSION}")
sc_program_info(sc-s3-tool)
install(TARGETS sc-s3-tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tools)

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 4.0.0~)" libcurl4-openssl-dev libssl-dev)
