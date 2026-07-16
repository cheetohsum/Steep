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
#include <utility>

#include "aidenoise.h"

#include "guiutils.h"

#include "rtengine/aidenoise.h"
#include "rtengine/procparams.h"
#include "rtengine/rtengine.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring AIDenoise::TOOL_NAME = "aidenoise";

AIDenoise::AIDenoise () : FoldableToolPanel(this, TOOL_NAME, M("TP_AIDENOISE_LABEL"), true, true),
    contentExpanded_(false),
    ipc_(nullptr)
{

    isoConditioning = Gtk::manage (new Adjuster (M("TP_AIDENOISE_ISO_CONDITIONING"), 0, 100, 1, 50));
    blend = Gtk::manage (new Adjuster (M("TP_AIDENOISE_BLEND"), 0, 100, 1, 100));

    useGpu = Gtk::manage (new Gtk::CheckButton (M("TP_AIDENOISE_USE_GPU")));

    cancelBtn = Gtk::manage (new Gtk::Button (M("TP_AIDENOISE_CANCEL")));
    cancelBtn->set_sensitive(false);

    statusLabel = Gtk::manage (new Gtk::Label (M("TP_AIDENOISE_STATUS_READY")));
    statusLabel->set_halign(Gtk::ALIGN_END);

    // Checkbox (no label) triggers AI Denoise when ticked, sets blend=0 when unticked
    activateCheck_ = Gtk::manage(new Gtk::CheckButton());
    activateCheck_->signal_toggled().connect([this]() {
        if (blockActivate_) return;
        if (activateCheck_->get_active()) {
            // Restore blend and run denoise
            blend->setValue(savedBlend_);
            onDenoiseClicked();
        } else {
            // Save current blend and zero it out
            savedBlend_ = blend->getValue();
            blend->setValue(0);
            if (listener && getEnabled()) {
                listener->panelChanged(EvAIDNBlend,
                    Glib::ustring::format(std::setw(2), std::fixed,
                        std::setprecision(1), 0.0));
            }
        }
    });
    activateCheck_->set_tooltip_text("Check to run AI Denoise, uncheck to disable effect");

    // Clickable label that toggles the settings panel
    sectionLabel_ = Gtk::manage(new Gtk::Label("AI Denoise"));
    sectionLabel_->get_style_context()->add_class("tool-section-label");
    sectionLabel_->set_halign(Gtk::ALIGN_START);
    auto* labelEvent = Gtk::manage(new Gtk::EventBox());
    labelEvent->add(*sectionLabel_);
    labelEvent->set_events(Gdk::BUTTON_PRESS_MASK);
    labelEvent->signal_button_press_event().connect([this](GdkEventButton*) -> bool {
        toggleContent();
        return true;
    });
    labelEvent->set_tooltip_text("Click to show/hide settings");

    // Header row: checkbox + label + status
    auto* headerRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    headerRow->pack_start(*activateCheck_, Gtk::PACK_SHRINK);
    headerRow->pack_start(*labelEvent, Gtk::PACK_SHRINK);
    headerRow->pack_end(*statusLabel, Gtk::PACK_SHRINK);
    getSummaryBox()->pack_start(*headerRow, Gtk::PACK_SHRINK);

    // Collapsible content box
    toolContent_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));

    toolContent_->pack_start(*isoConditioning, Gtk::PACK_SHRINK);
    toolContent_->pack_start(*blend, Gtk::PACK_SHRINK);
    toolContent_->pack_start(*useGpu, Gtk::PACK_SHRINK);
    toolContent_->pack_start(*cancelBtn, Gtk::PACK_SHRINK);
    toolContent_->show_all();

    // Animated revealer for smooth expand/collapse
    revealer_ = Gtk::manage(new Gtk::Revealer());
    revealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    revealer_->set_transition_duration(200);
    revealer_->set_reveal_child(false);
    revealer_->add(*toolContent_);
    revealer_->show();
    getSummaryBox()->pack_start(*revealer_, Gtk::PACK_SHRINK);
    getSummaryBox()->show_all();

    isoConditioning->setAdjusterListener (this);
    blend->setAdjusterListener (this);

    cancelBtn->signal_clicked().connect(sigc::mem_fun(*this, &AIDenoise::onCancelClicked));

    // Register callback so the status label updates when detection finishes
    auto& aidm = rtengine::AIDenoiseManager::getInstance();
    aidm.setDetectDoneCallback([this](bool success) {
        Glib::signal_idle().connect_once([this, success]() {
            if (success) {
                updateStatus(M("TP_AIDENOISE_STATUS_READY"));
                if (activateCheck_->get_active() && !imagePath_.empty()) {
                    onDenoiseClicked();
                }
            } else {
                updateStatus(M("TP_AIDENOISE_STATUS_NOT_AVAILABLE"));
            }
        });
    });

    // Show initial status based on current detection state
    if (aidm.isAvailable()) {
        statusLabel->set_text(M("TP_AIDENOISE_STATUS_READY"));
    } else if (aidm.isDetecting()) {
        statusLabel->set_text(M("TP_AIDENOISE_STATUS_LOADING"));
    } else {
        statusLabel->set_text(M("TP_AIDENOISE_STATUS_READY"));
    }

    show_all();
}

