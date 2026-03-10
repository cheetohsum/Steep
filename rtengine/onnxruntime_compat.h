/*
 *  ONNX Runtime compatibility wrapper for MinGW/GCC on Windows.
 *
 *  The ONNX Runtime C API header defines ORT_API_CALL as _stdcall (single
 *  underscore, MSVC keyword). MinGW/GCC only recognizes __stdcall (double
 *  underscore). Since _stdcall is not a keyword in GCC, it causes parse
 *  errors in function pointer declarations.
 *
 *  Fix: define _stdcall as empty before including the header. This does NOT
 *  affect __stdcall (used by WINAPI/CALLBACK in system headers).
 *  On x86_64 and ARM64 the calling convention is irrelevant anyway.
 */
#pragma once

#if defined(_WIN32) && defined(__MINGW32__)
/* ONNX Runtime does: #define ORT_API_CALL _stdcall
   _stdcall is MSVC-only; GCC doesn't recognize it. Define it as empty. */
#ifndef _stdcall
#define _stdcall
#endif
#endif

#include <onnxruntime_c_api.h>
