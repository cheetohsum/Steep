/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <functional>
#include <list>
#include <memory>

#include <gtkmm.h>

#include "guiutils.h"
#include "hidpi.h"
#include "threadutils.h"

#include "rtengine/noncopyable.h"
#include "rtengine/rtengine.h"

class PreviewListener
{
public:
    virtual ~PreviewListener() = default;
    virtual void previewImageChanged() = 0;
};

class PreviewHandler;

struct PreviewHandlerIdleHelper {
    PreviewHandler* phandler;
    bool destroyed;
    int pending;
};

class PreviewHandler final : public rtengine::PreviewImageListener, public rtengine::NonCopyable
{
private:
    friend int setImageUI   (void* data);
    friend int delImageUI   (void* data);
    friend int imageReadyUI (void* data);

    IdleRegister idle_register;

protected:
    rtengine::IImage8* image;
    const std::unique_ptr<rtengine::procparams::CropParams> cropParams;
    double previewScale;
    PreviewHandlerIdleHelper* pih;
    std::list<PreviewListener*> listeners;
    MyMutex previewImgMutex;
    Glib::RefPtr<Gdk::Pixbuf> previewImg;
    bool previewImgReferencesEngineData;
    std::function<void()> firstEngineImageReadyCallback;
    std::function<void()> engineImageReadyCallback;

public:

    explicit PreviewHandler (
        std::function<void()> firstEngineImageReadyCallback = {},
        std::function<void()> engineImageReadyCallback = {});
    ~PreviewHandler () override;

    void addPreviewImageListener (PreviewListener* l)
    {
        listeners.push_back (l);
    }

    // previewimagelistener
    void setImage(rtengine::IImage8* img, double scale, const rtengine::procparams::CropParams& cp) override;
    void delImage(rtengine::IImage8* img) override;
    void imageReady(const rtengine::procparams::CropParams& cp) override;

    // this function is called when a new preview image arrives from rtengine
    void previewImageChanged ();

    // Set a placeholder preview (e.g. cached thumbnail) before the engine delivers the real image
    void setPlaceholder(Glib::RefPtr<Gdk::Pixbuf> pixbuf, double scale);
    bool trySetPlaceholder(Glib::RefPtr<Gdk::Pixbuf> pixbuf, double scale);
    bool hasPlaceholder() const;

    // with this function it is possible to ask for a rough approximation of a (possibly zoomed) crop of the image
    Glib::RefPtr<Gdk::Pixbuf> getRoughImage(ImageCoord pos, hidpi::ScaledDeviceSize desiredSize, double zoom);
    hidpi::DevicePixbuf getRoughImage(hidpi::LogicalSize desiredSize, int deviceScale, double& outLogicalZoom);

    // Downsample the latest monitor-space engine result without first copying
    // the full editor preview. imageScale is output pixels per full-size pixel.
    Glib::RefPtr<Gdk::Pixbuf> getScaledEnginePreview(
        int width,
        int height,
        double& imageScale);

    rtengine::procparams::CropParams    getCropParams ();

    // Snapshot the current preview pixbuf and scale for reuse as a placeholder.
    // Returns a deep copy because previewImg may wrap engine memory that gets freed.
    Glib::RefPtr<Gdk::Pixbuf> getPreviewPixbuf(double& scale) {
        MyMutex::MyLock lock(previewImgMutex);
        scale = previewScale;
        if (!previewImg) {
            return Glib::RefPtr<Gdk::Pixbuf>();
        }

        return previewImgReferencesEngineData ? previewImg->copy() : previewImg;
    }
    Glib::RefPtr<Gdk::Pixbuf> tryGetPreviewPixbuf(double& scale, bool* lockBusy = nullptr) {
        if (lockBusy) {
            *lockBusy = false;
        }

        if (!previewImgMutex.trylock()) {
            if (lockBusy) {
                *lockBusy = true;
            }
            scale = 1.0;
            return {};
        }

        struct UnlockGuard {
            MyMutex& mutex;
            ~UnlockGuard() { mutex.unlock(); }
        } guard{previewImgMutex};

        scale = previewScale;
        if (!previewImg) {
            return {};
        }

        return previewImgReferencesEngineData ? previewImg->copy() : previewImg;
    }
};
