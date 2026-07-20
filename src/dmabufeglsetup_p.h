/*
    SPDX-FileCopyrightText: 2026 SonicDE Project

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "rendernodecontext_p.h"

#include <functional>

#include <epoxy/egl.h>
#include <gbm.h>

namespace DmaBufEglSetup {
struct State {
    bool initialized = false;
    bool ownsDisplay = false;
    bool supportsDmaBufImportModifiers = false;
    qint32 drmFd = -1;
    gbm_device* gbmDevice = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
};

struct Ops {
    std::function<RenderNodeContext()> resolveRenderNode;
    std::function<bool(EGLDisplay, const char*)> hasExtension;
    std::function<int(const char*, int)> openRenderNode;
    std::function<void(int)> closeFd;
    std::function<gbm_device*(int)> createGbmDevice;
    std::function<void(gbm_device*)> destroyGbmDevice;
    std::function<EGLDisplay(EGLenum, void*, const EGLint*)> getPlatformDisplay;
    std::function<EGLBoolean(EGLDisplay, EGLint*, EGLint*)> initialize;
    std::function<EGLBoolean(EGLDisplay)> terminate;
    std::function<EGLBoolean(EGLenum)> bindApi;
    std::function<EGLBoolean(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)> chooseConfig;
    std::function<EGLContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*)> createContext;
    std::function<EGLBoolean(EGLDisplay, EGLContext)> destroyContext;
    std::function<EGLBoolean(EGLDisplay, EGLSurface, EGLSurface, EGLContext)> makeCurrent;
};

Ops defaultOps();

bool setup(State& state, const Ops& ops = defaultOps());
void cleanup(State& state, const Ops& ops = defaultOps());
}
