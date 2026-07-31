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

#include <memory>
#include <string>

#include <glibmm/ustring.h>
#include <glibmm/datetime.h>
#include <gdkmm/pixbuf.h>

#include "cacheimagedata.h"
#include "threadutils.h"
#include "thumbnaillistener.h"

namespace rtengine
{
class Thumbnail;

namespace procparams
{

class ProcParams;

}

}
class CacheManager;

struct ParamsEdited;

class Thumbnail
{

    MyMutex         mutex;

    Glib::ustring   fname;              // file name corresponding to the thumbnail
    CacheImageData  cfs;                // cache entry corresponding to the thumbnail
    Glib::ustring   cacheBaseName_;     // basename.md5 used for cache files
    CacheManager*   cachemgr;           // parent
    int             ref;                // variable for reference counting
    int             enqueueNumber;      // the number of instances in the batch queue corresponding to this thumbnail

    // if the thumbnail is in processed mode, this class holds its data:
    rtengine::Thumbnail* tpp;
    int             tw, th;             // dimensions of timgdata (it stores tpp->width and tpp->height in processed mode for simplicity)
    float           imgRatio;           // hack to avoid rounding error
//  double          scale;              // portion of the sizes of the processed thumbnail image and the full scale image

    const std::unique_ptr<rtengine::procparams::ProcParams>      pparams;
    bool            pparamsValid;
    bool            imageLoading;

    // these are the data of the result image of the last getthumbnailimage  call (for caching purposes)
    unsigned char*  lastImg;            // pointer to the processed and scaled base ImageIO image
    int             lastW;              // non rotated width of the cached ImageIO image
    int             lastH;              // non rotated height of the cached ImageIO image
    double          lastScale;          // scale of the cached ImageIO image

    // exif & date/time strings
    mutable Glib::ustring   exifString;
    mutable Glib::ustring   dateTimeString;
    mutable Glib::DateTime  dateTime;
    mutable bool            exifDateTimeStringsValid_ = false;

    // Cached low-res pixbuf for instant editor preview on image switch
    Glib::RefPtr<Gdk::Pixbuf> cachedPixbuf_;
    double cachedPixbufScale_ = 1.0;
    bool            initial_;

    // Properties holds values and edited states for rank, color and trashed
    struct Properties {
        template <class T> struct Property {
            T value;
            bool edited;
            Property(T v): value(v), edited(false) {}
            Property& operator=(T v)
            {
                value = v;
                edited = true;
                return *this;
            }
            operator T() const { return value; }
        };
        Property<int> rank;
        Property<int> color;
        Property<bool> trashed;
        Property<int> pick;

        explicit Properties(int r=0, int c=0, bool t=false, int p=0):
            rank(r), color(c), trashed(t), pick(p) {}
        bool edited() const { return rank.edited || color.edited || trashed.edited || pick.edited; }
    };
    Properties properties;

    // vector of listeners
    std::vector<ThumbnailListener*> listeners;

    void            _loadThumbnail (bool firstTrial = true);
    void            _saveThumbnail (bool saveLiveThumbData = true);
    void            _generateThumbnailImage ();
    void            initCachedThumbnailSize ();
    rtengine::IImage8* processThumbImageLocked (const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool cachePixbuf);
    rtengine::IImage8* upgradeThumbImageLocked (const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool forceUpgrade, bool cachePixbuf);
    int             infoFromImage (const Glib::ustring& fname);
    void            generateExifDateTimeStrings () const;
    void            invalidateExifDateTimeStrings () const;

    Glib::ustring    getCacheFileName (const Glib::ustring& subdir, const Glib::ustring& fext) const;

    void saveMetadata();
    void loadProperties();
    void updateProcParamsProperties(bool forceUpdate = false);
    void saveXMPSidecarProperties();

public:
    Thumbnail (CacheManager* cm, const Glib::ustring& fname, CacheImageData* cf, const Glib::ustring& cacheBaseName);
    Thumbnail (CacheManager* cm, const Glib::ustring& fname, const std::string& md5, const std::string &xmpSidecarMd5, const Glib::ustring& cacheBaseName);
    ~Thumbnail ();

    static int infoFromImage(const Glib::ustring &fname, CacheImageData &cfs);
    static Glib::ustring xmpSidecarPath(const Glib::ustring &imagePath);

    bool              hasProcParams () const;
    const rtengine::procparams::ProcParams& getProcParams ();
    const rtengine::procparams::ProcParams& getProcParamsU ();  // Unprotected version

    // Copy taken while the params mutex is held. Worker threads MUST use this
    // instead of copying getProcParams(): that lock releases before the copy
    // constructor runs, racing GUI-thread setProcParams (heap corruption).
    rtengine::procparams::ProcParams getProcParamsCopy ();

    // Use this to create params on demand for update ; if flaggingMode=true, the procparams is created for a file being flagged (inTrash, rank, colorLabel)
    rtengine::procparams::ProcParams* createProcParamsForUpdate (bool returnParams, bool force, bool flaggingMode = false);

    void              setProcParams (const rtengine::procparams::ProcParams& pp, ParamsEdited* pe = nullptr, int whoChangedIt = -1, bool updateCacheNow = true, bool resetToDefault = false);
    void              clearProcParams (int whoClearedIt = -1);
    void              loadProcParams (bool resetToDefaults = true);

