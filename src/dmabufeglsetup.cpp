/*
    SPDX-FileCopyrightText: 2026 SonicDE Project

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "dmabufeglsetup_p.h"
#include "glhelpers.h"
#include <logging_dmabuf.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {
bool hasClientPlatformGbm(const DmaBufEglSetup::Ops& ops)
{
    if (!ops.hasExtension(EGL_NO_DISPLAY, "EGL_EXT_platform_base")) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Required EGL client extension EGL_EXT_platform_base is missing";
        return false;
    }
    if (!ops.hasExtension(EGL_NO_DISPLAY, "EGL_MESA_platform_gbm")) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Required EGL client extension EGL_MESA_platform_gbm is missing";
        return false;
    }
    return true;
}
}

DmaBufEglSetup::Ops DmaBufEglSetup::defaultOps()
{
    return {
        .resolveRenderNode = [] { return RenderNodeResolver::resolveForCurrentSession(); },
        .hasExtension = [](EGLDisplay display, const char* extension) { return epoxy_has_egl_extension(display, extension); },
        .openRenderNode = [](const char* path, int flags) { return open(path, flags); },
        .closeFd = [](int fd) { close(fd); },
        .createGbmDevice = [](int fd) { return gbm_create_device(fd); },
        .destroyGbmDevice = [](gbm_device* device) { gbm_device_destroy(device); },
        .getPlatformDisplay = [](EGLenum platform, void* nativeDisplay, const EGLint* attribs) { return eglGetPlatformDisplayEXT(platform, nativeDisplay, attribs); },
        .initialize = [](EGLDisplay display, EGLint* major, EGLint* minor) { return eglInitialize(display, major, minor); },
        .terminate = [](EGLDisplay display) { return eglTerminate(display); },
        .bindApi = [](EGLenum api) { return eglBindAPI(api); },
        .chooseConfig = [](EGLDisplay display, const EGLint* attribs, EGLConfig* configs, EGLint configSize, EGLint* count) { return eglChooseConfig(display, attribs, configs, configSize, count); },
        .createContext = [](EGLDisplay display, EGLConfig config, EGLContext shareContext, const EGLint* attribs) { return eglCreateContext(display, config, shareContext, attribs); },
        .destroyContext = [](EGLDisplay display, EGLContext context) { return eglDestroyContext(display, context); },
        .makeCurrent = [](EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) { return eglMakeCurrent(display, draw, read, context); },
    };
}

void DmaBufEglSetup::cleanup(State& state, const Ops& ops)
{
    if (state.context != EGL_NO_CONTEXT) {
        ops.destroyContext(state.display, state.context);
        state.context = EGL_NO_CONTEXT;
    }
    if (state.display != EGL_NO_DISPLAY && state.ownsDisplay) {
        ops.terminate(state.display);
    }
    state.display = EGL_NO_DISPLAY;
    state.ownsDisplay = false;
    state.supportsDmaBufImportModifiers = false;
    state.initialized = false;

    if (state.gbmDevice) {
        ops.destroyGbmDevice(state.gbmDevice);
        state.gbmDevice = nullptr;
    }
    if (state.drmFd >= 0) {
        ops.closeFd(state.drmFd);
        state.drmFd = -1;
    }
}

bool DmaBufEglSetup::setup(State& state, const Ops& ops)
{
    if (state.initialized) {
        return true;
    }

    const auto cleanupOnFailure = [&state, &ops] {
        cleanup(state, ops);
        return false;
    };

    const auto renderContext = ops.resolveRenderNode();
    if (renderContext.eglDisplay != EGL_NO_DISPLAY) {
        state.display = renderContext.eglDisplay;
        state.ownsDisplay = false;
    } else {
        if (!hasClientPlatformGbm(ops)) {
            return false;
        }

        const QByteArray renderNode = renderContext.renderNode;
        if (renderNode.isEmpty()) {
            qCWarning(PIPEWIREDMABUF_LOGGING) << "Failed to resolve a render node for the current session";
            return false;
        }

        state.drmFd = ops.openRenderNode(renderNode.constData(), O_RDWR);
        if (state.drmFd < 0) {
            qCWarning(PIPEWIREDMABUF_LOGGING) << "Failed to open drm render node" << renderNode << "with error:" << strerror(errno);
            return cleanupOnFailure();
        }

        state.gbmDevice = ops.createGbmDevice(state.drmFd);
        if (!state.gbmDevice) {
            qCWarning(PIPEWIREDMABUF_LOGGING) << "Cannot create GBM device:" << strerror(errno);
            return cleanupOnFailure();
        }

        state.display = ops.getPlatformDisplay(EGL_PLATFORM_GBM_MESA, state.gbmDevice, nullptr);
        state.ownsDisplay = state.display != EGL_NO_DISPLAY;
    }

    if (state.display == EGL_NO_DISPLAY) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Error during obtaining EGL display:" << GLHelpers::formatEGLError(eglGetError());
        return cleanupOnFailure();
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (ops.initialize(state.display, &major, &minor) == EGL_FALSE) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Error during eglInitialize:" << GLHelpers::formatEGLError(eglGetError());
        return cleanupOnFailure();
    }

    if (!ops.hasExtension(state.display, "EGL_EXT_image_dma_buf_import")) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Required EGL display extension EGL_EXT_image_dma_buf_import is missing";
        return cleanupOnFailure();
    }
    state.supportsDmaBufImportModifiers = ops.hasExtension(state.display, "EGL_EXT_image_dma_buf_import_modifiers");

    if (ops.bindApi(EGL_OPENGL_API) == EGL_FALSE) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Binding the OpenGL EGL API failed:" << GLHelpers::formatEGLError(eglGetError());
        return cleanupOnFailure();
    }

    static const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_BIT,
        EGL_CONFIG_CAVEAT,
        EGL_NONE,
        EGL_NONE,
    };

    EGLConfig config = EGL_NO_CONFIG_KHR;
    EGLint configCount = 0;
    if (ops.chooseConfig(state.display, configAttribs, &config, 1, &configCount) == EGL_FALSE || configCount <= 0 || config == EGL_NO_CONFIG_KHR) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Choosing an EGL config failed:" << GLHelpers::formatEGLError(eglGetError());
        return cleanupOnFailure();
    }

    static const EGLint contextAttribs[] = {EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE};
    state.context = ops.createContext(state.display, config, EGL_NO_CONTEXT, contextAttribs);
    if (state.context == EGL_NO_CONTEXT) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Creating an EGL context failed:" << GLHelpers::formatEGLError(eglGetError());
        return cleanupOnFailure();
    }

    if (ops.makeCurrent(state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, state.context) == EGL_FALSE) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Making the EGL context current failed:" << GLHelpers::formatEGLError(eglGetError());
        return cleanupOnFailure();
    }

    qCDebug(PIPEWIREDMABUF_LOGGING) << "EGL initialization succeeded";
    qCDebug(PIPEWIREDMABUF_LOGGING) << QStringLiteral("EGL version: %1.%2").arg(major).arg(minor);

    state.initialized = true;
    return true;
}
