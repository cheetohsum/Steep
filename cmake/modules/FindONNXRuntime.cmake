# FindONNXRuntime.cmake
# Find the ONNX Runtime library
#
# This module defines:
#   ONNXRuntime_FOUND        - System has ONNX Runtime
#   ONNXRuntime_INCLUDE_DIRS - ONNX Runtime include directories
#   ONNXRuntime_LIBRARIES    - Libraries needed to use ONNX Runtime
#   onnxruntime::onnxruntime - Imported target for ONNX Runtime
#
# The following variables can be set to guide the search:
#   ONNXRuntime_ROOT         - Root directory of ONNX Runtime installation
#   ONNXRUNTIME_ROOT_DIR     - Alternative root directory

find_path(ONNXRuntime_INCLUDE_DIR
    NAMES onnxruntime_c_api.h
    PATHS
        ${ONNXRuntime_ROOT}
        ${ONNXRUNTIME_ROOT_DIR}
        /usr/local
        /usr
        /opt/onnxruntime
    PATH_SUFFIXES
        include
        include/onnxruntime
        include/onnxruntime/core/session
)

find_library(ONNXRuntime_LIBRARY
    NAMES onnxruntime
    PATHS
        ${ONNXRuntime_ROOT}
        ${ONNXRUNTIME_ROOT_DIR}
        /usr/local
        /usr
        /opt/onnxruntime
    PATH_SUFFIXES
        lib
        lib64
        lib/x86_64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ONNXRuntime
    REQUIRED_VARS ONNXRuntime_LIBRARY ONNXRuntime_INCLUDE_DIR
)

if(ONNXRuntime_FOUND)
    set(ONNXRuntime_INCLUDE_DIRS ${ONNXRuntime_INCLUDE_DIR})
    set(ONNXRuntime_LIBRARIES ${ONNXRuntime_LIBRARY})
    set(ONNXRuntime_DML_FOUND FALSE)

    if(WIN32)
        find_path(ONNXRuntime_DML_INCLUDE_DIR
            NAMES dml_provider_factory.h
            PATHS
                ${ONNXRuntime_ROOT}
                ${ONNXRUNTIME_ROOT_DIR}
                ${ONNXRuntime_INCLUDE_DIR}
            PATH_SUFFIXES
                include
        )
        if(ONNXRuntime_DML_INCLUDE_DIR)
            set(ONNXRuntime_DML_FOUND TRUE)
        endif()
    endif()

    if(NOT TARGET onnxruntime::onnxruntime)
        if(WIN32)
            # On Windows, ONNX Runtime ships onnxruntime.lib (import lib) +
            # onnxruntime.dll.  Use SHARED IMPORTED so IMPORTED_IMPLIB works.
            add_library(onnxruntime::onnxruntime SHARED IMPORTED)
            set_target_properties(onnxruntime::onnxruntime PROPERTIES
                IMPORTED_IMPLIB "${ONNXRuntime_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ONNXRuntime_INCLUDE_DIR}"
            )
            # Locate the DLL for runtime bundling.
            find_file(ONNXRuntime_DLL
                NAMES onnxruntime.dll
                PATHS
                    ${ONNXRuntime_ROOT}
                    ${ONNXRUNTIME_ROOT_DIR}
                PATH_SUFFIXES
                    lib
                    bin
            )
            if(ONNXRuntime_DLL)
                set_target_properties(onnxruntime::onnxruntime PROPERTIES
                    IMPORTED_LOCATION "${ONNXRuntime_DLL}"
                )
            endif()
        else()
            add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
            set_target_properties(onnxruntime::onnxruntime PROPERTIES
                IMPORTED_LOCATION "${ONNXRuntime_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ONNXRuntime_INCLUDE_DIR}"
            )
        endif()
    endif()
endif()

mark_as_advanced(
    ONNXRuntime_INCLUDE_DIR
    ONNXRuntime_LIBRARY
    ONNXRuntime_DLL
    ONNXRuntime_DML_INCLUDE_DIR
)
