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
#include <fftw3.h>
#include <glibmm/miscutils.h>
#include <glibmm/ustring.h>
#include "color.h"
#include "rtengine.h"
#include "iccstore.h"
#include "dcp.h"
#include "camconst.h"
#include "curves.h"
#include "rawimagesource.h"
#include "improcfun.h"
#include "improccoordinator.h"
#include "dfmanager.h"
#include "ffmanager.h"
#include "rtthumbnail.h"
#include "profilestore.h"
#include "rtgui/threadutils.h"
#include "rtlensfun.h"
#include "metadata.h"
#include "procparams.h"
#include "aidenoise.h"
#ifdef RT_AI_MASKING
#include "aisegmentation.h"
#include "aiinpainting.h"
#endif

namespace rtengine
{

const Settings* settings;

MyMutex* lcmsMutex = nullptr;
MyMutex *fftwMutex = nullptr;
MyMutex *librawMutex = nullptr;

int init (const Settings* s, const Glib::ustring& baseDir, const Glib::ustring& userSettingsDir, bool loadAll)
{
    settings = s;
    ProcParams::init();
    PerceptualToneCurve::init();
    RawImageSource::init();

#ifdef _OPENMP
#pragma omp parallel sections if (!settings->verbose)
#endif
{
#ifdef _OPENMP
#pragma omp section
#endif
{
    bool ok;

    if (s->lensfunDbDirectory.empty() || Glib::path_is_absolute(s->lensfunDbDirectory)) {
        ok = LFDatabase::init(s->lensfunDbDirectory);
    } else {
        ok = LFDatabase::init(Glib::build_filename(baseDir, s->lensfunDbDirectory));
    }

    if (!ok && !s->lensfunDbBundleDirectory.empty() && s->lensfunDbBundleDirectory != s->lensfunDbDirectory) {
        if (Glib::path_is_absolute(s->lensfunDbBundleDirectory)) {
            LFDatabase::init(s->lensfunDbBundleDirectory);
        } else {
            LFDatabase::init(Glib::build_filename(baseDir, s->lensfunDbBundleDirectory));
        }
    }
}
#ifdef _OPENMP
#pragma omp section
#endif
{
    ProfileStore::getInstance()->init(loadAll);
}
#ifdef _OPENMP
#pragma omp section
#endif
{
    ICCStore::getInstance()->init(s->iccDirectory, Glib::build_filename (baseDir, "iccprofiles"), loadAll);
}
#ifdef _OPENMP
#pragma omp section
#endif
{
    DCPStore::getInstance()->init(Glib::build_filename (baseDir, "dcpprofiles"), loadAll);
}
#ifdef _OPENMP
#pragma omp section
#endif
{
    CameraConstantsStore::getInstance()->init(baseDir, userSettingsDir);
}
#ifdef _OPENMP
#pragma omp section
#endif
{
    DFManager::getInstance().init(s->darkFramesPath);
}
#ifdef _OPENMP
#pragma omp section
#endif
{
    ffm.init(s->flatFieldsPath);
}
}

    Color::init ();
    Exiv2Metadata::init();

    // AI Denoise dependency probing is started lazily from the UI. Running the
    // Python probe during engine init races the Windows GTK startup path.

#ifdef RT_AI_MASKING
    {
        const Glib::ustring modelPath = Glib::build_filename(baseDir, "models", "segformer_b0_ade20k.onnx");
        fprintf(stderr, "AI Masking: Looking for model at: %s\n", modelPath.c_str());
        if (Glib::file_test(modelPath, Glib::FILE_TEST_EXISTS)) {
            fprintf(stderr, "AI Masking: Model file found, initializing...\n");
            if (!getAISegmentationEngine().init(modelPath)) {
                fprintf(stderr, "AI Masking: Failed to initialize segmentation engine\n");
            } else {
                fprintf(stderr, "AI Masking: Engine initialized successfully\n");
            }
        } else {
            fprintf(stderr, "AI Masking: Model file NOT found at %s\n", modelPath.c_str());
        }
    }

    // LaMa inpainting model
    {
        const Glib::ustring lamaPath = Glib::build_filename(baseDir, "models", "lama_inpainting.onnx");
        fprintf(stderr, "AI Inpainting: Looking for model at: %s\n", lamaPath.c_str());
        if (Glib::file_test(lamaPath, Glib::FILE_TEST_EXISTS)) {
            fprintf(stderr, "AI Inpainting: Model file found, initializing...\n");
            if (!getAIInpaintingEngine().init(lamaPath)) {
                fprintf(stderr, "AI Inpainting: Failed to initialize inpainting engine\n");
            } else {
                fprintf(stderr, "AI Inpainting: Engine initialized successfully\n");
            }
        } else {
            fprintf(stderr, "AI Inpainting: Model file NOT found at %s\n", lamaPath.c_str());
        }
    }
#endif

    delete lcmsMutex;
    lcmsMutex = new MyMutex;
    fftwMutex = new MyMutex;
    delete librawMutex;
    librawMutex = new MyMutex;
    return 0;
}

void cleanup ()
{
    Exiv2Metadata::cleanup();
    ProcParams::cleanup ();
    Color::cleanup ();
    RawImageSource::cleanup ();

#ifdef RT_FFTW3F_OMP
    fftwf_cleanup_threads();
#else
    fftwf_cleanup();
#endif

}

StagedImageProcessor* StagedImageProcessor::create (InitialImage* initialImage)
{

    ImProcCoordinator* ipc = new ImProcCoordinator ();
    ipc->assign (initialImage->getImageSource ());
    return ipc;
}

void StagedImageProcessor::destroy (StagedImageProcessor* sip)
{

    delete sip;
}

Settings* Settings::create  ()
{

    return new Settings;
}

void Settings::destroy (Settings* s)
{

    delete s;
}


}

