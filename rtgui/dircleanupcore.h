/*
 *  This file is part of RawTherapee / Steep.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#pragma once

// Directory cleanup: the pure part (no GTK widgets). Folder names become date
// spans, photos become groups (a RAW, its JPEG, sidecars and edits move as
// one), and misplaced groups become (source folder -> destination folder)
// suggestions with a confidence score. Dates come from an injected reader so
// the logic can be exercised headlessly (tools/dircleanup_tests.cc).

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace dircleanup
{

// Days since 1970-01-01 (proleptic Gregorian).
int dayNumber(int y, int m, int d);
void dayToYMD(int day, int& y, int& m, int& d);

enum class FolderKind {
    Month,   // only date words: "July 2024", "AugustSeptember2024", "7_24_23"
    Event,   // date + other words: "August 2023 - Taliesin" (never moved into or out of)
    Purpose, // no month: "Kimo", "toprint", "OneDrive2025"
    Root     // a starred place itself: loose photos there may be filed away
};

struct Span {
    int first;  // inclusive day numbers
    int last;
};

struct FolderInfo {
    FolderKind kind = FolderKind::Purpose;
    std::vector<Span> spans;
    bool covers(int day) const;
    int width() const;   // total days covered
};

FolderInfo parseFolderName(const std::string& name);

enum class DateSource { None, Exif, FileName, Run };

struct Group {
    std::string dir;
    std::string stem;                 // case-preserved stem of the primary file
    std::vector<std::string> files;   // full paths: media, sidecars, edits
    std::vector<std::string> dirs;    // the photo's own data folders (*.steep-*), same drive only
    std::string primary;              // the file whose date counts (RAW, else JPEG, ...)
    std::string prefix;               // "DSCF", "_DSF", "IMG_" ... (empty if none)
    long number = -1;                 // sequence number after the prefix
    int day = -1;                     // capture day number, -1 unknown
    int minute = -1;                  // minute of day, -1 unknown
    DateSource source = DateSource::None;
    long long bytes = 0;
    bool derivative = false;          // an edit/export with no original beside it
};

struct FolderScan {
    std::string path;
    std::string name;
    std::string parent;
    std::string root;                 // the starred place it was found under
    FolderInfo info;
    std::vector<Group> groups;
};

// (path, y, m, d, hour, minute) -> found
using DateReader = std::function<bool(const std::string&, int&, int&, int&, int&, int&)>;

// [DateTime] Year/Month/Day/Hour/Min from a thumbnail-cache data file.
bool readCacheDateTime(const std::string& dataFile, int& y, int& m, int& d, int& hh, int& mm);

// Capture date read straight from the file's Exif block (JPEG, RAF preview,
// TIFF-based raws): one small read, no metadata library.
bool exifDateFast(const std::string& path, int& y, int& m, int& d, int& hh, int& mm);

// Date from the file name itself: IMG_20240712_..., 2024-07-12 ..., 20240712_101500, PXL_...
bool dateFromFileName(const std::string& fileName, int& y, int& m, int& d, int& hh, int& mm);

// Folders under the starred places (nested places folded in), depth-limited,
// hidden folders skipped. Roots come first.
std::vector<std::pair<std::string, std::string>> listFolders(const std::vector<std::string>& roots, int maxDepth);

// One folder: groups its photos and dates them (reader first, then the file
// name). `exts` are lower-case photo extensions without the dot.
FolderScan scanFolder(const std::string& dir, const std::string& root, const std::set<std::string>& exts,
                      const DateReader& reader);

// Fill missing dates from sequence neighbours that agree on the month.
void inferRunDates(FolderScan& folder);

enum class Reason : char { Date = 'd', Sequence = '#', FileName = 'A' };

struct Suggestion {
    std::string source;
    std::string dest;            // existing folder, or the folder to create
    bool createDest = false;
    std::vector<Group> groups;
    int firstDay = 0;
    int lastDay = 0;
    double score = 0.0;
    Reason reason = Reason::Date;
    bool crossDrive = false;
    bool alternative = false;    // another folder would also fit
    long long bytes = 0;
    int dots() const { return score >= 0.85 ? 3 : score >= 0.65 ? 2 : 1; }
};

std::vector<Suggestion> suggest(const std::vector<FolderScan>& folders);

// "July 2024" in the same spelling style as the folders around it.
std::string monthFolderName(int y, int m);

std::string driveOf(const std::string& path);
std::string lowerAscii(std::string s);

}
