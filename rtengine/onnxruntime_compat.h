/*
 *  ONNX Runtime compatibility wrapper for MinGW/GCC on Windows.
 *
 *  The ONNX Runtime C API header uses ORT_API_CALL (__stdcall) in function
 *  pointer declarations within structs. GCC/MinGW cannot parse this syntax
 *  (e.g., "int(ORT_API_CALL* Fn)(...)"). On x86_64 and ARM64 Windows there
 *  is only one calling convention, so __stdcall is meaningless.
 *
 *  Fix: neutralize __stdcall/_stdcall keywords during header inclusion so
 *  the preprocessor expands ORT_API_CALL to nothing.
 */
#pragma once

#if defined(_WIN32) && defined(__MINGW32__)
/* Shadow the __stdcall keyword with empty macros before the ONNX header
   can use them in its own #define ORT_API_CALL __stdcall. The preprocessor
   will then expand ORT_API_CALL -> __stdcall -> (empty). */
#define _stdcall
#define __stdcall
#endif

#include <onnxruntime_c_api.h>

#if defined(_WIN32) && defined(__MINGW32__)
/* Remove our shadow macros so __stdcall works normally elsewhere. */
#undef _stdcall
#undef __stdcall
#endif
