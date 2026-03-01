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
#include <cmath>
#include <iomanip>

#include "aidenoise.h"

#include "guiutils.h"

#include "rtengine/aidenoise.h"
#include "rtengine/procparams.h"
#include "rtengine/rtengine.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring AIDenoise::TOOL_NAME = "aidenoise";

AIDenoise::AIDenoise () : FoldableToolPanel(this, TOOL_NAME, M("TP_AIDENOISE_LABEL"), true, true),
    ipc_(nullptr)
{

    isoConditioning = Gtk::manage (new Adjuster (M("TP_AIDENOISE_ISO_CONDITIONING"), 0, 100, 1, 50));
    blend = Gtk::manage (new Adjuster (M("TP_AIDENOISE_BLEND"), 0, 100, 1, 100));

    useGpu = Gtk::manage (new Gtk::CheckButton (M("TP_AIDENOISE_USE_GPU")));

    denoiseBtn = Gtk::manage (new Gtk::Button (M("TP_AIDENOISE_DENOISE")));
    cancelBtn = Gtk::manage (new Gtk::Button (M("TP_AIDENOISE_CANCEL")));
    cancelBtn->set_sensitive(false);

    statusLabel = Gtk::manage (new Gtk::Label (M("TP_AIDENOISE_STATUS_READY")));
    statusLabel->set_halign(Gtk::ALIGN_START);

    Gtk::Box* btnBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL, 4));
    btnBox->pack_start (*denoiseBtn, Gtk::PACK_EXPAND_WIDGET);
    btnBox->pack_start (*cancelBtn, Gtk::PACK_EXPAND_WIDGET);

    // Advanced section for ISO Conditioning and Blend
    advancedSection = Gtk::manage(new AdvancedSection());
    pack_start(*advancedSection, Gtk::PACK_SHRINK, 0);
    Gtk::Box* const advBox = advancedSection->getContentBox();
    advBox->pack_start(*isoConditioning);
    advBox->pack_start(*blend);

    pack_start (*useGpu);
    pack_start (*btnBox);
    pack_start (*statusLabel);

    isoConditioning->setAdjusterListener (this);
    blend->setAdjusterListener (this);

    denoiseBtn->signal_clicked().connect(sigc::mem_fun(*this, &AIDenoise::onDenoiseClicked));
    cancelBtn->signal_clicked().connect(sigc::mem_fun(*this, &AIDenoise::onCancelClicked));

    show_all_children ();
}

void AIDenoise::read (const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener ();

    if (pedited) {
        isoConditioning->setEditedState (pedited->aiDenoise.isoConditioning ? Edited : UnEdited);
        blend->setEditedState           (pedited->aiDenoise.blend ? Edited : UnEdited);
        useGpu->set_inconsistent        (!pedited->aiDenoise.useGpu);
        set_inconsistent                (multiImage && !pedited->aiDenoise.enabled);
    }

    setEnabled(pp->aiDenoise.enabled);

    isoConditioning->setValue (pp->aiDenoise.isoConditioning);
    blend->setValue (pp->aiDenoise.blend);
    useGpu->set_active (pp->aiDenoise.useGpu);

    enableListener ();
}

void AIDenoise::write (ProcParams* pp, ParamsEdited* pedited)
{

    pp->aiDenoise.enabled          = getEnabled();
    pp->aiDenoise.isoConditioning  = isoConditioning->getValue ();
    pp->aiDenoise.blend            = blend->getValue ();
    pp->aiDenoise.useGpu           = useGpu->get_active ();

    if (pedited) {
        pedited->aiDenoise.enabled          = !get_inconsistent();
        pedited->aiDenoise.isoConditioning  = isoConditioning->getEditedState ();
        pedited->aiDenoise.blend            = blend->getEditedState ();
        pedited->aiDenoise.useGpu           = !useGpu->get_inconsistent();
    }
}

void AIDenoise::setDefaults (const ProcParams* defParams, const ParamsEdited* pedited)
{

    isoConditioning->setDefault (defParams->aiDenoise.isoConditioning);
    blend->setDefault (defParams->aiDenoise.blend);

    if (pedited) {
        isoConditioning->setDefaultEditedState (pedited->aiDenoise.isoConditioning ? Edited : UnEdited);
        blend->setDefaultEditedState (pedited->aiDenoise.blend ? Edited : UnEdited);
    } else {
        isoConditioning->setDefaultEditedState (Irrelevant);
        blend->setDefaultEditedState (Irrelevant);
    }
}

void AIDenoise::adjusterChanged(Adjuster* a, double newval)
{
    autoEnable();
    if (listener && getEnabled()) {
        if (a == blend) {
            listener->panelChanged (EvAIDNBlend, Glib::ustring::format (std::setw(2), std::fixed, std::setprecision(1), a->getValue()));
        } else if (a == isoConditioning) {
            updateStatus(M("TP_AIDENOISE_STATUS_SETTINGS_CHANGED"));
        }
    }
}

