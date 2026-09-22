cmake_minimum_required(VERSION 3.16)
project(qviewsr_worker LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OPENVINO_ROOT "" CACHE PATH "OpenVINO 2020.3.355 runtime root")
if(NOT EXISTS "${OPENVINO_ROOT}/deployment_tools/inference_engine/include/inference_engine.hpp")
  message(FATAL_ERROR "Set OPENVINO_ROOT to the pinned extracted runtime")
endif()
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.2 REQUIRED)
set(IE "${OPENVINO_ROOT}/deployment_tools/inference_engine")
set(CV "${OPENVINO_ROOT}/opencv")
add_executable(ncs-sr-worker main.cpp)
target_include_directories(ncs-sr-worker SYSTEM PRIVATE "${IE}/include" "${CV}/include")
target_link_libraries(ncs-sr-worker PRIVATE
  "${IE}/lib/intel64/libinference_engine.so" "${IE}/lib/intel64/libinference_engine_legacy.so"
  "${CV}/lib/libopencv_core.so" "${CV}/lib/libopencv_imgproc.so" "${CV}/lib/libopencv_imgcodecs.so"
  Threads::Threads OpenSSL::Crypto nlohmann_json::nlohmann_json)
target_link_options(ncs-sr-worker PRIVATE
  "-Wl,-rpath-link,${IE}/external/tbb/lib" "-Wl,-rpath-link,${OPENVINO_ROOT}/deployment_tools/ngraph/lib")
target_compile_options(ncs-sr-worker PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
set_target_properties(ncs-sr-worker PROPERTIES BUILD_RPATH
  "${IE}/lib/intel64;${IE}/external/tbb/lib;${OPENVINO_ROOT}/deployment_tools/ngraph/lib;${CV}/lib")
enable_testing()
add_executable(tile-plan-test test_tile_plan.cpp)
add_test(NAME TilePlan COMMAND tile-plan-test)