void AIDenoise::toggleContent()
{
    contentExpanded_ = !contentExpanded_;
    revealer_->set_reveal_child(contentExpanded_);
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

    // Sync checkbox: active only when enabled AND blend > 0
    blockActivate_ = true;
    activateCheck_->set_active(pp->aiDenoise.enabled && pp->aiDenoise.blend > 0);
    if (pp->aiDenoise.blend > 0) {
        savedBlend_ = pp->aiDenoise.blend;
    }
    blockActivate_ = false;

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
    // Auto-enable the tool when user clicks Denoise
    if (!getEnabled()) {
        setEnabled(true);
        enabledChanged();
    }

    auto& aidm = rtengine::AIDenoiseManager::getInstance();

    if (aidm.isDetecting()) {
        updateStatus(M("TP_AIDENOISE_STATUS_LOADING"));
        return;
    }

    if (!aidm.isAvailable()) {
        updateStatus(M("TP_AIDENOISE_STATUS_LOADING"));
        aidm.detect();
        return;
    }

    if (imagePath_.empty()) {
        updateStatus("No image loaded");
        return;
    }

    updateStatus(M("TP_AIDENOISE_STATUS_PREPARING"));
    cancelBtn->set_sensitive(true);

    if (!ipc_) {
        updateStatus("No image processor available");
        cancelBtn->set_sensitive(false);
        return;
    }

    std::unique_ptr<rtengine::Imagefloat> inputImage =
        ipc_->createDemosaicedImage();
    if (!inputImage) {
        updateStatus("Failed to prepare image");
        cancelBtn->set_sensitive(false);
        return;
    }

    updateStatus(M("TP_AIDENOISE_STATUS_PROCESSING"));

    // Read current params from the UI
    rtengine::procparams::AIDenoiseParams params;
    params.enabled = getEnabled();
    params.isoConditioning = isoConditioning->getValue();
    params.blend = blend->getValue();
    params.useGpu = useGpu->get_active();

    // Get actual ISO from image metadata
    int actualIso = 0;
    if (ipc_) {
        const auto* md = ipc_->getInitialImage()->getMetaData();
        if (md) {
            actualIso = md->getISOSpeed();
        }
    }
    fprintf(stderr, "AI Denoise: actual ISO from EXIF = %d\n", actualIso);

    const Glib::ustring requestedImagePath = imagePath_;
    aidm.startDenoising(
        imagePath_,
        params,
        std::move(inputImage),
        // Progress callback — dispatch to GTK main thread
        [this](double progress) {
            Glib::signal_idle().connect_once([this, progress]() {
                Glib::ustring msg = Glib::ustring::compose(
                    M("TP_AIDENOISE_STATUS_PROCESSING") + " %1%%",
                    static_cast<int>(progress));
                updateStatus(msg);
            });
        },
        // Done callback — dispatch to GTK main thread via idle_add (thread-safe)
        [this, requestedImagePath](bool success, const Glib::ustring& message) {
            fprintf(stderr, "AI Denoise: doneCb called success=%d, dispatching to main thread\n", (int)success);
            struct IdleData {
                AIDenoise* self;
                bool success;
                Glib::ustring message;
                Glib::ustring imagePath;
            };
            auto* data = new IdleData{this, success, message, requestedImagePath};
            g_idle_add([](gpointer user_data) -> gboolean {
                fprintf(stderr, "AI Denoise: g_idle_add callback FIRED\n");
                auto* d = static_cast<IdleData*>(user_data);
                auto* self = d->self;
                bool success = d->success;
                Glib::ustring message = d->message;
                Glib::ustring imagePath = d->imagePath;
                delete d;

                self->cancelBtn->set_sensitive(false);
                if (success) {
                    self->updateStatus(M("TP_AIDENOISE_STATUS_COMPLETE"));
                    fprintf(stderr, "AI Denoise: Done callback - listener=%p enabled=%d\n",
                            (void*)self->listener, (int)self->getEnabled());
                    if (self->imagePath_ == imagePath
                            && self->listener
                            && self->getEnabled()) {
                        fprintf(stderr, "AI Denoise: Firing panelChanged(EvAIDNBlend)\n");
                        self->listener->panelChanged(EvAIDNBlend,
                            Glib::ustring::format(std::setw(2), std::fixed,
                                std::setprecision(1), self->blend->getValue()));
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
                    } else if (message.find("model") != Glib::ustring::npos
                               && message.find("not found") != Glib::ustring::npos) {
                        cleanMsg = M("TP_AIDENOISE_STATUS_NOT_AVAILABLE");
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
                    self->updateStatus(cleanMsg);
                }
                return G_SOURCE_REMOVE;
            }, data);
        },
        actualIso
    );
}

void AIDenoise::onCancelClicked ()
{
    rtengine::AIDenoiseManager::getInstance().cancel();
    updateStatus(M("TP_AIDENOISE_STATUS_CANCELLED"));
    cancelBtn->set_sensitive(false);
}

void AIDenoise::updateStatus (const Glib::ustring& text)
{
    statusLabel->set_text(text);
}
