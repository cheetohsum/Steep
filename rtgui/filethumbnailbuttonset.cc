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
#include <algorithm>

#include "filethumbnailbuttonset.h"

#include "multilangmgr.h"
#include "rtsurface.h"
#include "widgets/basic/lwbutton.h"
#include "widgets/basic/lwbuttonset.h"

bool FileThumbnailButtonSet::iconsLoaded = false;

std::shared_ptr<RTSurface> FileThumbnailButtonSet::rankIcon = std::shared_ptr<RTSurface>(nullptr);
std::shared_ptr<RTSurface> FileThumbnailButtonSet::gRankIcon = std::shared_ptr<RTSurface>(nullptr);
std::shared_ptr<RTSurface> FileThumbnailButtonSet::unRankIcon = std::shared_ptr<RTSurface>(nullptr);
std::shared_ptr<RTSurface> FileThumbnailButtonSet::trashIcon = std::shared_ptr<RTSurface>(nullptr);
std::shared_ptr<RTSurface> FileThumbnailButtonSet::unTrashIcon = std::shared_ptr<RTSurface>(nullptr);
std::shared_ptr<RTSurface> FileThumbnailButtonSet::processIcon = std::shared_ptr<RTSurface>(nullptr);
std::array<std::shared_ptr<RTSurface>, 6> FileThumbnailButtonSet::colorLabelIcon;
std::shared_ptr<RTSurface> FileThumbnailButtonSet::pickIcon = std::shared_ptr<RTSurface>(nullptr);
std::shared_ptr<RTSurface> FileThumbnailButtonSet::rejectIcon = std::shared_ptr<RTSurface>(nullptr);

Glib::ustring FileThumbnailButtonSet::processToolTip;
Glib::ustring FileThumbnailButtonSet::unrankToolTip;
Glib::ustring FileThumbnailButtonSet::trashToolTip;
Glib::ustring FileThumbnailButtonSet::untrashToolTip;
Glib::ustring FileThumbnailButtonSet::colorLabelToolTip;
std::array<Glib::ustring, 5> FileThumbnailButtonSet::rankToolTip;

void FileThumbnailButtonSet::ensureIconsLoaded ()
{
    if (!iconsLoaded) {
        unRankIcon  = std::shared_ptr<RTSurface>(new RTSurface("star-hollow-small", Gtk::ICON_SIZE_BUTTON));
        rankIcon    = std::shared_ptr<RTSurface>(new RTSurface("star-gold-small", Gtk::ICON_SIZE_BUTTON));
        gRankIcon   = std::shared_ptr<RTSurface>(new RTSurface("star-small", Gtk::ICON_SIZE_BUTTON));
        trashIcon   = std::shared_ptr<RTSurface>(new RTSurface("trash-small", Gtk::ICON_SIZE_BUTTON));
        unTrashIcon = std::shared_ptr<RTSurface>(new RTSurface("trash-remove-small", Gtk::ICON_SIZE_BUTTON));
        processIcon = std::shared_ptr<RTSurface>(new RTSurface("gears-small", Gtk::ICON_SIZE_BUTTON));
        colorLabelIcon[0] = std::shared_ptr<RTSurface>(new RTSurface("circle-empty-gray-small", Gtk::ICON_SIZE_MENU));
        colorLabelIcon[1] = std::shared_ptr<RTSurface>(new RTSurface("circle-red-small", Gtk::ICON_SIZE_MENU));
        colorLabelIcon[2] = std::shared_ptr<RTSurface>(new RTSurface("circle-yellow-small", Gtk::ICON_SIZE_MENU));
        colorLabelIcon[3] = std::shared_ptr<RTSurface>(new RTSurface("circle-green-small", Gtk::ICON_SIZE_MENU));
        colorLabelIcon[4] = std::shared_ptr<RTSurface>(new RTSurface("circle-blue-small", Gtk::ICON_SIZE_MENU));
        colorLabelIcon[5] = std::shared_ptr<RTSurface>(new RTSurface("circle-purple-small", Gtk::ICON_SIZE_MENU));
        pickIcon = std::shared_ptr<RTSurface>(new RTSurface("flag-pick", Gtk::ICON_SIZE_BUTTON));
        rejectIcon = std::shared_ptr<RTSurface>(new RTSurface("flag-reject", Gtk::ICON_SIZE_BUTTON));

        processToolTip = M("FILEBROWSER_POPUPPROCESS");
        unrankToolTip = M("FILEBROWSER_UNRANK_TOOLTIP");
        trashToolTip = M("FILEBROWSER_POPUPTRASH");
        untrashToolTip = M("FILEBROWSER_POPUPUNTRASH");
        colorLabelToolTip = M("FILEBROWSER_COLORLABEL_TOOLTIP");
        rankToolTip[0] = M("FILEBROWSER_RANK1_TOOLTIP");
        rankToolTip[1] = M("FILEBROWSER_RANK2_TOOLTIP");
        rankToolTip[2] = M("FILEBROWSER_RANK3_TOOLTIP");
        rankToolTip[3] = M("FILEBROWSER_RANK4_TOOLTIP");
        rankToolTip[4] = M("FILEBROWSER_RANK5_TOOLTIP");

        iconsLoaded = true;
    }
}

