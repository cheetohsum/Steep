/*
 *  This file is part of RawTherapee / Steep.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#include "dircleanupcore.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

namespace dircleanup
{

namespace
{

const char* const kMonths[12] = {"january", "february", "march", "april", "may", "june", "july",
                                 "august", "september", "october", "november", "december"};
const char* const kMonthNames[12] = {"January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December"};

int daysInMonth(int y, int m)
{
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        return 29;
    }
    return dim[(m - 1) % 12];
}

bool validDate(int y, int m, int d)
{
    return y >= 1990 && y <= 2099 && m >= 1 && m <= 12 && d >= 1 && d <= daysInMonth(y, m);
}

// Month at the start of `w` (full name, or a 3-letter form when the rest of
// the word is empty or another month). Returns its length, 0 if none.
int monthAt(const std::string& w, size_t pos, int& month);

int fullMonthAt(const std::string& w, size_t pos, int& month)
{
    for (int i = 0; i < 12; ++i) {
        const size_t len = std::strlen(kMonths[i]);
        if (w.compare(pos, len, kMonths[i]) == 0) {
            month = i + 1;
            return static_cast<int>(len);
        }
    }
    // "sept"
    if (w.compare(pos, 4, "sept") == 0) {
        month = 9;
        return 4;
    }
    return 0;
}

int monthAt(const std::string& w, size_t pos, int& month)
{
    const int full = fullMonthAt(w, pos, month);
    if (full) {
        return full;
    }
    for (int i = 0; i < 12; ++i) {
        if (w.compare(pos, 3, std::string(kMonths[i], 3)) == 0) {
            const size_t rest = pos + 3;
            int dummy = 0;
            if (rest == w.size() || fullMonthAt(w, rest, dummy) || w.compare(rest, 3, "and") == 0) {
                month = i + 1;
                return 3;
            }
        }
    }
    return 0;
}

bool isConnector(const std::string& w)
{
    return w == "and" || w == "to" || w == "thru" || w == "through" || w == "n";
}

struct Token {
    enum Kind { Month, Year, Number, Word } kind;
    int value;
    char sepBefore;   // separator char before this token ('\0' if none / letter-digit switch)
};

std::string baseName(const std::string& path)
{
    return Glib::path_get_basename(path);
}

bool isRawExt(const std::string& ext)
{
    static const std::set<std::string> nonRaw = {"jpg", "jpeg", "png", "tif", "tiff", "heic", "heif",
                                                 "webp", "avif", "jxl", "bmp", "gif"};
    return !nonRaw.count(ext);
}

int primaryRank(const std::string& ext)
{
    if (isRawExt(ext)) {
        return 0;
    }
    if (ext == "jpg" || ext == "jpeg" || ext == "heic" || ext == "heif") {
        return 1;
    }
    return 2;
}

// "_copy", "-edit", " copy" ... at the end of a stem; returns the original's stem.
bool derivativeBase(const std::string& stemLower, std::string& base)
{
    static const char* const tails[] = {"_copy", "-copy", " copy", "_edit", "-edit", " edit", "_edited",
                                        "-edited", "_export", "-export"};
    for (const char* t : tails) {
        const size_t n = std::strlen(t);
        if (stemLower.size() > n && stemLower.compare(stemLower.size() - n, n, t) == 0) {
            base = stemLower.substr(0, stemLower.size() - n);
            return true;
        }
    }
    return false;
}

void sequenceOf(const std::string& stem, std::string& prefix, long& number)
{
    prefix.clear();
    number = -1;
    size_t i = 0;
    while (i < stem.size() && !std::isdigit(static_cast<unsigned char>(stem[i]))) {
        ++i;
    }
    if (i > 5) {
        return;
    }
    size_t j = i;
    while (j < stem.size() && std::isdigit(static_cast<unsigned char>(stem[j]))) {
        ++j;
    }
    const size_t digits = j - i;
    if (digits < 3 || digits > 6) {
        return;   // dates and timestamps are not sequence numbers
    }
    prefix = stem.substr(0, i);
    number = std::strtol(stem.substr(i, digits).c_str(), nullptr, 10);
}

bool coveredWithSlack(const FolderInfo& info, int day, int minute)
{
    if (info.covers(day)) {
        return true;
    }
    // Six hours either side of the folder's edge: a party past midnight, a
    // camera clock in another time zone.
    for (const auto& s : info.spans) {
        if (day == s.last + 1 && minute >= 0 && minute < 360) {
            return true;
        }
        if (day == s.first - 1 && minute >= 1080) {
            return true;
        }
    }
    return false;
}

bool nearEdge(const FolderInfo& info, int day)
{
    for (const auto& s : info.spans) {
        if (std::abs(day - s.first) <= 1 || std::abs(day - s.last) <= 1) {
            return true;
        }
    }
    return false;
}

std::string parentOf(const std::string& path)
{
    return Glib::path_get_dirname(path);
}

bool samePath(const std::string& a, const std::string& b)
{
    return lowerAscii(a) == lowerAscii(b);
}

bool isUnder(const std::string& path, const std::string& root)
{
    const std::string p = lowerAscii(path), r = lowerAscii(root);
    if (p.size() <= r.size() || p.compare(0, r.size(), r) != 0) {
        return false;
    }
    const char c = p[r.size()];
    return c == '\\' || c == '/' || r.back() == '\\' || r.back() == '/';
}

} // namespace

std::string lowerAscii(std::string s)
{
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string driveOf(const std::string& path)
{
    if (path.size() >= 2 && path[1] == ':') {
        return lowerAscii(path.substr(0, 2));
    }
    if (path.size() >= 2 && (path[0] == '\\' || path[0] == '/') && path[1] == path[0]) {
        const size_t s = path.find_first_of("\\/", 2);
        return lowerAscii(path.substr(0, s));
    }
    return "/";
}

int dayNumber(int y, int m, int d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

void dayToYMD(int z, int& y, int& m, int& d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yy = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = yy + (m <= 2);
}

bool FolderInfo::covers(int day) const
{
    for (const auto& s : spans) {
        if (day >= s.first && day <= s.last) {
            return true;
        }
    }
    return false;
}

int FolderInfo::width() const
{
    int w = 0;
    for (const auto& s : spans) {
        w += s.last - s.first + 1;
    }
    return w;
}

FolderInfo parseFolderName(const std::string& name)
{
    FolderInfo info;
    const std::string s = lowerAscii(name);

    // Split into letter runs, digit runs and separators.
    std::vector<Token> tokens;
    bool otherWords = false;
    char sep = '\0';
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isalpha(c)) {
            size_t j = i;
            while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) {
                ++j;
            }
            const std::string word = s.substr(i, j - i);
            // Months can be run together: "augustseptember", "januaryandfebruary".
            size_t p = 0;
            while (p < word.size()) {
                int month = 0;
                const int len = monthAt(word, p, month);
                if (len) {
                    tokens.push_back({Token::Month, month, sep});
                    sep = '\0';
                    p += len;
                    continue;
                }
                if (word.compare(p, 3, "and") == 0) {
                    p += 3;
                    continue;
                }
                // Not a month: the rest of the word is an ordinary word.
                const std::string rest = word.substr(p);
                if (!isConnector(rest)) {
                    otherWords = true;
                    tokens.push_back({Token::Word, 0, sep});
                }
                sep = '\0';
                break;
            }
            i = j;
        } else if (std::isdigit(c)) {
            size_t j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
                ++j;
            }
            const std::string digits = s.substr(i, j - i);
            const int v = std::atoi(digits.c_str());
            if (digits.size() == 4 && v >= 1990 && v <= 2099) {
                tokens.push_back({Token::Year, v, sep});
            } else if (digits.size() <= 2) {
                tokens.push_back({Token::Number, v, sep});
            } else {
                tokens.push_back({Token::Word, 0, sep});
                otherWords = true;
            }
            sep = '\0';
            i = j;
        } else {
            if (c == '-' || c == '_' || c == '.' || c == ' ' || c == '&' || c == ',') {
                sep = c == ' ' && sep ? sep : static_cast<char>(c);
            }
            ++i;
        }
    }

    std::vector<std::pair<int, int>> months;   // (year, month)
    std::vector<Span> days;

    // Numeric dates first: M_D_YY / M-D-YYYY, YYYY-MM-DD, YYYY-MM.
    std::vector<bool> used(tokens.size(), false);
    for (size_t t = 0; t < tokens.size(); ++t) {
        if (used[t]) {
            continue;
        }
        const auto sepOk = [&](size_t k) {
            return tokens[k].sepBefore == '-' || tokens[k].sepBefore == '_' || tokens[k].sepBefore == '.';
        };
        if (tokens[t].kind == Token::Year && t + 1 < tokens.size() && tokens[t + 1].kind == Token::Number && sepOk(t + 1)) {
            const int y = tokens[t].value, m = tokens[t + 1].value;
            if (m >= 1 && m <= 12) {
                if (t + 2 < tokens.size() && tokens[t + 2].kind == Token::Number && sepOk(t + 2)
                    && validDate(y, m, tokens[t + 2].value)) {
                    const int dn = dayNumber(y, m, tokens[t + 2].value);
                    days.push_back({dn, dn});
                    used[t] = used[t + 1] = used[t + 2] = true;
                } else {
                    months.emplace_back(y, m);
                    used[t] = used[t + 1] = true;
                }
                continue;
            }
        }
        if (tokens[t].kind == Token::Number && t + 2 < tokens.size() && tokens[t + 1].kind == Token::Number
            && sepOk(t + 1) && sepOk(t + 2)
            && (tokens[t + 2].kind == Token::Number || tokens[t + 2].kind == Token::Year)) {
            int y = tokens[t + 2].value;
            if (tokens[t + 2].kind == Token::Number) {
                y += 2000;
            }
            const int m = tokens[t].value, d = tokens[t + 1].value;
            if (validDate(y, m, d)) {
                const int dn = dayNumber(y, m, d);
                days.push_back({dn, dn});
                used[t] = used[t + 1] = used[t + 2] = true;
                continue;
            }
        }
    }

    // Month words: a year binds to the months before it that have none
    // (with a wrap: "DecemberJanuary2025" is Dec 2024 + Jan 2025); months
    // after the last year take that year.
    std::vector<int> pending;
    int lastYear = 0;
    for (size_t t = 0; t < tokens.size(); ++t) {
        if (used[t]) {
            continue;
        }
        if (tokens[t].kind == Token::Month) {
            pending.push_back(tokens[t].value);
            used[t] = true;
        } else if (tokens[t].kind == Token::Year) {
            used[t] = true;
            lastYear = tokens[t].value;
            if (!pending.empty()) {
                int y = lastYear;
                for (size_t k = pending.size(); k-- > 0;) {
                    if (k + 1 < pending.size() && pending[k] > pending[k + 1]) {
                        --y;
                    }
                    months.emplace_back(y, pending[k]);
                }
                pending.clear();
            }
        } else if (tokens[t].kind == Token::Number) {
            otherWords = true;   // "Trip 2", a day number on its own
        }
    }
    if (!pending.empty() && lastYear) {
        for (int m : pending) {
            months.emplace_back(lastYear, m);
        }
        pending.clear();
    }
    if (!pending.empty()) {
        otherWords = true;   // a month with no year can't be placed
    }

    for (const auto& ym : months) {
        days.push_back({dayNumber(ym.first, ym.second, 1),
                        dayNumber(ym.first, ym.second, daysInMonth(ym.first, ym.second))});
    }
    std::sort(days.begin(), days.end(), [](const Span& a, const Span& b) { return a.first < b.first; });
    for (const auto& d : days) {
        if (!info.spans.empty() && d.first <= info.spans.back().last + 1) {
            info.spans.back().last = std::max(info.spans.back().last, d.last);
        } else {
            info.spans.push_back(d);
        }
    }

    if (info.spans.empty()) {
        info.kind = FolderKind::Purpose;
    } else if (otherWords) {
        info.kind = FolderKind::Event;
    } else {
        info.kind = FolderKind::Month;
    }
    return info;
}

namespace
{

unsigned rd16(const unsigned char* p, bool le)
{
    return le ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]);
}

unsigned rd32(const unsigned char* p, bool le)
{
    return le ? (p[0] | (p[1] << 8) | (p[2] << 16) | (unsigned(p[3]) << 24))
              : ((unsigned(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

bool parseExifStamp(const unsigned char* s, size_t n, int& y, int& m, int& d, int& hh, int& mm)
{
    // "YYYY:MM:DD HH:MM:SS"
    if (n < 16) {
        return false;
    }
    char buf[20] = {};
    std::memcpy(buf, s, std::min<size_t>(19, n));
    if (std::sscanf(buf, "%4d:%2d:%2d %2d:%2d", &y, &m, &d, &hh, &mm) != 5) {
        return false;
    }
    return validDate(y, m, d) && hh >= 0 && hh < 24 && mm >= 0 && mm < 60;
}

// DateTimeOriginal (Exif IFD), else DateTime (IFD0), from a TIFF block.
bool tiffDate(const unsigned char* t, size_t n, int& y, int& m, int& d, int& hh, int& mm)
{
    if (n < 8) {
        return false;
    }
    const bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) {
        return false;
    }
    auto stampAt = [&](size_t entry, int& yy, int& mo, int& dd, int& h, int& mi) {
        const unsigned count = rd32(t + entry + 4, le);
        const unsigned off = rd32(t + entry + 8, le);
        return count >= 19 && off + 19 <= n && parseExifStamp(t + off, 19, yy, mo, dd, h, mi);
    };
    const unsigned ifd0 = rd32(t + 4, le);
    if (ifd0 + 2 > n) {
        return false;
    }
    const unsigned entries = rd16(t + ifd0, le);
    unsigned exifIfd = 0;
    bool haveFallback = false;
    int fy = 0, fm = 0, fd = 0, fh = 0, fmi = 0;
    for (unsigned i = 0; i < entries; ++i) {
        const size_t e = ifd0 + 2 + i * 12;
        if (e + 12 > n) {
            break;
        }
        const unsigned tag = rd16(t + e, le);
        if (tag == 0x8769) {
            exifIfd = rd32(t + e + 8, le);
        } else if (tag == 0x0132) {
            haveFallback = stampAt(e, fy, fm, fd, fh, fmi);
        }
    }
    if (exifIfd && exifIfd + 2 <= n) {
        const unsigned ec = rd16(t + exifIfd, le);
        for (unsigned i = 0; i < ec; ++i) {
            const size_t e = exifIfd + 2 + i * 12;
            if (e + 12 > n) {
                break;
            }
            if (rd16(t + e, le) == 0x9003 && stampAt(e, y, m, d, hh, mm)) {
                return true;
            }
        }
    }
    if (haveFallback) {
        y = fy; m = fm; d = fd; hh = fh; mm = fmi;
        return true;
    }
    return false;
}

// The Exif APP1 of a JPEG held in `b`.
bool jpegDate(const unsigned char* b, size_t n, int& y, int& m, int& d, int& hh, int& mm)
{
    if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) {
        return false;
    }
    size_t p = 2;
    while (p + 4 <= n && b[p] == 0xFF) {
        const unsigned marker = b[p + 1];
        const unsigned len = (b[p + 2] << 8) | b[p + 3];
        if (marker == 0xDA || len < 2) {
            break;
        }
        if (marker == 0xE1 && p + 10 <= n && std::memcmp(b + p + 4, "Exif\0\0", 6) == 0) {
            const size_t start = p + 10;
            const size_t avail = std::min<size_t>(n - start, len - 8);
            return tiffDate(b + start, avail, y, m, d, hh, mm);
        }
        p += 2 + len;
    }
    return false;
}

} // namespace

bool exifDateFast(const std::string& path, int& y, int& m, int& d, int& hh, int& mm)
{
    FILE* f = g_fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::vector<unsigned char> buf(64 * 1024);
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    bool ok = false;
    if (n >= 100 && std::memcmp(buf.data(), "FUJIFILMCCD-RAW", 15) == 0) {
        // RAF: the preview JPEG (with the Exif block) sits at a stored offset.
        const unsigned off = rd32(buf.data() + 84, false);
        if (off && fseek(f, static_cast<long>(off), SEEK_SET) == 0) {
            n = std::fread(buf.data(), 1, buf.size(), f);
            ok = jpegDate(buf.data(), n, y, m, d, hh, mm);
        }
    } else if (n >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
        ok = jpegDate(buf.data(), n, y, m, d, hh, mm);
    } else if (n >= 8 && ((buf[0] == 'I' && buf[1] == 'I') || (buf[0] == 'M' && buf[1] == 'M'))) {
        ok = tiffDate(buf.data(), n, y, m, d, hh, mm);
    }
    std::fclose(f);
    return ok;
}

bool readCacheDateTime(const std::string& dataFile, int& y, int& m, int& d, int& hh, int& mm)
{
    gchar* raw = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(dataFile.c_str(), &raw, &length, nullptr)) {
        return false;
    }
    const std::string text(raw, length);
    g_free(raw);
    const size_t sec = text.find("[DateTime]");
    if (sec == std::string::npos) {
        return false;
    }
    const size_t end = text.find("\n[", sec + 1);
    const std::string body = text.substr(sec, end == std::string::npos ? std::string::npos : end - sec);
    auto value = [&](const char* key) {
        const std::string k = std::string("\n") + key + "=";
        const size_t p = body.find(k);
        return p == std::string::npos ? -1 : std::atoi(body.c_str() + p + k.size());
    };
    y = value("Year");
    m = value("Month");
    d = value("Day");
    hh = value("Hour");
    mm = value("Min");
    return validDate(y, m, d);
}

bool dateFromFileName(const std::string& fileName, int& y, int& m, int& d, int& hh, int& mm)
{
    // Find 8 consecutive digits (optionally 4-2-2 with - or _ between) that
    // make a valid date, then an optional 4-6 digit time after a separator.
    const std::string& s = fileName;
    for (size_t i = 0; i + 8 <= s.size(); ++i) {
        if (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) {
            continue;
        }
        auto digit = [&](size_t k) { return k < s.size() && std::isdigit(static_cast<unsigned char>(s[k])); };
        size_t k = i;
        std::string dd;
        // YYYY
        for (int n = 0; n < 4; ++n) {
            if (!digit(k)) {
                dd.clear();
                break;
            }
            dd += s[k++];
        }
        if (dd.size() != 4) {
            continue;
        }
        const bool sep1 = k < s.size() && (s[k] == '-' || s[k] == '_' || s[k] == '.');
        if (sep1) {
            ++k;
        }
        for (int n = 0; n < 2; ++n) {
            if (!digit(k)) {
                break;
            }
            dd += s[k++];
        }
        if (sep1 && k < s.size() && (s[k] == '-' || s[k] == '_' || s[k] == '.')) {
            ++k;
        }
        for (int n = 0; n < 2; ++n) {
            if (!digit(k)) {
                break;
            }
            dd += s[k++];
        }
        if (dd.size() != 8 || digit(k)) {
            continue;
        }
        y = std::atoi(dd.substr(0, 4).c_str());
        m = std::atoi(dd.substr(4, 2).c_str());
        d = std::atoi(dd.substr(6, 2).c_str());
        if (!validDate(y, m, d)) {
            continue;
        }
        hh = mm = -1;
        if (k < s.size() && (s[k] == '_' || s[k] == '-' || s[k] == ' ' || s[k] == 'T' || s[k] == 't')) {
            size_t t = k + 1;
            std::string tt;
            while (t < s.size() && std::isdigit(static_cast<unsigned char>(s[t])) && tt.size() < 6) {
                tt += s[t++];
            }
            if (tt.size() >= 4) {
                const int h = std::atoi(tt.substr(0, 2).c_str());
                const int mi = std::atoi(tt.substr(2, 2).c_str());
                if (h < 24 && mi < 60) {
                    hh = h;
                    mm = mi;
                }
            }
        }
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> listFolders(const std::vector<std::string>& rootsIn, int maxDepth)
{
    // Fold starred places nested under another one into it.
    std::vector<std::string> roots;
    for (const auto& r : rootsIn) {
        if (r.empty() || !Glib::file_test(r, Glib::FILE_TEST_IS_DIR)) {
            continue;
        }
        bool nested = false;
        for (const auto& other : rootsIn) {
            if (&other != &r && !other.empty() && (isUnder(r, other))) {
                nested = true;
                break;
            }
        }
        bool dup = false;
        for (const auto& kept : roots) {
            dup = dup || samePath(kept, r);
        }
        if (!nested && !dup) {
            roots.push_back(r);
        }
    }

    std::vector<std::pair<std::string, std::string>> out;   // (folder, root)
    struct Item { std::string path; int depth; };
    for (const auto& root : roots) {
        std::vector<Item> stack{{root, 0}};
        while (!stack.empty()) {
            Item it = stack.back();
            stack.pop_back();
            out.emplace_back(it.path, root);
            if (it.depth >= maxDepth) {
                continue;
            }
            std::vector<std::string> children;
            try {
                Glib::Dir dir(it.path);
                for (const auto& n : dir) {
                    const std::string name = n;
                    if (name.empty() || name[0] == '.' || name[0] == '$'
                        || name.find(".steep-") != std::string::npos) {
                        continue;   // hidden, system, or a photo's own data folder
                    }
                    const std::string child = Glib::build_filename(it.path, name);
                    if (Glib::file_test(child, Glib::FILE_TEST_IS_DIR) && !Glib::file_test(child, Glib::FILE_TEST_IS_SYMLINK)) {
                        children.push_back(child);
                    }
                }
            } catch (...) {}
            std::sort(children.rbegin(), children.rend());
            for (const auto& c : children) {
                stack.push_back({c, it.depth + 1});
            }
        }
    }
    return out;
}

FolderScan scanFolder(const std::string& dir, const std::string& root, const std::set<std::string>& exts,
                      const DateReader& reader)
{
    FolderScan fs;
    fs.path = dir;
    fs.name = Glib::path_get_basename(dir);
    fs.parent = parentOf(dir);
    fs.root = root;
    fs.info = parseFolderName(fs.name);
    if (samePath(dir, root) && fs.info.kind != FolderKind::Month) {
        fs.info.kind = FolderKind::Root;
    }

    struct Pending {
        std::vector<std::string> media;
        std::vector<std::string> other;
        std::vector<std::string> dirs;
        std::string stem;
    };
    std::map<std::string, Pending> byStem;
    try {
        Glib::Dir d(dir);
        for (const auto& n : d) {
            const std::string name = n;
            const std::string full = Glib::build_filename(dir, name);
            const size_t firstDot = name.find('.');
            if (Glib::file_test(full, Glib::FILE_TEST_IS_DIR)) {
                // "DSCF8796.RAF.steep-composite": that photo's own data.
                if (firstDot != std::string::npos && firstDot > 0 && name.find(".steep-") != std::string::npos) {
                    byStem[lowerAscii(name.substr(0, firstDot))].dirs.push_back(full);
                }
                continue;
            }
            if (firstDot == 0 || firstDot == std::string::npos) {
                continue;
            }
            const std::string stem = name.substr(0, firstDot);
            const std::string lastExt = lowerAscii(name.substr(name.rfind('.') + 1));
            auto& p = byStem[lowerAscii(stem)];
            p.stem = stem;
            // A media file has exactly one extension and it is a photo type.
            if (name.rfind('.') == firstDot && exts.count(lastExt)) {
                p.media.push_back(full);
            } else {
                p.other.push_back(full);
            }
        }
    } catch (...) {}

    // Edits beside their original join it.
    std::map<std::string, std::string> mergeInto;
    for (auto& kv : byStem) {
        std::string base;
        if (derivativeBase(kv.first, base)) {
            auto it = byStem.find(base);
            if (it != byStem.end() && !it->second.media.empty()) {
                mergeInto[kv.first] = base;
            }
        }
    }
    for (const auto& mv : mergeInto) {
        auto& from = byStem[mv.first];
        auto& to = byStem[mv.second];
        to.other.insert(to.other.end(), from.media.begin(), from.media.end());
        to.other.insert(to.other.end(), from.other.begin(), from.other.end());
        byStem.erase(mv.first);
    }

    for (auto& kv : byStem) {
        Pending& p = kv.second;
        if (p.media.empty()) {
            continue;   // sidecars with no photo stay where they are
        }
        std::sort(p.media.begin(), p.media.end(), [](const std::string& a, const std::string& b) {
            const auto ea = lowerAscii(a.substr(a.rfind('.') + 1));
            const auto eb = lowerAscii(b.substr(b.rfind('.') + 1));
            return primaryRank(ea) < primaryRank(eb);
        });
        Group g;
        g.dir = dir;
        g.stem = p.stem;
        g.primary = p.media.front();
        g.files = p.media;
        g.files.insert(g.files.end(), p.other.begin(), p.other.end());
        g.dirs = p.dirs;
        std::string base;
        g.derivative = derivativeBase(kv.first, base);
        sequenceOf(p.stem, g.prefix, g.number);
        for (const auto& f : g.files) {
            GStatBuf st;
            if (g_stat(f.c_str(), &st) == 0) {
                g.bytes += static_cast<long long>(st.st_size);
                if (f == g.primary) {
                    g.primarySize = static_cast<long long>(st.st_size);
                }
            }
        }

        int y = 0, m = 0, d = 0, hh = -1, mi = -1;
        bool found = false;
        if (reader) {
            for (const auto& media : p.media) {
                if (reader(media, y, m, d, hh, mi) && validDate(y, m, d)) {
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            g.source = DateSource::Exif;
        } else if (dateFromFileName(baseName(g.primary), y, m, d, hh, mi)) {
            found = true;
            g.source = DateSource::FileName;
        }
        if (found) {
            g.day = dayNumber(y, m, d);
            g.minute = hh >= 0 && mi >= 0 ? hh * 60 + mi : -1;
        }
        fs.groups.push_back(std::move(g));
    }
    std::sort(fs.groups.begin(), fs.groups.end(), [](const Group& a, const Group& b) {
        return lowerAscii(a.stem) < lowerAscii(b.stem);
    });
    return fs;
}

void inferRunDates(FolderScan& folder)
{
    std::vector<Group*> seq;
    for (auto& g : folder.groups) {
        if (g.number >= 0) {
            seq.push_back(&g);
        }
    }
    std::sort(seq.begin(), seq.end(), [](const Group* a, const Group* b) {
        return a->prefix != b->prefix ? a->prefix < b->prefix : a->number < b->number;
    });
    for (size_t i = 0; i < seq.size(); ++i) {
        Group* g = seq[i];
        if (g->day >= 0) {
            continue;
        }
        const Group* prev = nullptr;
        const Group* next = nullptr;
        for (size_t k = i; k-- > 0;) {
            if (seq[k]->prefix != g->prefix || g->number - seq[k]->number > 20) {
                break;
            }
            if (seq[k]->day >= 0 && seq[k]->source != DateSource::Run) {
                prev = seq[k];
                break;
            }
        }
        for (size_t k = i + 1; k < seq.size(); ++k) {
            if (seq[k]->prefix != g->prefix || seq[k]->number - g->number > 20) {
                break;
            }
            if (seq[k]->day >= 0 && seq[k]->source != DateSource::Run) {
                next = seq[k];
                break;
            }
        }
        if (prev && next) {
            int y1, m1, d1, y2, m2, d2;
            dayToYMD(prev->day, y1, m1, d1);
            dayToYMD(next->day, y2, m2, d2);
            if (y1 == y2 && m1 == m2) {
                g->day = prev->day;
                g->minute = -1;
                g->source = DateSource::Run;
            }
        }
    }
}

std::string monthFolderName(int y, int m)
{
    return std::string(kMonthNames[(m - 1) % 12]) + " " + std::to_string(y);
}

std::vector<Suggestion> suggest(const std::vector<FolderScan>& folders)
{
    // Anything inside an event or purpose folder ("_Favorites\LR exports\2024-06",
    // "October 2023\Japan") is the user's own arrangement: never touched.
    std::vector<std::string> blocked;
    for (const auto& f : folders) {
        if (f.info.kind == FolderKind::Event || f.info.kind == FolderKind::Purpose) {
            blocked.push_back(f.path);
        }
    }
    const auto insideBlocked = [&](const std::string& path) {
        for (const auto& b : blocked) {
            if (isUnder(path, b)) {
                return true;
            }
        }
        return false;
    };

    // Month folders that can take photos.
    std::vector<const FolderScan*> targets;
    for (const auto& f : folders) {
        if (f.info.kind == FolderKind::Month && !insideBlocked(f.path)) {
            targets.push_back(&f);
        }
    }

    struct Choice {
        const FolderScan* dest = nullptr;
        bool alternative = false;
        bool create = false;
        std::string createPath;
    };

    std::map<std::pair<std::string, std::string>, Suggestion> rows;
    std::map<std::pair<std::string, std::string>, std::vector<const Group*>> rowGroups;
    std::map<std::pair<std::string, std::string>, bool> rowRunsAgree;
    std::map<std::pair<std::string, std::string>, bool> rowNearEdge;

    for (const auto& src : folders) {
        const bool isMonth = src.info.kind == FolderKind::Month;
        if ((!isMonth && src.info.kind != FolderKind::Root) || insideBlocked(src.path)) {
            continue;
        }

        // Runs: by sequence number where there is one, else by time.
        std::vector<const Group*> order;
        for (const auto& g : src.groups) {
            if (!g.derivative && g.day >= 0) {
                order.push_back(&g);
            }
        }
        std::sort(order.begin(), order.end(), [](const Group* a, const Group* b) {
            if ((a->number >= 0) != (b->number >= 0)) {
                return a->number >= 0;
            }
            if (a->number >= 0 && a->prefix != b->prefix) {
                return a->prefix < b->prefix;
            }
            if (a->number >= 0) {
                return a->number < b->number;
            }
            return a->day != b->day ? a->day < b->day : a->minute < b->minute;
        });
        std::vector<int> runOf(order.size(), 0);
        int run = 0;
        for (size_t i = 1; i < order.size(); ++i) {
            const Group* a = order[i - 1];
            const Group* b = order[i];
            const bool seq = a->number >= 0 && b->number >= 0 && a->prefix == b->prefix;
            const bool numberBreak = seq ? (b->number - a->number > 20) : (a->number >= 0) != (b->number >= 0);
            const long minutesA = a->day * 1440L + std::max(0, a->minute);
            const long minutesB = b->day * 1440L + std::max(0, b->minute);
            const bool timeBreak = std::labs(minutesB - minutesA) > 36 * 60;
            if (numberBreak || timeBreak) {
                ++run;
            }
            runOf[i] = run;
        }

        // Which groups are out of place, honouring the straddle rule: a run
        // that is mostly inside the folder keeps its stragglers.
        std::map<int, std::pair<int, int>> runCounts;   // run -> (inside, outside)
        std::vector<bool> outside(order.size(), false);
        for (size_t i = 0; i < order.size(); ++i) {
            const bool in = isMonth && coveredWithSlack(src.info, order[i]->day, order[i]->minute);
            outside[i] = !in;
            auto& c = runCounts[runOf[i]];
            (in ? c.first : c.second)++;
        }

        std::map<int, std::set<std::string>> runDests;
        std::vector<Choice> choice(order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            if (!outside[i]) {
                continue;
            }
            const auto& c = runCounts[runOf[i]];
            if (isMonth && c.first >= c.second) {
                outside[i] = false;
                continue;
            }
            const Group* g = order[i];
            std::vector<const FolderScan*> cands;
            for (const FolderScan* t : targets) {
                if (!samePath(t->path, src.path) && t->info.covers(g->day)) {
                    cands.push_back(t);
                }
            }
            const std::string srcDrive = driveOf(src.path);
            const bool anySameDrive = std::any_of(cands.begin(), cands.end(),
                [&](const FolderScan* t) { return driveOf(t->path) == srcDrive; });
            if (anySameDrive) {
                cands.erase(std::remove_if(cands.begin(), cands.end(),
                    [&](const FolderScan* t) { return driveOf(t->path) != srcDrive; }), cands.end());
            }
            if (cands.empty()) {
                int y, m, d;
                dayToYMD(g->day, y, m, d);
                choice[i].create = true;
                const std::string home = isMonth ? src.parent : src.path;
                choice[i].createPath = Glib::build_filename(home, monthFolderName(y, m));
                runDests[runOf[i]].insert(choice[i].createPath);
                continue;
            }
            const std::string sibParent = isMonth ? src.parent : src.path;
            auto nearby = [&](const FolderScan* t) {
                int n = 0;
                for (const auto& tg : t->groups) {
                    n += tg.day >= 0 && std::abs(tg.day - g->day) <= 3;
                }
                return n;
            };
            std::stable_sort(cands.begin(), cands.end(), [&](const FolderScan* a, const FolderScan* b) {
                const bool sa = samePath(a->parent, sibParent), sb = samePath(b->parent, sibParent);
                if (sa != sb) return sa;
                const bool ra = samePath(a->root, src.root), rb = samePath(b->root, src.root);
                if (ra != rb) return ra;
                if (a->info.width() != b->info.width()) return a->info.width() < b->info.width();
                return nearby(a) > nearby(b);
            });
            choice[i].dest = cands.front();
            choice[i].alternative = cands.size() > 1;
            runDests[runOf[i]].insert(cands.front()->path);
        }

        for (size_t i = 0; i < order.size(); ++i) {
            if (!outside[i]) {
                continue;
            }
            const Group* g = order[i];
            const std::string destPath = choice[i].create ? choice[i].createPath : choice[i].dest->path;
            // Already there (same name, same size): its own row, never moved.
            bool dup = false;
            if (choice[i].dest && g->primarySize >= 0) {
                const std::string stem = lowerAscii(g->stem);
                for (const auto& tg : choice[i].dest->groups) {
                    if (tg.primarySize == g->primarySize && lowerAscii(tg.stem) == stem) {
                        dup = true;
                        break;
                    }
                }
            }
            const auto key = std::make_pair(src.path, destPath + (dup ? std::string("\x01") : std::string()));
            auto& row = rows[key];
            if (row.groups.empty()) {
                row.duplicate = dup;
                row.source = src.path;
                row.dest = destPath;
                row.createDest = choice[i].create;
                row.firstDay = g->day;
                row.lastDay = g->day;
                row.crossDrive = driveOf(src.path) != driveOf(destPath);
                rowRunsAgree[key] = true;
            }
            row.groups.push_back(*g);
            row.bytes += g->bytes;
            row.firstDay = std::min(row.firstDay, g->day);
            row.lastDay = std::max(row.lastDay, g->day);
            row.alternative = row.alternative || choice[i].alternative;
            rowGroups[key].push_back(g);
            if (runDests[runOf[i]].size() > 1) {
                rowRunsAgree[key] = false;
            }
            if (isMonth && nearEdge(src.info, g->day)) {
                rowNearEdge[key] = true;
            }
        }
    }

    std::vector<Suggestion> out;
    for (auto& kv : rows) {
        Suggestion& row = kv.second;
        const auto& groups = rowGroups[kv.first];
        double base = 0.0;
        int nExif = 0, nName = 0, nRun = 0;
        for (const Group* g : groups) {
            switch (g->source) {
                case DateSource::Exif: base += 0.70; ++nExif; break;
                case DateSource::FileName: base += 0.50; ++nName; break;
                default: base += 0.45; ++nRun; break;
            }
        }
        base /= std::max<size_t>(1, groups.size());

        const FolderScan* src = nullptr;
        const FolderScan* dst = nullptr;
        for (const auto& f : folders) {
            if (samePath(f.path, row.source)) src = &f;
            if (samePath(f.path, row.dest)) dst = &f;
        }
        if (row.duplicate) {
            // Shown so the user knows why these stay; confidence is moot.
            row.score = 1.0;
            row.reason = Reason::Date;
            out.push_back(std::move(row));
            continue;
        }

        double score = base;
        bool continuity = false;
        if (dst) {
            for (const Group* g : groups) {
                if (g->number < 0) continue;
                for (const auto& tg : dst->groups) {
                    if (tg.prefix == g->prefix && tg.number >= 0 && std::labs(tg.number - g->number) <= 50) {
                        continuity = true;
                        break;
                    }
                }
                if (continuity) break;
            }
        }
        if (rowRunsAgree[kv.first]) score += 0.15;
        if (continuity) score += 0.15;
        if (src && samePath(parentOf(row.dest), src->info.kind == FolderKind::Root ? src->path : src->parent)) score += 0.10;
        if (groups.size() >= 10) score += 0.05;
        if (src && src->info.kind == FolderKind::Month && src->info.width() > 31) score += 0.05;
        if (row.alternative) score -= 0.20;
        if (row.crossDrive) score -= 0.15;
        if (rowNearEdge[kv.first]) score -= 0.20;
        if (groups.size() == 1) score -= 0.10;
        if (row.crossDrive) score = std::min(score, 0.84);
        if (nName > nExif && nName >= nRun) score = std::min(score, 0.84);

        if (row.createDest) {
            if (groups.size() < 20) {
                continue;
            }
            score = std::min(score, 0.60);
        }
        score = std::max(0.0, std::min(1.0, score));
        if (score < 0.45) {
            continue;
        }
        row.score = score;
        row.reason = nName > nExif && nName >= nRun ? Reason::FileName
                   : (nRun > nExif || continuity) ? Reason::Sequence : Reason::Date;
        out.push_back(std::move(row));
    }

    std::sort(out.begin(), out.end(), [](const Suggestion& a, const Suggestion& b) {
        if (!samePath(a.source, b.source)) return lowerAscii(a.source) < lowerAscii(b.source);
        return a.groups.size() > b.groups.size();
    });
    return out;
}

}
