// SPDX-FileCopyrightText: 2022 Aleix Pol i Gonzalez <aleixpol@kde.org>
// SPDX-License-Identifier: Apache-2.0

#include "dmabufhandler.h"
#include "dmabufeglsetup_p.h"
#include "glhelpers.h"
#include <libdrm/drm_fourcc.h>
#include <logging_dmabuf.h>

struct DmaBufHandlerPrivate {
    ~DmaBufHandlerPrivate()
    {
        DmaBufEglSetup::cleanup(egl);
    }

    DmaBufEglSetup::State egl;
};

DmaBufHandler::DmaBufHandler()
    : d(std::make_unique<DmaBufHandlerPrivate>())
{
}

DmaBufHandler::~DmaBufHandler() = default;

void DmaBufHandler::setupEgl()
{
    DmaBufEglSetup::setup(d->egl);
}

GLenum closestGLType(const QImage &image)
{
    switch (image.format()) {
    case QImage::Format_RGB888:
        return GL_RGB;
    case QImage::Format_BGR888:
        return GL_BGR;
    case QImage::Format_RGB32:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return GL_RGBA;
    default:
        qDebug() << "cannot convert QImage format to GLType" << image.format();
        return GL_RGBA;
    }
}

bool DmaBufHandler::downloadFrame(QImage &qimage, const PipeWireFrame &frame)
{
    Q_ASSERT(frame.dmabuf);
    const QSize streamSize = {frame.dmabuf->width, frame.dmabuf->height};
    Q_ASSERT(qimage.size() == streamSize);
    setupEgl();
    if (!d->egl.initialized) {
        return false;
    }
    if (frame.dmabuf->planes.isEmpty()) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Cannot import a DMA-BUF without planes";
        return false;
    }
    if (frame.dmabuf->modifier != DRM_FORMAT_MOD_INVALID && !d->egl.supportsDmaBufImportModifiers) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Cannot import a modified DMA-BUF without EGL_EXT_image_dma_buf_import_modifiers";
        return false;
    }

    if (!eglMakeCurrent(d->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, d->egl.context)) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Failed to make context current" << GLHelpers::formatEGLError(eglGetError());
        return false;
    }
    EGLImageKHR image = GLHelpers::createImage(d->egl.display, *frame.dmabuf, PipeWireSourceStream::spaVideoFormatToDrmFormat(frame.format), qimage.size(), d->egl.gbmDevice);

    if (image == EGL_NO_IMAGE_KHR) {
        qCWarning(PIPEWIREDMABUF_LOGGING) << "Failed to record frame: Error creating EGLImageKHR - " << GLHelpers::formatEGLError(eglGetError());
        return false;
    }

    GLHelpers::initDebugOutput();
    // create GL 2D texture for framebuffer
    GLuint texture = 0;
    GLuint fbo = 0;
    glGenTextures(1, &texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, texture);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    auto releaseResources = qScopeGuard([&]() {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
        eglDestroyImageKHR(d->egl.display, image);
    });

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }

    glReadPixels(0, 0, frame.dmabuf->width, frame.dmabuf->height, closestGLType(qimage), GL_UNSIGNED_BYTE, qimage.bits());
    return true;
}
