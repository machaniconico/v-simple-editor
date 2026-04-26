// Phase 1e — isolate d3d11.h from the rest of the codebase.
// Including d3d11.h alongside Qt 6 headers (QCborTag etc.) trips a known
// MSVC ambiguity at d3d11.h:1374 in WinSDK 10.0.26100. Keeping the include
// here, in a TU that pulls in zero Qt headers, sidesteps the conflict and
// gives VideoPlayer a clean C linkage entry point for the device pointer.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// Skip d3d11.h's CD3D11_BOX C++ helper (and the operator==/!= pair) — that
// pair fails to compile under MSVC C++17 with the WinSDK 10.0.26100 layout
// when GUID's operator== is also visible. We don't use those helpers.
#define D3D11_NO_HELPERS

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

extern "C" void *veditor_avHwDeviceCtxToD3D11Device(AVBufferRef *ref)
{
    if (!ref) return nullptr;
    AVHWDeviceContext *devCtx = (AVHWDeviceContext*)ref->data;
    if (!devCtx) return nullptr;
    AVD3D11VADeviceContext *d3dCtx = (AVD3D11VADeviceContext*)devCtx->hwctx;
    if (!d3dCtx) return nullptr;
    return (void*)d3dCtx->device;
}

#endif // _WIN32
