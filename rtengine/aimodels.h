/*
 *  This file is part of RawTherapee.
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

#include <string>
#include <vector>

#include <glibmm/ustring.h>

namespace rtengine
{

class Settings;

/**
 * Which AI model does which job. Several jobs have more than one model that
 * fits (a small fast one and a large accurate one, typically); this lists
 * what is installed for each and resolves the user's choice to files.
 *
 * Models live in two places: `models/` beside the application and `models/`
 * in the user's settings directory (where anything fetched after installation
 * goes). A model is one file, or a set of fixed file names in one directory;
 * alternatives that share file names (every SAM 2.1 size does) sit in their
 * own sub-directory of either root.
 */
namespace aimodels
{

enum class Role {
    Scene,     ///< sky / person / building ... classes for AI masks
    Subject,   ///< the "subject" mask
    Select,    ///< click selection: Remove Object picks, mask picks, partner cut-outs
    WideFill,  ///< Remove Object fill for holes too wide for the standard model
};

struct Choice {
    std::string id;          ///< stored in the options file
    Glib::ustring name;      ///< shown as is: model names are not translated
    Glib::ustring location;  ///< the file, or the directory holding the set
    long long megabytes = 0; ///< on disk
};

/// Recorded once by rtengine::init.
void setRoots(const Glib::ustring& applicationModels, const Glib::ustring& userModels);

/// What is installed for a job, the default first.
std::vector<Choice> installed(Role role);

/// The model a job uses: `wanted` when it is installed, the default
/// otherwise (also for an empty `wanted`, which means "automatic"). An empty
/// id comes back when nothing is installed -- or, for WideFill, when the
/// choice is "off".
Choice resolve(Role role, const Glib::ustring& wanted);

/// The id that turns the optional wide fill off.
extern const char* const kOff;

/// Points the engines that can change model while running (click selection,
/// wide fill, how long idle models stay loaded) at the current choices. The
/// scene and subject models are bound when the engine starts.
void applyLive(const Settings& settings);

}

}