FileThumbnailButtonSet::FileThumbnailButtonSet (FileBrowserEntry* myEntry)
{
    ensureIconsLoaded();

    constexpr double overlayScale = 0.78;

    add(new LWButton(unRankIcon, 0, myEntry, LWButton::Left, LWButton::Center, &unrankToolTip, overlayScale));

    for (int i = 0; i < 5; i++) {
        add(new LWButton(rankIcon, i + 1, myEntry, LWButton::Left, LWButton::Center, &rankToolTip[i], overlayScale));
    }

    add(new LWButton(colorLabelIcon[0], 8, myEntry, LWButton::Left, LWButton::Center, &colorLabelToolTip, overlayScale));
}

void FileThumbnailButtonSet::arrangeButtons (int x, int y, int w, int h)
{
    constexpr double preferredScale = 0.78;
    constexpr int groupGap = 2;
    constexpr int ratingButtonCount = 6;
    constexpr int labelButtonIndex = 6;

    int naturalWidth = 0;
    for (auto* button : buttons) {
        const auto icon = button->getIcon();
        if (icon) {
            naturalWidth += icon->getWidth();
        }
    }

    double scale = preferredScale;
    if (w >= 0 && naturalWidth > 0) {
        scale = std::min(
            preferredScale,
            std::max(0.35, static_cast<double>(std::max(1, w - groupGap)) / naturalWidth)
        );
    }

    for (auto* button : buttons) {
        button->setScale(scale);
    }

    int labelWidth = 0;
    int labelHeight = 0;
    buttons[labelButtonIndex]->getSize(labelWidth, labelHeight);

    int ratingWidth = 0;
    int ratingHeight = 0;
    for (int i = 0; i < ratingButtonCount; ++i) {
        int buttonWidth = 0;
        int buttonHeight = 0;
        buttons[i]->getSize(buttonWidth, buttonHeight);
        ratingWidth += buttonWidth;
        ratingHeight = std::max(ratingHeight, buttonHeight);
    }

    const int minimumWidth = labelWidth + groupGap + ratingWidth;
    if (w < 0) {
        w = minimumWidth;
    }
    if (h < 0) {
        h = std::max(labelHeight, ratingHeight);
    }

    int ratingX = x + (w - ratingWidth) / 2;
    int labelX = ratingX - groupGap - labelWidth;

    if (labelX < x) {
        labelX = x + std::max(0, (w - minimumWidth) / 2);
        ratingX = labelX + labelWidth + groupGap;
    }

    buttons[labelButtonIndex]->setPosition(labelX, y + (h - labelHeight) / 2);

    int buttonX = ratingX;
    for (int i = 0; i < ratingButtonCount; ++i) {
        int buttonWidth = 0;
        int buttonHeight = 0;
        buttons[i]->getSize(buttonWidth, buttonHeight);
        buttons[i]->setPosition(buttonX, y + (h - buttonHeight) / 2);
        buttonX += buttonWidth;
    }

    aw = w;
    ah = h;
    ax = x;
    ay = y;
}

void FileThumbnailButtonSet::setRank (int stars)
{
    currentRank_ = stars;
    for (int i = 1; i <= 5; i++) {
        buttons[i]->setIcon(i <= stars ? rankIcon : gRankIcon);
    }
}

void FileThumbnailButtonSet::setColorLabel (int colorLabel)
{
    currentColorLabel_ = colorLabel;
    if (colorLabel >= 0 && colorLabel <= 5) {
        buttons[6]->setIcon(colorLabelIcon[colorLabel]);
    }
}

