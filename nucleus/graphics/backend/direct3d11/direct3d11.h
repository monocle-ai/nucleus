/**
 * (c) 2015 Alexandro Sanchez Bach. All rights reserved.
 * Released under GPL v2 license. Read LICENSE for more details.
 */

#pragma once

#include "nucleus/common.h"

#if defined(NUCLEUS_PLATFORM_WINDOWS)
#include <d3d11.h>
#include <dxgi1_4.h>
#endif

// Function types: dxgi.dll
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);

namespace gfx {
namespace direct3d11 {

// Declare Function
#define DECLARE_FUNCTION(type, module, function) extern type _##function;
#define FUNCTION DECLARE_FUNCTION
#include "direct3d11.inl"
#undef FUNCTION
#undef DECLARE_FUNCTION

// Load extensions
bool initializeDirect3D11();

}  // namespace direct3d11
}  // namespace gfx