void AIDenoise::enabledChanged ()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged (EvAIDNEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged (EvAIDNEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvAIDNEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void AIDenoise::setBatchMode (bool batchMode)
{

    ToolPanel::setBatchMode (batchMode);
    advancedSection->setBatchMode(batchMode);
    isoConditioning->showEditedCB ();
    blend->showEditedCB ();
}

void AIDenoise::trimValues (rtengine::procparams::ProcParams* pp)
{

    isoConditioning->trimValue(pp->aiDenoise.isoConditioning);
    blend->trimValue(pp->aiDenoise.blend);
}

void AIDenoise::setImagePath (const Glib::ustring& path)
{
    imagePath_ = path;
}

void AIDenoise::setImProcCoordinator (rtengine::StagedImageProcessor* ipc)
{
    ipc_ = ipc;
}

void AIDenoise::onDenoiseClicked ()
{
    auto& aidm = rtengine::AIDenoiseManager::getInstance();

    if (!aidm.isAvailable()) {
        updateStatus(M("TP_AIDENOISE_STATUS_NOT_AVAILABLE"));
        return;
    }

    if (imagePath_.empty()) {
        updateStatus("No image loaded");
        return;
    }

    updateStatus(M("TP_AIDENOISE_STATUS_PROCESSING"));
    denoiseBtn->set_sensitive(false);
    cancelBtn->set_sensitive(true);

    // Export RT's own demosaiced image for the denoiser (correct color space)
    Glib::ustring inputTiffPath;
    if (ipc_) {
        inputTiffPath = Glib::build_filename(Glib::get_tmp_dir(), "rt_aidenoise_input.tif");
        if (!ipc_->exportDemosaicedTIFF(inputTiffPath)) {
            updateStatus("Failed to export demosaiced image");
            denoiseBtn->set_sensitive(true);
            cancelBtn->set_sensitive(false);
            return;
        }
    }

    // Build output path in /tmp
    Glib::ustring outputPath = Glib::build_filename(Glib::get_tmp_dir(), "rt_aidenoise_result.tif");

    // Read current params from the UI
    rtengine::procparams::AIDenoiseParams params;
    params.enabled = getEnabled();
    params.isoConditioning = isoConditioning->getValue();
    params.blend = blend->getValue();
    params.useGpu = useGpu->get_active();

    aidm.startDenoising(
        imagePath_,
        params,
        outputPath,
        // Progress callback — dispatch to GTK main thread
        [this](double progress) {
            Glib::signal_idle().connect_once([this, progress]() {
                Glib::ustring msg = Glib::ustring::compose(
                    M("TP_AIDENOISE_STATUS_PROCESSING") + " %1%%",
                    static_cast<int>(progress));
                updateStatus(msg);
            });
        },
        // Done callback — dispatch to GTK main thread
        [this](bool success, const Glib::ustring& message) {
            Glib::signal_idle().connect_once([this, success, message]() {
                denoiseBtn->set_sensitive(true);
                cancelBtn->set_sensitive(false);
                if (success) {
                    updateStatus("Denoised successfully");
                    // Trigger a pipeline re-render by calling adjusterChanged
                    // directly. We can't use blend->setValue() because Adjuster
                    // blocks signals during programmatic changes (no event fires).
                    // Calling adjusterChanged() directly goes through
                    // listener->panelChanged(EvAIDNBlend) → endUpdateParams →
                    // startProcessing → Crop::update where the blend code runs.
                    if (listener && getEnabled()) {
                        listener->panelChanged(EvAIDNBlend,
                            Glib::ustring::format(std::setw(2), std::fixed,
                                std::setprecision(1), blend->getValue()));
                    }
                } else {
                    // Extract a clean error message from stderr output
                    Glib::ustring cleanMsg = message;
                    // Look for known error patterns
                    if (message.find("Unsupported Bayer pattern") != Glib::ustring::npos) {
                        cleanMsg = "Unsupported sensor (X-Trans). Only Bayer sensors are supported.";
                    } else if (message.find("CUDA") != Glib::ustring::npos
                               || message.find("out of memory") != Glib::ustring::npos) {
                        cleanMsg = "GPU out of memory. Try disabling 'Use GPU'.";
                    } else if (message.find("No module named") != Glib::ustring::npos) {
                        cleanMsg = "RawRefinery module not found. Reinstall with: pip install rawrefinery";
                    } else {
                        // Extract last meaningful line from stderr
                        auto pos = message.rfind("Error during processing:");
                        if (pos != Glib::ustring::npos) {
                            cleanMsg = message.substr(pos);
                        } else {
                            // Truncate to something reasonable
                            if (cleanMsg.size() > 120) {
                                cleanMsg = cleanMsg.substr(cleanMsg.size() - 120);
                            }
                        }
                    }
                    updateStatus(cleanMsg);
                }
            });
        },
        inputTiffPath
    );
}

void AIDenoise::onCancelClicked ()
{
    rtengine::AIDenoiseManager::getInstance().cancel();
    updateStatus(M("TP_AIDENOISE_STATUS_CANCELLED"));
    denoiseBtn->set_sensitive(true);
    cancelBtn->set_sensitive(false);
}

void AIDenoise::updateStatus (const Glib::ustring& text)
{
    statusLabel->set_text(text);
}
