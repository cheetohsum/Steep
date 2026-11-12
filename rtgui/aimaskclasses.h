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

#include <vector>

// The AI mask classes, in the one order every menu shows them: the smart
// mask chip, the mask panel's + menu, the class picker on an existing mask
// and the AI Mask tool's combo. Creating a mask and changing it later must
// offer the same choices. Indices are rtengine::AISegClass values, as plain
// ints so widgets build without the AI masking headers.
namespace aimaskclasses
{

struct Entry {
    int classIndex;
    const char* icon;
    const char* labelKey;
    bool separatorBefore;
};

inline const std::vector<Entry>& menu()
{
    static const std::vector<Entry> entries = {
        {8, "mask-ai", "TP_LOCALLAB_AIMASK_CLASS_SUBJECT", false},
        {9, "mask-ai", "TP_LOCALLAB_AIMASK_CLASS_NOTSUBJECT", false},
        {1, "mask-class-person", "TP_LOCALLAB_AIMASK_CLASS_PERSON", true},
        {2, "mask-class-sky", "TP_LOCALLAB_AIMASK_CLASS_SKY", false},
        {3, "mask-class-vegetation", "TP_LOCALLAB_AIMASK_CLASS_VEGETATION", false},
        {4, "mask-class-building", "TP_LOCALLAB_AIMASK_CLASS_BUILDING", false},
        {5, "mask-class-vehicle", "TP_LOCALLAB_AIMASK_CLASS_VEHICLE", false},
        {6, "mask-class-animal", "TP_LOCALLAB_AIMASK_CLASS_ANIMAL", false},
        {7, "mask-class-foreground", "TP_LOCALLAB_AIMASK_CLASS_FOREGROUND", false},
        {0, "mask-class-background", "TP_LOCALLAB_AIMASK_CLASS_BACKGROUND", false},
        // By depth, not by kind: split where the scene's depths fall apart.
        {10, "mask-depth-near", "TP_LOCALLAB_AIMASK_CLASS_NEAR", true},
        {11, "mask-depth-far", "TP_LOCALLAB_AIMASK_CLASS_FAR", false},
    };
    return entries;
}

// Number of class indices (rtengine::AISegClass::TOTAL_CLASSES).
inline int classCount()
{
    int count = 0;
    for (const Entry& e : menu()) {
        count = e.classIndex + 1 > count ? e.classIndex + 1 : count;
    }
    return count;
}

// Position of a class in menu(), or -1.
inline int rowOf(int classIndex)
{
    const auto& entries = menu();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].classIndex == classIndex) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Class at a menu() position, or -1.
inline int classAt(int row)
{
    const auto& entries = menu();
    return row >= 0 && row < static_cast<int>(entries.size()) ? entries[row].classIndex : -1;
}

// Translation key naming a class (falls back to Background).
inline const char* labelKey(int classIndex)
{
    const int row = rowOf(classIndex);
    return row >= 0 ? menu()[row].labelKey : "TP_LOCALLAB_AIMASK_CLASS_BACKGROUND";
}

}
