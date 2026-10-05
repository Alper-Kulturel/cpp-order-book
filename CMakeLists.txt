cmake_minimum_required(VERSION 3.16)

project(cpp_order_book
    VERSION 1.0.0
    DESCRIPTION "Price/time-priority limit order book with Python bindings"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Targets
#   lob          static library -- the order book itself, no Python involved
#   lob_bench    native benchmark binary
#   order_book_cpp / bench_driver
#                pybind11 extension modules, copied into python/ after build
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Keep the exported compile commands usable for tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_compile_options($<$<CONFIG:Release>:/O2>)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    # The book is the hot path, so optimise it hard. -fno-math-errno lets the
    # compiler fold the std::isfinite checks in the price validation.
    add_compile_options($<$<CONFIG:Release>:-O3>)
    add_compile_options($<$<CONFIG:Release>:-fno-math-errno>)
    # Tune for the build machine. Comment this out if you ship the artefact.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" LOB_HAS_MARCH_NATIVE)
    if(LOB_HAS_MARCH_NATIVE)
        add_compile_options($<$<CONFIG:Release>:-march=native>)
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

add_library(lob STATIC
    cpp/src/order_book.cpp)

target_include_directories(lob PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cpp/include")

# ---------------------------------------------------------------------------
# Native benchmark (no Python)
# ---------------------------------------------------------------------------

add_executable(lob_bench cpp/src/bench_main.cpp)
target_link_libraries(lob_bench PRIVATE lob)

# ---------------------------------------------------------------------------
# Python bindings
# ---------------------------------------------------------------------------

# Locate pybind11. Prefer an explicit -Dpybind11_DIR=..., otherwise ask the
# interpreter we are building against.
if(NOT pybind11_DIR)
    # CMake's own Python3 search does not reliably land on the interpreter that
    # is first on PATH: it can settle on a system or framework install instead,
    # and the pybind11 probe below then inspects the wrong site-packages. When a
    # virtualenv or conda environment is active, that environment is the clear
    # intent, so name its interpreter up front. A caller-supplied
    # -DPython3_EXECUTABLE=... still wins, as it is already set by this point.
    if(NOT Python3_EXECUTABLE)
        foreach(_lob_env_root "$ENV{VIRTUAL_ENV}" "$ENV{CONDA_PREFIX}")
            if(_lob_env_root)
                # POSIX venvs and conda put the interpreter in bin/; Windows
                # venvs use Scripts/.
                foreach(_lob_py "bin/python3" "bin/python" "Scripts/python.exe")
                    if(EXISTS "${_lob_env_root}/${_lob_py}")
                        set(Python3_EXECUTABLE "${_lob_env_root}/${_lob_py}")
                        break()
                    endif()
                endforeach()
                if(Python3_EXECUTABLE)
                    message(STATUS
                        "Using active environment interpreter ${Python3_EXECUTABLE}")
                    break()
                endif()
            endif()
        endforeach()
        unset(_lob_env_root)
        unset(_lob_py)
    endif()

    find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
    if(Python3_EXECUTABLE)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
            OUTPUT_VARIABLE LOB_PYBIND11_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE LOB_PYBIND11_LOOKUP)
        if(LOB_PYBIND11_LOOKUP EQUAL 0 AND LOB_PYBIND11_DIR)
            set(pybind11_DIR "${LOB_PYBIND11_DIR}")
            message(STATUS "pybind11 found via ${Python3_EXECUTABLE}")
        else()
            message(WARNING
                "Could not ask ${Python3_EXECUTABLE} for pybind11. Install it "
                "with `pip install pybind11` into that interpreter, activate "
                "the environment you want to build against, or pass "
                "-Dpybind11_DIR=...")
        endif()
    endif()
endif()

find_package(pybind11 CONFIG REQUIRED)

# Everything a Python extension needs to exist as a loadable module.
function(lob_add_module name source)
    pybind11_add_module(${name} ${source})
    target_link_libraries(${name} PRIVATE lob)
    # Drop the built extension next to the Python sources so `python3 cli.py`
    # and `pytest` work straight after a build, with no manual copy step.
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/python"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${name}>" "${CMAKE_CURRENT_SOURCE_DIR}/python/"
        COMMENT "Copied $<TARGET_FILE_NAME:${name}> to python/")
endfunction()

lob_add_module(order_book_cpp cpp/bindings.cpp)
lob_add_module(bench_driver cpp/bench_driver.cpp)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "cpp-order-book ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  python     : ${Python3_EXECUTABLE}")
message(STATUS "  pybind11   : ${pybind11_VERSION}")
message(STATUS "")
