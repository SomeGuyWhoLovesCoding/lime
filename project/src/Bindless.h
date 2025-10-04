#pragma once

#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#elif defined(__linux__)
#include <GL/glx.h>
#elif defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#endif

// ------------------------
// Function pointers
// ------------------------
static uint64_t (*glGetTextureHandleARB)(GLuint) = nullptr;
static void (*glMakeTextureHandleResidentARB)(uint64_t) = nullptr;
static void (*glMakeTextureHandleNonResidentARB)(uint64_t) = nullptr;

// ------------------------
// Cross-platform glGetProcAddress
// ------------------------
inline void* glGetProcAddressCross(const char* name) {
#if defined(_WIN32)
    void* p = (void*)wglGetProcAddress(name);
    if (!p) {
        static HMODULE lib = LoadLibraryA("opengl32.dll");
        if (lib) p = (void*)GetProcAddress(lib, name);
    }
    return p;
#elif defined(__linux__)
    return (void*)glXGetProcAddressARB((const GLubyte*)name);
#elif defined(__APPLE__)
    static void* lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    if (!lib) return nullptr;
    return dlsym(lib, name);
#elif defined(__ANDROID__)
    return (void*)eglGetProcAddress(name);
#else
    return nullptr;
#endif
}

// ------------------------
// Bindless texture initialization
// ------------------------
inline bool BindlessTextureSupported() {
    static bool initialized = false;
    static bool supported = false;

    if (!initialized) {
        initialized = true;

        glGetTextureHandleARB = (decltype(glGetTextureHandleARB))glGetProcAddressCross("glGetTextureHandleARB");
        glMakeTextureHandleResidentARB = (decltype(glMakeTextureHandleResidentARB))glGetProcAddressCross("glMakeTextureHandleResidentARB");
        glMakeTextureHandleNonResidentARB = (decltype(glMakeTextureHandleNonResidentARB))glGetProcAddressCross("glMakeTextureHandleNonResidentARB");

        supported = glGetTextureHandleARB && glMakeTextureHandleResidentARB && glMakeTextureHandleNonResidentARB;
    }

    return supported;
}

// ------------------------
// Bindless texture API
// ------------------------
inline uint64_t GetTextureHandle(GLuint texture) {
    if (!BindlessTextureSupported()) return 0;
    return glGetTextureHandleARB(texture);
}

inline void MakeTextureResident(uint64_t handle) {
    if (!BindlessTextureSupported()) return;
    glMakeTextureHandleResidentARB(handle);
}

inline void MakeTextureNonResident(uint64_t handle) {
    if (!BindlessTextureSupported()) return;
    glMakeTextureHandleNonResidentARB(handle);
}