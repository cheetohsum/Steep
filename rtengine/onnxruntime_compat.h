/*
 *  ONNX Runtime compatibility wrapper for MinGW/GCC on Windows.
 *
 *  The ONNX Runtime C API header uses ORT_API_CALL (__stdcall) in function
 *  pointer declarations within structs. GCC/MinGW cannot parse this syntax
 *  (e.g., "int(ORT_API_CALL* Fn)(...)"). Since all calls go through the
 *  OrtApi vtable, the calling convention annotation is not needed at
 *  compile time.
 */
#pragma once

#if defined(_WIN32) && defined(__MINGW32__)
#define ORT_API_CALL
#endif

#include <onnxruntime_c_api.h>