    void              notifylisterners_procParamsChanged(int whoChangedIt);

    bool              isQuick() const;
    bool              isPParamsValid() const;
    bool              isRecentlySaved () const;
    void              imageDeveloped ();
    void              imageEnqueued ();
    void              imageRemovedFromQueue ();
    bool              isEnqueued () const;
    bool              isPixelShift () const;
    bool              isHDR () const;

//        unsigned char*  getThumbnailImage (int &w, int &h, int fixwh=1); // fixwh = 0: fix w and calculate h, =1: fix h and calculate w
    rtengine::IImage8* processThumbImage    (const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool cachePixbuf = false);
    rtengine::IImage8* processThumbImage    (int h, double& scale, rtengine::procparams::CropParams* crop = nullptr, bool cachePixbuf = false);
    rtengine::IImage8* processFullThumbImage(const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool cachePixbuf = false);
    Glib::RefPtr<Gdk::Pixbuf> getCachedPixbuf(double& scale) {
        MyMutex::MyLock lock(mutex);
        scale = cachedPixbufScale_;
        return cachedPixbuf_;
    }
    Glib::RefPtr<Gdk::Pixbuf> tryLoadCachedPreviewPixbuf(int h, double& scale);
    void setCachedPixbuf(Glib::RefPtr<Gdk::Pixbuf> pixbuf, double scale, bool copyPixbuf = true) {
        MyMutex::MyLock lock(mutex);
        cachedPixbuf_ = pixbuf
            ? (copyPixbuf ? pixbuf->copy() : pixbuf)
            : Glib::RefPtr<Gdk::Pixbuf>();
        cachedPixbufScale_ = scale > 0.0 ? scale : 1.0;
    }
    bool trySetCachedPixbuf(Glib::RefPtr<Gdk::Pixbuf> pixbuf, double scale, bool copyPixbuf = true) {
        if (!mutex.trylock()) {
            return false;
        }

        struct UnlockGuard {
            MyMutex& mutex;
            ~UnlockGuard() { mutex.unlock(); }
        } guard{mutex};

        cachedPixbuf_ = pixbuf
            ? (copyPixbuf ? pixbuf->copy() : pixbuf)
            : Glib::RefPtr<Gdk::Pixbuf>();
        cachedPixbufScale_ = scale > 0.0 ? scale : 1.0;
        return true;
    }
    Glib::RefPtr<Gdk::Pixbuf> tryGetCachedPixbuf(double& scale, bool* lockBusy = nullptr) {
        if (lockBusy) {
            *lockBusy = false;
        }

        if (!mutex.trylock()) {
            if (lockBusy) {
                *lockBusy = true;
            }
            scale = 1.0;
            return {};
        }

        struct UnlockGuard {
            MyMutex& mutex;
            ~UnlockGuard() { mutex.unlock(); }
        } guard{mutex};

        scale = cachedPixbufScale_;
        return cachedPixbuf_;
    }
    rtengine::IImage8* upgradeThumbImage    (const rtengine::procparams::ProcParams& pparams, int h, double& scale, bool forceUpgrade, bool cachePixbuf = false);
    rtengine::IImage8* upgradeThumbImage    (int h, double& scale, bool forceUpgrade, rtengine::procparams::CropParams* crop = nullptr, bool cachePixbuf = false);
    void            getThumbnailSize        (int &w, int &h, const rtengine::procparams::ProcParams *pparams = nullptr);
    void            getFinalSize            (const rtengine::procparams::ProcParams& pparams, int& w, int& h);
    void            getOriginalSize         (int& w, int& h) const;

    const Glib::ustring&  getExifString () const;
    const Glib::ustring&  getDateTimeString () const;
    const Glib::DateTime& getDateTime () const;
    void                  getCamWB  (double& temp, double& green, rtengine::StandardObserver observer) const;
    void                  getAutoWB (double& temp, double& green, double equal, rtengine::StandardObserver observer, double tempBias);
    void                  getSpotWB (int x, int y, int rect, double& temp, double& green);
    void                  applyAutoExp (rtengine::procparams::ProcParams& pparams);

    ThFileType      getType () const;
    const Glib::ustring& getFileName () const
    {
        return fname;
    }
    void            setFileName (const Glib::ustring &fn);

    bool            isSupported () const;

    const CacheImageData* getCacheImageData() const;
    std::string     getMD5   () const;

    int             getRank  () const;
    void            setRank  (int rank);

    int             getColorLabel  () const;
    void            setColorLabel  (int colorlabel);

    bool            getTrashed () const;
    void            setTrashed (bool trashed);

    int             getPick () const;
    void            setPick (int pick);

    void            addThumbnailListener (ThumbnailListener* tnl);
    void            removeThumbnailListener (ThumbnailListener* tnl);
    bool            removeThumbnailListenerNoRelease (ThumbnailListener* tnl);

    void            increaseRef ();
    void            decreaseRef ();
    int             decreaseRefCacheMgr (); ///< Special version used by the CacheManager

    void            updateCache (bool updatePParams = true, bool updateCacheImageData = true);
    void            saveThumbnail ();

    bool            openDefaultViewer(int destination);
    bool            imageLoad(bool loading);
};
