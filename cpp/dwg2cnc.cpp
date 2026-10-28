// dwg2cnc - Konversi gambar DWG/DXF menjadi G-code (.TXT) untuk kontroler
// FLSK F2100B (mesin potong CNC portable plasma/flame, mis. MiniCut).
// Versi C++ dari dwg2cnc.py: algoritma, pengaturan, dan output G-code sama.
//
// Cara pakai cepat:
//     seret file .dwg/.dxf (boleh banyak, atau satu folder) ke dwg2cnc.exe
//     dwg2cnc.exe bagian1.dwg bagian2.dwg
//     dwg2cnc.exe folder_gambar\ --holes skip --mode G90
//
// Hasil per gambar (di folder yang sama dengan gambar):
//     NAMA.TXT          -> program G-code untuk flashdisk
//     NAMA_preview.png  -> gambar jalur potong hasil simulasi G-code
//
// Pengaturan dibaca dari dwg2cnc.ini di samping exe (kalau ada).
//
// Dua exe dari sumber ini (lihat build.bat):
//     dwg2cnc.exe      -> command line / seret ke ikon, .TXT langsung ditulis
//     dwg2cnc-gui.exe  -> jendela: seret file, daftar hasil, animasi potong 2D/3D, tombol Simpan .TXT
//
// DWG dibaca dengan dwg2dxf.exe (LibreDWG) di proses terpisah untuk SETIAP file.
// LibreDWG menyimpan state global: kalau dipakai berulang di satu proses (seperti versi
// web/WASM) satu file bermasalah bisa membuat file berikutnya ikut gagal ("null function").
// dwg2dxf.exe + libredwg-0.dll ditanam di exe ini dan diekstrak ke %TEMP% saat dibutuhkan;
// folder libredwg\ di samping exe (kalau ada) dipakai lebih dulu.

#define NOMINMAX
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <GL/gl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using std::string;
using std::vector;
using std::wstring;

static const double PI = 3.14159265358979323846, TAU = 2 * PI, DEG = PI / 180;
static wstring g_exeDir;

// --------------------------------------------------------------------------
// Utilitas
// --------------------------------------------------------------------------
static string u8(const wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static wstring wide(const string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static string fmt(const char* f, ...) {
    va_list a;
    va_start(a, f);
    int n = vsnprintf(nullptr, 0, f, a);
    va_end(a);
    string s(n, 0);
    va_start(a, f);
    vsnprintf(&s[0], n + 1, f, a);
    va_end(a);
    return s;
}

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}

static string lower(string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static string upper(string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

static bool readFile(const wstring& p, string& out) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static bool isDir(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static long long fileSize(const wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static wstring absPath(const wstring& p) {
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(p.c_str(), 32768, buf, nullptr);
    return n ? wstring(buf, n) : p;
}

static wstring dirName(const wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    if (i == wstring::npos) return L"";
    return (i == 2 && p[1] == L':') ? p.substr(0, 3) : p.substr(0, i);
}

static wstring baseName(const wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? p : p.substr(i + 1);
}

static wstring joinPath(const wstring& d, const wstring& f) {
    if (d.empty()) return f;
    wchar_t last = d.back();
    return (last == L'\\' || last == L'/') ? d + f : d + L"\\" + f;
}

static wstring stem(const wstring& p) {  // os.path.splitext(os.path.basename(p))[0]
    wstring b = baseName(p);
    size_t i = b.find_last_of(L'.');
    return (i == wstring::npos || i == 0) ? b : b.substr(0, i);
}

static wstring lowerExt(const wstring& p) {
    wstring b = baseName(p);
    size_t i = b.find_last_of(L'.');
    wstring e = i == wstring::npos ? L"" : b.substr(i);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

// round(v, d) Python: desimal dibulatkan benar (printf UCRT), lalu dibaca lagi
static double rnd(double v, int d) {
    char b[512];
    snprintf(b, sizeof b, "%.*f", d, v);
    return strtod(b, nullptr);
}

static double pmod(double a, double n) {  // a % n Python (hasil selalu bertanda sama dengan n)
    double r = std::fmod(a, n);
    return (r != 0 && r < 0) ? r + n : r;
}

static bool isclose(double a, double b) {  // math.isclose default
    return std::fabs(a - b) <= 1e-9 * std::max(std::fabs(a), std::fabs(b));
}

// --------------------------------------------------------------------------
// Pengaturan
// --------------------------------------------------------------------------
using Cfg = std::map<string, string>;

static const vector<std::pair<string, string>> DEFAULTS = {
    {"mode", "G91"},                  // G91 = inkremental (seperti FastCAM), G90 = absolut
    {"kerf", "G41"},                  // G41 = pakai kompensasi kerf mesin, NONE = tanpa
    {"holes", "cut"},                 // cut = lubang ikut dipotong, skip = hanya kontur luar
    {"min_hole_diameter", "0"},       // lubang lebih kecil dari ini (mm) dilewati
    {"lead_in_outer", "8"},           // panjang lead-in kontur luar (mm)
    {"lead_in_hole", "6"},            // lead-in lubang non-lingkaran (mm)
    {"center_pierce_max_radius", "30"},  // lubang bulat r <= ini: tembus di titik tengah
    {"lead_out", "5"},                // panjang lead-out (mm), 0 = tanpa
    {"start_point", "corner"},        // edge = tengah sisi terpanjang, corner = di sudut part
    {"offset_x", "15"},               // jarak part dari titik nol torch (mm)
    {"offset_y", "15"},
    {"return_home", "yes"},           // kembali ke titik nol di akhir program
    {"decimals", "3"},
    {"ext", ".TXT"},
    {"header_comment", "yes"},        // baris komentar (NAMA ...) di awal file
    {"ignore_layers", "TEXT,DEFPOINTS,SCRIBE,DIM*,*DIMENSION*,MARK*,TITLE*"},
    {"cut_layers", ""},               // kosong = semua layer selain ignore_layers
    {"join_tolerance", "0.05"},       // toleransi sambung ujung garis (mm)
    {"cut_speed", "360"},             // mm/menit, hanya untuk estimasi waktu
    {"preview", "yes"},
    {"converter", "auto"},            // diterima demi kompatibilitas .ini; exe selalu pakai LibreDWG
};

static void loadIni(const wstring& path, Cfg& cfg) {
    string text;
    if (!readFile(path, text)) return;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    string section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == string::npos) e = text.size();
        string t = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') { section = lower(trim(t.substr(1, t.find(']') - 1))); continue; }
        size_t eq = t.find_first_of("=:");
        if (section != "settings" || eq == string::npos) continue;
        string val = trim(t.substr(eq + 1));
        // komentar di belakang nilai (configparser: ; atau # yang didahului spasi)
        for (size_t i = 0; i < val.size(); i++)
            if ((val[i] == ';' || val[i] == '#') && (i == 0 || isspace((unsigned char)val[i - 1]))) { val.resize(i); break; }
        cfg[lower(trim(t.substr(0, eq)))] = trim(val);
    }
}

static bool yes(const string& v) {
    string s = lower(trim(v));
    return s == "yes" || s == "y" || s == "true" || s == "1" || s == "ya";
}

static double num(const Cfg& cfg, const string& k) {
    const string& v = cfg.at(k);
    char* end;
    double d = strtod(v.c_str(), &end);
    if (end == v.c_str() || !trim(end).empty()) throw std::runtime_error("pengaturan " + k + " = '" + v + "' bukan angka");
    return d;
}

// --------------------------------------------------------------------------
// Geometri dasar
// --------------------------------------------------------------------------
struct P { double x = 0, y = 0; };
static double dist(P a, P b) { return std::hypot(a.x - b.x, a.y - b.y); }

struct Seg {
    char kind = 'L';  // 'L' garis, 'A' busur
    P p0, p1, c;      // c = pusat busur
    bool ccw = true;  // arah busur

    double r() const { return dist(c, p0); }
    Seg rev() const { return {kind, p1, p0, c, !ccw}; }

    double sweep() const {  // sudut sapuan busur (radian, + CCW, - CW)
        double a0 = std::atan2(p0.y - c.y, p0.x - c.x), a1 = std::atan2(p1.y - c.y, p1.x - c.x);
        if (ccw) { double s = pmod(a1 - a0, TAU); return s > 1e-12 ? s : TAU; }
        double s = pmod(a0 - a1, TAU);
        return -(s > 1e-12 ? s : TAU);
    }

    double length() const { return kind == 'L' ? dist(p0, p1) : std::fabs(sweep()) * r(); }

    void points(vector<P>& out, double stepDeg = 5.0) const {  // titik pendekatan (tanpa titik awal)
        if (kind == 'L') { out.push_back(p1); return; }
        double sw = sweep();
        int n = std::max(2, (int)(std::fabs(sw * (180.0 / PI)) / stepDeg) + 1);
        double a0 = std::atan2(p0.y - c.y, p0.x - c.x), rr = r();
        for (int i = 1; i < n; i++) out.push_back({c.x + rr * std::cos(a0 + sw * i / n), c.y + rr * std::sin(a0 + sw * i / n)});
        out.push_back(p1);
    }

    P midpoint() const {
        if (kind == 'L') return {(p0.x + p1.x) / 2, (p0.y + p1.y) / 2};
        double a = std::atan2(p0.y - c.y, p0.x - c.x) + sweep() / 2, rr = r();
        return {c.x + rr * std::cos(a), c.y + rr * std::sin(a)};
    }

    P tangentAtMid() const {
        P d;
        if (kind == 'L') d = {p1.x - p0.x, p1.y - p0.y};
        else {
            P m = midpoint(), v{m.x - c.x, m.y - c.y};
            d = ccw ? P{-v.y, v.x} : P{v.y, -v.x};
        }
        double n = std::hypot(d.x, d.y);
        if (n == 0) n = 1;
        return {d.x / n, d.y / n};
    }

    Seg translated(double dx, double dy) const {
        return {kind, {p0.x + dx, p0.y + dy}, {p1.x + dx, p1.y + dy}, {c.x + dx, c.y + dy}, ccw};
    }
};

struct Box { double x0, y0, x1, y1; };

struct Loop {
    vector<Seg> segs;
    string layer;
    bool isCircle = false;  // lingkaran penuh: pusat cc, jari-jari cr
    P cc;
    double cr = 0;
    int depth = 0;
    vector<int> children;

    vector<P> polygon() const {
        vector<P> p{segs[0].p0};
        for (auto& s : segs) s.points(p);
        return p;
    }

    double area() const {  // bertanda: + CCW, - CW
        vector<P> p = polygon();
        double a = 0;
        for (size_t i = 0; i + 1 < p.size(); i++) a += p[i].x * p[i + 1].y - p[i + 1].x * p[i].y;
        return 0.5 * a;
    }

    Box bbox() const {
        vector<P> p = polygon();
        Box b{p[0].x, p[0].y, p[0].x, p[0].y};
        for (auto& q : p) {
            b.x0 = std::min(b.x0, q.x); b.y0 = std::min(b.y0, q.y);
            b.x1 = std::max(b.x1, q.x); b.y1 = std::max(b.y1, q.y);
        }
        return b;
    }

    Loop reversed() const {
        Loop l = *this;
        l.segs.clear();
        for (auto it = segs.rbegin(); it != segs.rend(); ++it) l.segs.push_back(it->rev());
        return l;
    }
};

static bool pointInPoly(P pt, const vector<P>& poly) {
    bool inside = false;
    for (size_t i = 0; i + 1 < poly.size(); i++) {
        P a = poly[i], b = poly[i + 1];
        if ((a.y > pt.y) != (b.y > pt.y) && a.x + (pt.y - a.y) * (b.x - a.x) / (b.y - a.y) > pt.x) inside = !inside;
    }
    return inside;
}

// --------------------------------------------------------------------------
// Baca DXF (DWG diubah dulu ke DXF oleh LibreDWG)
// --------------------------------------------------------------------------
struct T2 { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };  // transformasi affine 2D

static P xf(const T2& T, P p) { return {T.a * p.x + T.c * p.y + T.e, T.b * p.x + T.d * p.y + T.f}; }

static T2 compose(const T2& T, const T2& U) {  // T setelah U
    return {T.a * U.a + T.c * U.b, T.b * U.a + T.d * U.b, T.a * U.c + T.c * U.d, T.b * U.c + T.d * U.d,
            T.a * U.e + T.c * U.f + T.e, T.b * U.e + T.d * U.f + T.f};
}

static T2 insertT(P pt, double sx, double sy, double rot, P base) {
    double cs = std::cos(rot), sn = std::sin(rot);
    return {cs * sx, sn * sx, -sn * sy, cs * sy, pt.x - (cs * sx * base.x - sn * sy * base.y), pt.y - (sn * sx * base.x + cs * sy * base.y)};
}

// OCS -> WCS: extrusion (0,0,-1) berarti X dicerminkan
struct Xf {
    T2 T;
    bool flip;
    P operator()(P p) const { if (flip) p.x = -p.x; return xf(T, p); }
    double det() const { return (T.a * T.d - T.b * T.c) * (flip ? -1 : 1); }
    double scale() const { return std::sqrt(std::fabs(T.a * T.d - T.b * T.c)); }
    bool uniform() const {  // lingkaran tetap lingkaran
        double sx = std::hypot(T.a, T.b), sy = std::hypot(T.c, T.d), tol = 1e-9 * std::max(1.0, std::max(sx, sy));
        return std::fabs(sx - sy) <= tol && std::fabs(T.a * T.c + T.b * T.d) <= tol * std::max(1.0, sx * sy);
    }
};

// Perataan kurva adaptif seperti ezdxf: tiap interval dibagi `segments`, lalu dipecah
// lagi selama titik tengahnya lebih dari 0.05 mm dari tali busur.
static double distPointLine(P m, P s, P e) {
    double ex = e.x - s.x, ey = e.y - s.y, L2 = ex * ex + ey * ey;
    if (L2 <= 1e-24) return 0;
    double vx = m.x - s.x, vy = m.y - s.y, dot = vx * ex + vy * ey;
    double diff = vx * vx + vy * vy - dot * dot / L2;
    return diff <= 0 ? 0 : std::sqrt(diff);
}

static void subdiv(const std::function<P(double)>& f, P s, P e, double t0, double t1, vector<P>& out, int depth) {
    double tm = (t0 + t1) * 0.5;
    P m = f(tm);
    if (depth > 20 || distPointLine(m, s, e) < 0.05) { out.push_back(e); return; }
    subdiv(f, s, m, t0, tm, out, depth + 1);
    subdiv(f, m, e, tm, t1, out, depth + 1);
}

static vector<P> flatten(const std::function<P(double)>& f, const vector<double>& breaks, int segments) {
    double t = breaks[0];
    P s = f(t);
    vector<P> out{s};
    for (size_t i = 1; i < breaks.size(); i++) {
        double t1 = breaks[i], delta = (t1 - t) / segments;
        if (!(delta > 0)) continue;
        while (t < t1) {
            double nt = t + delta;
            if (std::fabs(nt - t1) <= 1e-8 + 1e-5 * std::fabs(t1) || nt <= t) nt = t1;  // np.isclose
            P e = f(nt);
            subdiv(f, s, e, t, nt, out, 0);
            t = nt;
            s = e;
        }
    }
    return out;
}

static double arcSpan(double s, double e) {  // ezdxf arc_angle_span_rad
    if (std::fabs(s - e) <= std::max(1e-9 * std::max(std::fabs(s), std::fabs(e)), 1e-15)) return 0;
    s = pmod(s, TAU);
    if (isclose(s, pmod(e, TAU))) return TAU;
    e = pmod(e, TAU);
    if (e < s) e += TAU;
    return e - s;
}

static P bulgeCenter(P a, P b, double bulge) {  // ezdxf bulge_center
    double r = dist(a, b) * (1 + bulge * bulge) / 4 / bulge;
    double ang = std::atan2(b.y - a.y, b.x - a.x) + (PI / 2 - std::atan(bulge) * 2);
    return {a.x + r * std::cos(ang), a.y + r * std::sin(ang)};
}

struct Prim {  // satu entitas gambar (LINE/ARC/CIRCLE/polyline/...) dalam WCS
    string layer;
    vector<Seg> segs;
    bool circ = false;
    P cc;
    double cr = 0;
};

static void linePts(Prim& pr, const vector<P>& pts) {
    for (size_t i = 0; i + 1 < pts.size(); i++)
        if (dist(pts[i], pts[i + 1]) > 1e-6) pr.segs.push_back({'L', pts[i], pts[i + 1]});
}

static void arcPrim(Prim& pr, const Xf& X, P c, double r, bool isArc, double a0, double a1) {
    auto at = [&](double a) { return P{c.x + r * std::cos(a), c.y + r * std::sin(a)}; };
    if (!X.uniform()) {  // block dengan skala X != Y: lingkaran jadi elips, diratakan
        double e = isArc ? a1 : a0 + TAU;
        if (e <= a0) e += TAU;
        linePts(pr, flatten([&](double t) { return X(at(t)); }, {a0, e}, 8));
        return;
    }
    P cc = X(c);
    double rr = r * X.scale();
    bool ccw = X.det() > 0;
    if (!isArc) {
        P p0{cc.x + rr, cc.y}, p1{cc.x - rr, cc.y};
        pr.segs = {{'A', p0, p1, cc, true}, {'A', p1, p0, cc, true}};
        pr.circ = true, pr.cc = cc, pr.cr = rr;
        return;
    }
    P a = X(at(a0)), b = X(at(a1));
    if (dist(a, b) < 1e-6) {  // busur 360 derajat
        P q{2 * cc.x - a.x, 2 * cc.y - a.y};
        pr.segs = {{'A', a, q, cc, ccw}, {'A', q, a, cc, ccw}};
        pr.circ = true, pr.cc = cc, pr.cr = rr;
        return;
    }
    pr.segs.push_back({'A', a, b, cc, ccw});
}

static void polyPrim(Prim& pr, const Xf& X, const vector<std::array<double, 3>>& v, bool closed) {
    size_t n = v.size();
    if (n < 2) return;
    bool ccw = X.det() > 0;
    for (size_t i = 0; i < (closed ? n : n - 1); i++) {
        P a{v[i][0], v[i][1]}, b{v[(i + 1) % n][0], v[(i + 1) % n][1]};
        double bulge = v[i][2];
        P wa = X(a), wb = X(b);
        if (dist(wa, wb) <= 1e-6) continue;
        if (std::fabs(bulge) < 1e-9) { pr.segs.push_back({'L', wa, wb}); continue; }
        P c = bulgeCenter(a, b, bulge);
        if (!X.uniform()) {
            double r = dist(c, a), s = std::atan2(a.y - c.y, a.x - c.x), sw = 4 * std::atan(bulge);
            linePts(pr, flatten([&](double u) { return X({c.x + r * std::cos(s + u * sw), c.y + r * std::sin(s + u * sw)}); }, {0, 1}, 8));
            continue;
        }
        // seperti ezdxf: busur selalu CCW di OCS, bulge negatif = dari titik akhir ke titik awal
        pr.segs.push_back(bulge > 0 ? Seg{'A', wa, wb, X(c), ccw} : Seg{'A', wb, wa, X(c), ccw});
    }
}

struct Grp { int code; string val; };
using Grps = vector<Grp>;
struct Ent { string type; Grps g; vector<Grps> verts; };
struct Block { P base; vector<Ent> ents; };

static double gnum(const Grps& g, int code, double def = 0) {
    for (auto& p : g) if (p.code == code) return strtod(p.val.c_str(), nullptr);
    return def;
}

static string gstr(const Grps& g, int code, const string& def = "") {
    for (auto& p : g) if (p.code == code) return p.val;
    return def;
}

static string mtextPlain(const Grps& g) {  // buang kode format MTEXT (\P = baris baru)
    string raw, out;
    for (auto& p : g) if (p.code == 3) raw += p.val;
    raw += gstr(g, 1);
    for (size_t i = 0; i < raw.size(); i++) {
        char ch = raw[i];
        if (ch == '{' || ch == '}') continue;
        if (ch != '\\' || i + 1 >= raw.size()) { out += ch; continue; }
        char k = raw[++i];
        if (k == 'P' || k == 'N') out += '\n';
        else if (k == '~') out += ' ';
        else if (k == '\\' || k == '{' || k == '}') out += k;
        else if (k == 'L' || k == 'l' || k == 'O' || k == 'o' || k == 'K' || k == 'k') {}
        else { size_t e = raw.find(';', i); if (e == string::npos) break; i = e; }
    }
    return out;
}

struct DxfReader {
    std::map<string, Block> blocks;
    vector<Prim> prims;
    vector<string> texts;

    void parse(string text) {
        if (text.compare(0, 18, "AutoCAD Binary DXF") == 0) throw std::runtime_error("DXF biner tidak didukung. Simpan ulang sebagai DXF ASCII.");
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
        vector<Grp> pairs;
        size_t pos = 0;
        auto next = [&](string& line) {
            if (pos >= text.size()) return false;
            size_t e = text.find('\n', pos);
            if (e == string::npos) e = text.size();
            line = text.substr(pos, e - pos);
            pos = e + 1;
            return true;
        };
        string a, b;
        while (next(a) && next(b)) pairs.push_back({atoi(a.c_str()), trim(b)});

        size_t i = 0, n = pairs.size();
        string section;
        bool any = false;
        vector<Ent> ents;
        Block* cur = nullptr;
        auto readEntity = [&]() {
            Ent e;
            e.type = pairs[i++].val;
            while (i < n && pairs[i].code != 0) e.g.push_back(pairs[i++]);
            return e;
        };
        while (i < n) {
            const Grp& p = pairs[i];
            if (p.code == 0 && p.val == "SECTION") { section = i + 1 < n ? pairs[i + 1].val : ""; any = true; i += 2; continue; }
            if (p.code == 0 && p.val == "ENDSEC") { section.clear(); i++; continue; }
            if (p.code == 0 && p.val == "EOF") break;
            if (p.code == 0 && (section == "ENTITIES" || section == "BLOCKS")) {
                Ent e = readEntity();
                if (e.type == "BLOCK") {
                    Block& bl = blocks[upper(gstr(e.g, 2))];
                    bl = {{gnum(e.g, 10), gnum(e.g, 20)}, {}};
                    cur = &bl;
                    continue;
                }
                if (e.type == "ENDBLK") { cur = nullptr; continue; }
                if (e.type == "POLYLINE") {
                    while (i < n && pairs[i].code == 0 && pairs[i].val == "VERTEX") e.verts.push_back(readEntity().g);
                    if (i < n && pairs[i].code == 0 && pairs[i].val == "SEQEND") readEntity();
                }
                if (section == "BLOCKS") { if (cur) cur->ents.push_back(std::move(e)); }
                else ents.push_back(std::move(e));
                continue;
            }
            i++;
        }
        if (!any) throw std::runtime_error("bukan file DXF yang valid");
        walk(ents, T2{}, "", 0);
    }

    // Pecah entitas menjadi segmen, termasuk isi block (INSERT)
    void walk(const vector<Ent>& list, const T2& T, const string& parentLayer, int depth) {
        for (auto& e : list) {
            const Grps& g = e.g;
            const string& t = e.type;
            if (depth == 0 && gnum(g, 67) == 1) continue;  // paper space
            string layer = gstr(g, 8, "0");
            if (!parentLayer.empty() && layer == "0") layer = parentLayer;
            bool flip = gnum(g, 230, 1) < 0;
            Xf X{T, flip}, W{T, false};
            Prim pr;
            pr.layer = layer;
            if (t == "LINE") {
                P a = W({gnum(g, 10), gnum(g, 20)}), b = W({gnum(g, 11), gnum(g, 21)});
                if (dist(a, b) > 1e-6) pr.segs.push_back({'L', a, b});
            } else if (t == "CIRCLE" || t == "ARC") {
                arcPrim(pr, X, {gnum(g, 10), gnum(g, 20)}, gnum(g, 40), t == "ARC", gnum(g, 50) * DEG, gnum(g, 51) * DEG);
            } else if (t == "LWPOLYLINE") {
                vector<std::array<double, 3>> v;
                for (auto& p : g) {
                    double d = atof(p.val.c_str());
                    if (p.code == 10) v.push_back({d, 0, 0});
                    else if (p.code == 20 && !v.empty()) v.back()[1] = d;
                    else if (p.code == 42 && !v.empty()) v.back()[2] = d;
                }
                polyPrim(pr, X, v, (int)gnum(g, 70) & 1);
            } else if (t == "POLYLINE") {
                int fl = (int)gnum(g, 70);
                if (fl & (8 | 16 | 64)) continue;  // 3D polyline / mesh: bukan 2D
                vector<std::array<double, 3>> v;
                for (auto& vg : e.verts)
                    if (!((int)gnum(vg, 70) & 16)) v.push_back({gnum(vg, 10), gnum(vg, 20), gnum(vg, 42)});
                polyPrim(pr, X, v, fl & 1);
            } else if (t == "ELLIPSE") {
                P c{gnum(g, 10), gnum(g, 20)}, mj{gnum(g, 11), gnum(g, 21)};
                double ratio = gnum(g, 40, 1), s = gnum(g, 41, 0), en = gnum(g, 42, TAU), z = flip ? -1 : 1;
                P mn{-z * mj.y * ratio, z * mj.x * ratio};  // sumbu minor = extrusion x sumbu mayor
                double p0 = pmod(s, TAU), p1 = isclose(en, TAU) ? TAU : pmod(en, TAU);
                if (arcSpan(s, en) != 0 && !isclose(p0, p1)) {
                    if (p0 > p1) p1 += TAU;
                    linePts(pr, flatten([&](double u) {
                        return W({c.x + std::cos(u) * mj.x + std::sin(u) * mn.x, c.y + std::cos(u) * mj.y + std::sin(u) * mn.y});
                    }, {p0, p1}, 8));
                }
            } else if (t == "SPLINE") {
                splinePrim(pr, W, g);
            } else if (t == "TEXT" || t == "MTEXT") {
                if (depth == 0) {
                    string s = trim(t == "TEXT" ? gstr(g, 1) : mtextPlain(g));
                    if (!s.empty()) texts.push_back(s);
                }
                continue;
            } else if (t == "INSERT") {
                auto it = blocks.find(upper(gstr(g, 2)));
                if (depth >= 8 || it == blocks.end()) continue;
                T2 U = insertT({gnum(g, 10), gnum(g, 20)}, gnum(g, 41, 1), gnum(g, 42, 1), gnum(g, 50) * DEG, it->second.base);
                if (flip) U = compose(T2{-1, 0, 0, 1, 0, 0}, U);
                walk(it->second.ents, compose(T, U), layer, depth + 1);
                continue;
            } else {
                continue;
            }
            prims.push_back(std::move(pr));
        }
    }

    static void splinePrim(Prim& pr, const Xf& W, const Grps& g) {
        int deg = (int)gnum(g, 71, 3);
        vector<double> kn, wt, cx, cy, fx, fy;
        for (auto& p : g) {
            double v = atof(p.val.c_str());
            switch (p.code) {
                case 40: kn.push_back(v); break;
                case 41: wt.push_back(v); break;
                case 10: cx.push_back(v); break;
                case 20: cy.push_back(v); break;
                case 11: fx.push_back(v); break;
                case 21: fy.push_back(v); break;
            }
        }
        int n = (int)std::min(cx.size(), cy.size());
        if (deg >= 1 && n > deg) {
            if ((int)kn.size() != n + deg + 1) {  // knot seragam terbuka
                kn.clear();
                for (int i = 0; i < n + deg + 1; i++) kn.push_back(i <= deg ? 0 : i >= n ? 1 : double(i - deg) / (n - deg));
            }
            if ((int)wt.size() != n) wt.assign(n, 1.0);
            auto f = [&](double t) {  // de Boor, NURBS rasional
                int k = deg;
                while (k < n - 1 && t >= kn[k + 1]) k++;
                vector<std::array<double, 3>> d(deg + 1);
                for (int j = 0; j <= deg; j++) { int q = k - deg + j; d[j] = {cx[q] * wt[q], cy[q] * wt[q], wt[q]}; }
                for (int r = 1; r <= deg; r++)
                    for (int j = deg; j >= r; j--) {
                        int i = k - deg + j;
                        double den = kn[i + deg - r + 1] - kn[i], a = den ? (t - kn[i]) / den : 0;
                        for (int c = 0; c < 3; c++) d[j][c] = (1 - a) * d[j - 1][c] + a * d[j][c];
                    }
                return W({d[deg][0] / d[deg][2], d[deg][1] / d[deg][2]});
            };
            vector<double> br;
            for (double k : kn)
                if (k >= kn[deg] && k <= kn[n] && (br.empty() || k > br.back())) br.push_back(k);
            if (br.size() >= 2) linePts(pr, flatten(f, br, 4));
        } else if (std::min(fx.size(), fy.size()) >= 2) {
            // ponytail: spline tanpa titik kontrol disambung lurus antar fit point (ezdxf menginterpolasi)
            vector<P> pts;
            for (size_t i = 0; i < std::min(fx.size(), fy.size()); i++) pts.push_back(W({fx[i], fy[i]}));
            linePts(pr, pts);
        }
    }
};

// Folder sementara per file; dihapus lagi setelah selesai
struct TempDir {
    wstring path;
    TempDir() {
        static int counter = 0;
        wchar_t b[MAX_PATH + 1], s[MAX_PATH + 1];
        wstring base(b, GetTempPathW(MAX_PATH + 1, b));
        if (GetShortPathNameW(base.c_str(), s, MAX_PATH + 1)) base = s;  // dwg2dxf hanya paham path ANSI
        for (;;) {
            path = base + L"dwg2cnc_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++counter) + L"\\";
            if (CreateDirectoryW(path.c_str(), nullptr)) break;
            if (GetLastError() != ERROR_ALREADY_EXISTS) throw std::runtime_error("folder TEMP tidak bisa dipakai");
        }
    }
    ~TempDir() {
        for (auto f : {L"input.dwg", L"input.dxf", L"log.txt"}) DeleteFileW((path + f).c_str());
        RemoveDirectoryW(path.c_str());
    }
};

static bool extractRes(int id, const wstring& path) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    DWORD n = SizeofResource(nullptr, r);
    const void* data = LockResource(LoadResource(nullptr, r));
    if (fileSize(path) == (long long)n) return true;
    wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId());
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    ok = ok && (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) || fileSize(path) == (long long)n);
    DeleteFileW(tmp.c_str());
    return ok;
}

static wstring findLibredwg() {
    for (const wstring& d : {g_exeDir + L"libredwg\\", g_exeDir})
        if (fileSize(d + L"dwg2dxf.exe") > 0) return d + L"dwg2dxf.exe";
    wchar_t b[MAX_PATH + 1];
    wstring d = wstring(b, GetTempPathW(MAX_PATH + 1, b)) + L"dwg2cnc-libredwg\\";
    CreateDirectoryW(d.c_str(), nullptr);
    if (extractRes(2, d + L"libredwg-0.dll") && extractRes(1, d + L"dwg2dxf.exe")) return d + L"dwg2dxf.exe";
    return SearchPathW(nullptr, L"dwg2dxf.exe", nullptr, MAX_PATH + 1, b, nullptr) ? wstring(b) : L"";
}

static string dwgToDxf(const wstring& path) {
    const string hint = "). Simpan ulang gambar sebagai DXF dari AutoCAD (SAVEAS > DXF) lalu ulangi.";
    wstring exe = findLibredwg();
    if (exe.empty()) throw std::runtime_error("Tidak bisa membaca DWG (dwg2dxf (LibreDWG) tidak ditemukan" + hint);
    TempDir tmp;
    wstring in = tmp.path + L"input.dwg", out = tmp.path + L"input.dxf", log = tmp.path + L"log.txt";
    if (!CopyFileW(path.c_str(), in.c_str(), FALSE)) throw std::runtime_error("file tidak ditemukan / tidak bisa dibaca");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE hlog = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = hlog;
    PROCESS_INFORMATION pi{};
    wstring cmd = L"\"" + exe + L"\" -y -o \"" + out + L"\" \"" + in + L"\"";
    string why;
    if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, tmp.path.c_str(), &si, &pi)) {
        // ponytail: batas tetap 2 menit per file (LibreDWG bisa macet di file rusak)
        if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            why = "macet lebih dari 2 menit";
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        why = "dwg2dxf tidak bisa dijalankan";
    }
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    string dxf;
    if (why.empty() && readFile(out, dxf) && !dxf.empty()) {
        if (dxf.find("EOF", dxf.size() > 64 ? dxf.size() - 64 : 0) != string::npos) return dxf;
        why = "output DXF terpotong";  // dwg2dxf crash di tengah jalan
    }
    string lg;
    readFile(log, lg);
    lg = trim(lg);
    if (lg.size() > 300) lg = lg.substr(lg.size() - 300);
    if (!why.empty() && !lg.empty()) why += "; ";
    throw std::runtime_error("Tidak bisa membaca DWG (LibreDWG gagal: " + why + lg + hint);
}

static bool wild(const char* p, const char* s) {  // fnmatch: * dan ?
    if (!*p) return !*s;
    if (*p == '*') return wild(p + 1, s) || (*s && wild(p, s + 1));
    return *s && (*p == '?' || *p == *s) && wild(p + 1, s + 1);
}

static vector<string> patterns(const string& csv) {
    vector<string> out;
    size_t pos = 0;
    while (pos <= csv.size()) {
        size_t e = csv.find(',', pos);
        if (e == string::npos) e = csv.size();
        string p = upper(trim(csv.substr(pos, e - pos)));
        if (!p.empty()) out.push_back(p);
        pos = e + 1;
    }
    return out;
}

static bool layerOk(const string& layer, const Cfg& cfg) {
    string L = upper(layer.empty() ? "0" : layer);
    for (auto& p : patterns(cfg.at("ignore_layers"))) if (wild(p.c_str(), L.c_str())) return false;
    vector<string> only = patterns(cfg.at("cut_layers"));
    if (only.empty()) return true;
    for (auto& p : only) if (wild(p.c_str(), L.c_str())) return true;
    return false;
}

// Sambungkan segmen lepas menjadi loop tertutup
static vector<Loop> chain(const vector<Seg>& segs, double tol, int& openCount) {
    double cell = std::max(tol * 4, 1e-3);
    using Key = std::pair<long long, long long>;
    auto key = [&](P p) { return Key{(long long)std::floor(p.x / cell), (long long)std::floor(p.y / cell)}; };
    std::map<Key, vector<std::pair<int, int>>> grid;
    for (int i = 0; i < (int)segs.size(); i++) {
        grid[key(segs[i].p0)].push_back({i, 0});
        grid[key(segs[i].p1)].push_back({i, 1});
    }
    vector<char> used(segs.size(), 0);
    auto find = [&](P p, int& bi, int& bend) {
        Key k = key(p);
        double bd = 0;
        bi = -1;
        for (long long dx = -1; dx <= 1; dx++)
            for (long long dy = -1; dy <= 1; dy++) {
                auto it = grid.find({k.first + dx, k.second + dy});
                if (it == grid.end()) continue;
                for (auto& [i, end] : it->second) {
                    if (used[i]) continue;
                    double d = dist(p, end == 0 ? segs[i].p0 : segs[i].p1);
                    if (d <= tol && (bi < 0 || d < bd)) bi = i, bend = end, bd = d;
                }
            }
        return bi >= 0;
    };
    vector<Loop> loops;
    for (int i = 0; i < (int)segs.size(); i++) {
        if (used[i]) continue;
        used[i] = 1;
        vector<Seg> path{segs[i]};
        P start = path[0].p0;
        bool closed = false;
        for (;;) {
            P cur = path.back().p1;
            if (dist(cur, start) <= tol && (path.size() > 1 || path[0].kind == 'A')) { closed = true; break; }
            int j, end;
            if (!find(cur, j, end)) break;
            used[j] = 1;
            Seg s = end == 0 ? segs[j] : segs[j].rev();
            path.push_back({s.kind, cur, s.p1, s.c, s.ccw});  // rapatkan titik sambung
        }
        if (closed) {
            path.back().p1 = start;
            Loop lp;
            lp.segs = std::move(path);
            loops.push_back(std::move(lp));
        } else {
            openCount++;
        }
    }
    return loops;
}

static void detectCircle(Loop& lp) {
    if (lp.isCircle) return;
    for (auto& s : lp.segs) if (s.kind != 'A') return;
    P c0 = lp.segs[0].c;
    double r0 = lp.segs[0].r(), tot = 0;
    for (auto& s : lp.segs) {
        if (dist(s.c, c0) > 0.01 || std::fabs(s.r() - r0) > 0.01) return;
        tot += std::fabs(s.sweep());
    }
    if (std::fabs(tot - TAU) < 1e-3) lp.isCircle = true, lp.cc = c0, lp.cr = r0;
}

static vector<Loop> dedupe(const vector<Loop>& loops) {
    std::set<std::array<double, 5>> seen;
    vector<Loop> out;
    for (auto& lp : loops) {
        Box b = lp.bbox();
        std::array<double, 5> k{rnd(b.x0, 2), rnd(b.y0, 2), rnd(b.x1, 2), rnd(b.y1, 2), rnd(std::fabs(lp.area()), 1)};
        if (seen.insert(k).second) out.push_back(lp);
    }
    return out;
}

struct Geom { vector<Loop> loops; int open = 0; vector<string> texts, layers, skipped; };

static Geom readGeometry(const wstring& path, const Cfg& cfg) {
    string text;
    if (lowerExt(path) == L".dwg") text = dwgToDxf(path);
    else if (!readFile(path, text)) throw std::runtime_error("file tidak ditemukan / tidak bisa dibaca");
    DxfReader rd;
    rd.parse(std::move(text));

    double tol = num(cfg, "join_tolerance");
    Geom geo;
    geo.texts = rd.texts;
    vector<string> order;
    std::map<string, vector<Seg>> byLayer;
    std::set<string> used, skipped;
    for (auto& p : rd.prims) {
        if (!layerOk(p.layer, cfg)) { skipped.insert(p.layer); continue; }
        if (p.segs.empty()) continue;
        used.insert(p.layer);
        if (p.circ) {
            Loop lp;
            lp.segs = p.segs, lp.layer = p.layer, lp.isCircle = true, lp.cc = p.cc, lp.cr = p.cr;
            geo.loops.push_back(std::move(lp));
        } else {
            if (!byLayer.count(p.layer)) order.push_back(p.layer);
            auto& v = byLayer[p.layer];
            v.insert(v.end(), p.segs.begin(), p.segs.end());
        }
    }
    // disambung per layer, supaya garis bantu tidak menempel ke kontur
    for (auto& L : order) {
        for (auto& lp : chain(byLayer[L], tol, geo.open)) {
            lp.layer = L;
            detectCircle(lp);
            geo.loops.push_back(std::move(lp));
        }
    }
    geo.loops = dedupe(geo.loops);
    geo.layers.assign(used.begin(), used.end());
    geo.skipped.assign(skipped.begin(), skipped.end());
    return geo;
}

// --------------------------------------------------------------------------
// Susun part, urutan potong, lead-in
// --------------------------------------------------------------------------
static vector<int> buildParts(vector<Loop>& loops) {
    size_t n = loops.size();
    vector<vector<P>> polys(n);
    vector<double> areas(n);
    for (size_t i = 0; i < n; i++) polys[i] = loops[i].polygon(), areas[i] = std::fabs(loops[i].area());
    vector<int> parent(n, -1);
    for (size_t i = 0; i < n; i++) {
        P probe = loops[i].segs[0].midpoint();
        int best = -1;
        for (size_t j = 0; j < n; j++)
            if (i != j && areas[j] > areas[i] && pointInPoly(probe, polys[j]) && (best < 0 || areas[j] < areas[best])) best = (int)j;
        parent[i] = best;
    }
    vector<int> parts;
    for (size_t i = 0; i < n; i++) {
        int d = 0;
        for (int p = parent[i]; p >= 0; p = parent[p]) d++;
        loops[i].depth = d;
    }
    for (size_t i = 0; i < n; i++) {
        if (loops[i].depth % 2) continue;
        for (size_t j = 0; j < n; j++) if (parent[j] == (int)i) loops[i].children.push_back((int)j);
        parts.push_back((int)i);
    }
    return parts;
}

static Loop orient(const Loop& loop, bool wantCcw) { return (loop.area() > 0) == wantCcw ? loop : loop.reversed(); }

struct Cut { P pierce; vector<Seg> segs; bool hasLo = false; P lo; bool hole = false; };

// Mulai potong di sudut (sudut terdekat dari titik nol torch). Lead-in masuk dari sisi scrap
// searah garis bagi sudut, jadi tidak ada takik di tengah sisi part.
// Kontur luar (CW): sudut part = belok kanan. Lubang (CCW): sudut lubang = belok kiri.
static bool cornerStart(const Loop& loop, const vector<P>& poly, const Cfg& cfg, bool isHole, Cut& out) {
    auto tanAt = [](const Seg& s, P p) {
        P d;
        if (s.kind == 'L') d = {s.p1.x - s.p0.x, s.p1.y - s.p0.y};
        else {
            P v{p.x - s.c.x, p.y - s.c.y};
            d = s.ccw ? P{-v.y, v.x} : P{v.y, -v.x};
        }
        double m = std::hypot(d.x, d.y);
        if (m == 0) m = 1;
        return P{d.x / m, d.y / m};
    };
    struct Cand { double d; int k; P ta, tb; };
    int n = (int)loop.segs.size();
    bool hasSharp = false, hasSmooth = false;
    Cand sharp{}, smooth{};
    for (int k = 0; k < n; k++) {
        const Seg& a = loop.segs[(k - 1 + n) % n];  // a berakhir di sudut, b mulai dari sudut
        const Seg& b = loop.segs[k];
        P v = b.p0, ta = tanAt(a, v), tb = tanAt(b, v);
        double d = std::hypot(v.x, v.y);  // jarak ke titik nol torch
        if (ta.x * tb.x + ta.y * tb.y > 0.996) {  // belokan < ~5 derajat: bukan sudut tajam
            // sambungan garis-busur (ujung garis lurus) sebagai cadangan
            if ((a.kind == 'L') != (b.kind == 'L') && (!hasSmooth || d < smooth.d - 1e-6)) smooth = {d, k, ta, tb}, hasSmooth = true;
            continue;
        }
        double cross = ta.x * tb.y - ta.y * tb.x;
        if (isHole ? cross < 1e-6 : cross > -1e-6) continue;  // sudut cekung: lead-in akan menabrak part
        if (!hasSharp || d < sharp.d - 1e-6) sharp = {d, k, ta, tb}, hasSharp = true;
    }
    if (!hasSharp && !hasSmooth) return false;
    const Cand& best = hasSharp ? sharp : smooth;
    P v = loop.segs[best.k].p0;
    P bis{-best.ta.y - best.tb.y, best.ta.x + best.tb.x};  // normal kiri = sisi scrap
    double L0 = std::hypot(bis.x, bis.y);
    if (L0 == 0) L0 = 1;
    bis = {bis.x / L0, bis.y / L0};
    double L = num(cfg, isHole ? "lead_in_hole" : "lead_in_outer");
    bool found = false;
    while (L >= 0.5) {
        P p{v.x + bis.x * L, v.y + bis.y * L};
        if (pointInPoly(p, poly) == isHole) { out.pierce = p, found = true; break; }
        L /= 2;
    }
    if (!found) return false;
    out.segs.assign(loop.segs.begin() + best.k, loop.segs.end());
    out.segs.insert(out.segs.end(), loop.segs.begin(), loop.segs.begin() + best.k);
    double leadOut = num(cfg, "lead_out");
    if (leadOut > 0) out.hasLo = true, out.lo = {v.x + bis.x * std::min(leadOut, L), v.y + bis.y * std::min(leadOut, L)};
    return true;
}

// Kontur luar diputar CW, lubang CCW -> sisi kiri jalur = sisi scrap (cocok dengan G41)
static Cut prepareCut(const Loop& loop0, bool isHole, const Cfg& cfg) {
    Loop loop = orient(loop0, isHole);
    double leadOut = num(cfg, "lead_out");
    Cut cut;
    cut.hole = isHole;

    if (isHole && loop.isCircle) {
        P c = loop.cc;
        double r = loop.cr;
        P start{c.x - r, c.y}, opp{c.x + r, c.y};  // mulai di sisi kiri lingkaran
        cut.segs = {{'A', start, opp, c, true}, {'A', opp, start, c, true}};
        auto toward = [&](double L) { return P{start.x + L, start.y}; };  // dari start menuju pusat
        cut.pierce = r <= num(cfg, "center_pierce_max_radius") ? c : toward(std::min(num(cfg, "lead_in_hole") * 2, r * 0.6));
        if (leadOut > 0) cut.hasLo = true, cut.lo = toward(std::min(leadOut, r * 0.5));
        return cut;
    }

    vector<P> poly = loop.polygon();
    if (lower(cfg.at("start_point")) == "corner" && cornerStart(loop, poly, cfg, isHole, cut)) return cut;

    // titik start = tengah garis terpanjang (kalau tidak ada garis: busur terpanjang)
    int k = 0;
    for (int i = 1; i < (int)loop.segs.size(); i++) {
        const Seg &a = loop.segs[i], &b = loop.segs[k];
        bool aArc = a.kind != 'L', bArc = b.kind != 'L';
        if (aArc < bArc || (aArc == bArc && -a.length() < -b.length())) k = i;
    }
    const Seg& s = loop.segs[k];
    P m = s.midpoint();
    Seg a{s.kind, s.p0, m, s.c, s.ccw}, b{s.kind, m, s.p1, s.c, s.ccw};
    cut.segs = {b};
    cut.segs.insert(cut.segs.end(), loop.segs.begin() + k + 1, loop.segs.end());
    cut.segs.insert(cut.segs.end(), loop.segs.begin(), loop.segs.begin() + k);
    cut.segs.push_back(a);
    P start = b.p0, t = s.tangentAtMid(), nrm{-t.y, t.x};  // normal kiri = sisi scrap
    double L = num(cfg, isHole ? "lead_in_hole" : "lead_in_outer");
    cut.pierce = start;
    bool found = false;
    while (L >= 0.5) {
        P p{start.x + nrm.x * L, start.y + nrm.y * L};
        if (pointInPoly(p, poly) == isHole) { cut.pierce = p, found = true; break; }
        L /= 2;
    }
    if (leadOut > 0 && found) {
        double ll = std::min(leadOut, L);
        cut.hasLo = true, cut.lo = {start.x + nrm.x * ll, start.y + nrm.y * ll};
    }
    return cut;
}

// --------------------------------------------------------------------------
// G-code
// --------------------------------------------------------------------------
struct GWriter {
    string mode;
    int dec;
    vector<string> lines;
    P pos;                 // posisi absolut, sudah dibulatkan (cegah drift G91)
    vector<P> expected;    // target absolut tiap gerakan, untuk verifikasi
    double cutLen = 0, rapidLen = 0;
    int pierces = 0;

    P rp(P p) const { return {rnd(p.x, dec), rnd(p.y, dec)}; }

    string f(double v) const {
        v = rnd(v, dec);
        if (std::fabs(v) < 0.5 * std::pow(10.0, -dec)) v = 0.0;
        string s = fmt("%.*f", dec, v);
        if (s.find('.') != string::npos) {
            while (s.back() == '0') s.pop_back();
            if (s.back() == '.') s += '0';
        }
        return s;
    }

    string xy(P p, P& pr) const {
        pr = rp(p);
        if (mode == "G91") return "X" + f(pr.x - pos.x) + "Y" + f(pr.y - pos.y);
        return "X" + f(pr.x) + "Y" + f(pr.y);
    }

    void raw(const string& s) { lines.push_back(s); }

    void move(const string& code, P p) {
        if (dist(rp(p), pos) < std::pow(10.0, -dec)) return;
        P pr;
        string w = xy(p, pr);
        double L = dist(pos, pr);
        (code == "G00" ? rapidLen : cutLen) += L;
        lines.push_back(code + w);
        pos = pr;
        expected.push_back(pr);
    }

    void arc(const Seg& s) {
        if (s.r() > 1e5) { move("G01", s.p1); return; }
        double i = s.c.x - pos.x, j = s.c.y - pos.y;
        P pr;
        string w = xy(s.p1, pr);
        lines.push_back((s.ccw ? "G03" : "G02") + w + "I" + f(i) + "J" + f(j));
        cutLen += s.length();
        pos = pr;
        expected.push_back(pr);
    }
};

struct Stats {
    wstring name;
    int parts = 0, holes = 0, skippedHoles = 0, pierces = 0, lines = 0, open = 0;
    double cutLen = 0, rapidLen = 0, w = 0, h = 0;
    vector<string> texts, layers, ignored;
};

static GWriter generate(const wstring& path, const Cfg& cfg, Stats& st) {
    Geom geo = readGeometry(path, cfg);
    if (geo.loops.empty()) throw std::runtime_error("Tidak ada kontur tertutup yang bisa dipotong. Cek layer dan pastikan garis tersambung.");
    vector<Loop>& loops = geo.loops;
    vector<int> parts = buildParts(loops);
    string holesMode = lower(cfg.at("holes"));
    double minHole = num(cfg, "min_hole_diameter");
    int skippedHoles = 0;

    vector<Box> allB;
    for (int k : parts) allB.push_back(loops[k].bbox());
    double minx = allB[0].x0, miny = allB[0].y0, maxx = allB[0].x1, maxy = allB[0].y1;
    for (auto& b : allB) minx = std::min(minx, b.x0), miny = std::min(miny, b.y0), maxx = std::max(maxx, b.x1), maxy = std::max(maxy, b.y1);
    double dx = num(cfg, "offset_x") - minx, dy = num(cfg, "offset_y") - miny;
    auto shift = [&](const Loop& lp) {
        Loop o;
        for (auto& s : lp.segs) o.segs.push_back(s.translated(dx, dy));
        o.layer = lp.layer, o.isCircle = lp.isCircle, o.cc = {lp.cc.x + dx, lp.cc.y + dy}, o.cr = lp.cr, o.depth = lp.depth;
        return o;
    };

    struct Job { vector<Cut> holes; Cut outer; Box box; };
    vector<Job> jobs;
    for (int k : parts) {
        Job j;
        for (int h : loops[k].children) {
            Box b = loops[h].bbox();
            if (holesMode == "skip" || std::min(b.x1 - b.x0, b.y1 - b.y0) < minHole) { skippedHoles++; continue; }
            j.holes.push_back(prepareCut(shift(loops[h]), true, cfg));
        }
        Loop sp = shift(loops[k]);
        j.outer = prepareCut(sp, false, cfg);
        j.box = sp.bbox();
        jobs.push_back(std::move(j));
    }

    // urutan: part terdekat dulu; dalam part, lubang (tetangga terdekat) lalu kontur luar
    vector<const Cut*> order;
    P cur{0, 0};
    vector<int> rem;
    for (int i = 0; i < (int)jobs.size(); i++) rem.push_back(i);
    while (!rem.empty()) {
        size_t bi = 0;
        for (size_t q = 1; q < rem.size(); q++)
            if (dist(cur, {jobs[rem[q]].box.x0, jobs[rem[q]].box.y0}) < dist(cur, {jobs[rem[bi]].box.x0, jobs[rem[bi]].box.y0})) bi = q;
        Job& job = jobs[rem[bi]];
        rem.erase(rem.begin() + bi);
        vector<const Cut*> hs;
        for (auto& h : job.holes) hs.push_back(&h);
        while (!hs.empty()) {
            size_t hi = 0;
            for (size_t q = 1; q < hs.size(); q++) if (dist(cur, hs[q]->pierce) < dist(cur, hs[hi]->pierce)) hi = q;
            order.push_back(hs[hi]);
            cur = hs[hi]->pierce;
            hs.erase(hs.begin() + hi);
        }
        order.push_back(&job.outer);
        cur = job.outer.pierce;
    }

    GWriter g{upper(cfg.at("mode")), (int)num(cfg, "decimals")};
    st.name = stem(path);
    if (yes(cfg.at("header_comment"))) {
        string joined;
        for (size_t i = 0; i < geo.texts.size(); i++) joined += (i ? " " : "") + geo.texts[i];
        string cm = upper(u8(st.name) + " " + joined.substr(0, 40)), keep;
        for (char ch : cm) if (isalnum((unsigned char)ch) || (ch && strchr(" -+_.,/", ch))) keep += ch;
        g.raw("(" + trim(keep) + ")");
    }
    g.raw("G92X0Y0");
    g.raw("G21");
    g.raw(g.mode);
    bool useKerf = upper(cfg.at("kerf")) == "G41";
    for (const Cut* c : order) {
        g.move("G00", c->pierce);
        if (useKerf) g.raw("G41");
        g.raw("M07");
        g.pierces++;
        g.move("G01", c->segs[0].p0);
        for (auto& s : c->segs) {
            if (s.kind == 'L') g.move("G01", s.p1);
            else g.arc(s);
        }
        if (c->hasLo) g.move("G01", c->lo);
        g.raw("M08");
        if (useKerf) g.raw("G40");
    }
    if (yes(cfg.at("return_home"))) g.move("G00", {0, 0});
    g.raw("M02");

    st.parts = (int)parts.size();
    for (const Cut* c : order) st.holes += c->hole;
    st.skippedHoles = skippedHoles, st.pierces = g.pierces, st.cutLen = g.cutLen, st.rapidLen = g.rapidLen;
    st.lines = (int)g.lines.size(), st.open = geo.open, st.texts = geo.texts, st.layers = geo.layers, st.ignored = geo.skipped;
    st.w = maxx - minx, st.h = maxy - miny;
    return g;
}

// --------------------------------------------------------------------------
// Simulasi G-code (verifikasi + preview)
// --------------------------------------------------------------------------
struct Move { int code; P p0, p1, c; };

// Baca G-code seperti mesin
static void simulate(const vector<string>& lines, vector<Move>& moves, vector<string>& errors, vector<P>& ends) {
    bool absolute = false, cutting = false;
    P pos;
    for (size_t n = 1; n <= lines.size(); n++) {
        string line = trim(lines[n - 1].substr(0, lines[n - 1].find('(')));
        if (line.empty()) continue;
        std::map<char, string> w;
        for (size_t i = 0; i < line.size();) {
            if (!isalpha((unsigned char)line[i])) { i++; continue; }
            size_t j = i + 1;
            while (j < line.size() && (isdigit((unsigned char)line[j]) || line[j] == '.' || line[j] == '-' || line[j] == '+')) j++;
            w.emplace((char)toupper((unsigned char)line[i]), line.substr(i + 1, j - i - 1));
            i = j;
        }
        auto get = [&](char k) { auto it = w.find(k); return it == w.end() ? 0.0 : strtod(it->second.c_str(), nullptr); };
        int code = -1;
        if (w.count('G')) {
            int gv = (int)get('G');
            if (gv == 90 || gv == 91) absolute = gv == 90;
            else if (gv == 92) pos = {0, 0};
            else if (gv >= 0 && gv <= 3) code = gv;
        }
        if (w.count('M')) {
            int mv = (int)get('M');
            cutting = mv == 7 ? true : mv == 8 ? false : cutting;
        }
        if (code < 0) continue;
        double x = get('X'), y = get('Y');
        P p1 = absolute ? P{x, y} : P{pos.x + x, pos.y + y}, c;
        if (code >= 2) {
            c = {pos.x + get('I'), pos.y + get('J')};
            if (std::fabs(dist(pos, c) - dist(p1, c)) > 0.01) errors.push_back(fmt("baris %zu: radius busur tidak konsisten", n));
        }
        if (code != 0 && !cutting) errors.push_back(fmt("baris %zu: gerak potong saat torch mati", n));
        if (code == 0 && cutting) errors.push_back(fmt("baris %zu: gerak cepat saat torch menyala", n));
        moves.push_back({code, pos, p1, c});
        pos = p1;
        ends.push_back(p1);
    }
}

static void verify(const GWriter& g, vector<Move>& moves, vector<string>& errors) {
    vector<P> ends;
    simulate(g.lines, moves, errors, ends);
    if (ends.size() != g.expected.size()) {
        errors.push_back(fmt("jumlah gerakan %zu != %zu", ends.size(), g.expected.size()));
        return;
    }
    double worst = 0;
    for (size_t i = 0; i < ends.size(); i++) worst = std::max(worst, dist(ends[i], g.expected[i]));
    if (worst > 0.002) errors.push_back(fmt("posisi simulasi menyimpang %.4f mm", worst));
}

struct Result {  // hasil konversi satu gambar, belum ditulis ke file
    Stats st;
    vector<string> lines;
    vector<Move> moves;
    vector<string> errors;
    string ext;            // .TXT / .CNC
    double cutSpeed = 0;   // mm/menit, untuk estimasi waktu
};

static Result convert(const wstring& path, const Cfg& cfg) {
    Result r;
    GWriter g = generate(path, cfg, r.st);
    verify(g, r.moves, r.errors);
    r.lines = std::move(g.lines);
    r.ext = cfg.at("ext");
    if (r.ext.empty() || r.ext[0] != '.') r.ext = "." + r.ext;
    r.cutSpeed = num(cfg, "cut_speed");
    return r;
}

static string gcodeText(const vector<string>& lines) {
    string data;
    for (auto& l : lines) data += l + "\r\n";
    return data;
}

static void writeFile(const wstring& path, const string& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) throw std::runtime_error("tidak bisa menulis " + u8(path));
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok) throw std::runtime_error("gagal menulis " + u8(path));
}

// --------------------------------------------------------------------------
// Animasi potong: gerakan G-code diratakan jadi titik bertanda waktu (detik mesin)
// --------------------------------------------------------------------------
// ponytail: kecepatan gerak cepat & lama tembus tetap, hanya untuk animasi (G-code tidak memuatnya)
static const double RAPID_MM_MIN = 3000, PIERCE_S = 1.0;

struct SimPt { P p; double t; };
struct SimMove { int code; vector<SimPt> pts; };
struct Sim {
    vector<SimMove> moves;
    vector<P> pierceP;       // titik tembus, urut
    vector<double> pierceT;  // waktu mulai tembus
    double total = 0, cutMmS = 6;
    Box box{0, 0, 0, 0};     // semua gerakan + titik nol
    Box cutBox{0, 0, 0, 0};  // jalur potong saja
};

static Sim buildSim(const Result& r) {
    Sim s;
    s.cutMmS = std::max(r.cutSpeed, 1.0) / 60;
    bool anyCut = false;
    double t = 0;
    for (size_t i = 0; i < r.moves.size(); i++) {
        const Move& m = r.moves[i];
        if (m.code != 0 && (i == 0 || r.moves[i - 1].code == 0)) {  // torch menyala: tembus dulu
            s.pierceP.push_back(m.p0);
            s.pierceT.push_back(t);
            t += PIERCE_S;
        }
        vector<P> pts{m.p0};
        if (m.code >= 2) Seg{'A', m.p0, m.p1, m.c, m.code == 3}.points(pts, 3);
        else pts.push_back(m.p1);
        double v = m.code == 0 ? RAPID_MM_MIN / 60 : s.cutMmS;
        SimMove sm{m.code, {}};
        for (size_t k = 0; k < pts.size(); k++) {
            if (k) t += dist(pts[k - 1], pts[k]) / v;
            P q = pts[k];
            sm.pts.push_back({q, t});
            s.box = {std::min(s.box.x0, q.x), std::min(s.box.y0, q.y), std::max(s.box.x1, q.x), std::max(s.box.y1, q.y)};
            if (m.code == 0) continue;
            Box& c = s.cutBox;
            c = anyCut ? Box{std::min(c.x0, q.x), std::min(c.y0, q.y), std::max(c.x1, q.x), std::max(c.y1, q.y)} : Box{q.x, q.y, q.x, q.y};
            anyCut = true;
        }
        s.moves.push_back(std::move(sm));
    }
    s.total = t;
    return s;
}

static P lerpPt(const SimPt& a, const SimPt& b, double t) {
    double u = b.t > a.t ? (t - a.t) / (b.t - a.t) : 1;
    return {a.p.x + (b.p.x - a.p.x) * u, a.p.y + (b.p.y - a.p.y) * u};
}

struct Torch { P p; int mode = 0; int pierces = 0; };  // mode: 0 diam / gerak cepat, 1 tembus, 2 memotong

static Torch torchAt(const Sim& s, double t) {
    Torch tr;
    for (double pt : s.pierceT) tr.pierces += pt <= t;
    for (auto& m : s.moves) {
        if (t < m.pts.front().t) {  // menunggu gerakan ini: kalau gerak potong, sedang tembus
            tr.p = m.pts.front().p;
            tr.mode = m.code != 0;
            return tr;
        }
        if (t <= m.pts.back().t) {
            size_t k = 1;
            while (k + 1 < m.pts.size() && m.pts[k].t < t) k++;
            tr.p = m.pts.size() > 1 ? lerpPt(m.pts[k - 1], m.pts[k], t) : m.pts[0].p;
            tr.mode = m.code != 0 ? 2 : 0;
            return tr;
        }
    }
    if (!s.moves.empty()) tr.p = s.moves.back().pts.back().p;
    return tr;
}

// Titik-titik gerakan yang sudah dilalui sampai waktu t
static void donePts(const SimMove& m, double t, vector<P>& out) {
    out.clear();
    for (size_t k = 0; k < m.pts.size(); k++) {
        if (m.pts[k].t <= t) { out.push_back(m.pts[k].p); continue; }
        if (k) out.push_back(lerpPt(m.pts[k - 1], m.pts[k], t));
        break;
    }
}

struct View2D { double cx = 0, cy = 0, s = 0; };  // pusat (mm) + skala px/mm; s = 0: pas layar

static Gdiplus::RectF plotArea(Gdiplus::REAL W, Gdiplus::REAL H, Gdiplus::REAL k, bool title) {
    Gdiplus::REAL T = (title ? 50 : 20) * k;
    return Gdiplus::RectF(75 * k, T, W - 105 * k, H - T - 70 * k);
}

static View2D fitView(const Box& b, const Gdiplus::RectF& a) {
    View2D v;
    v.cx = (b.x0 + b.x1) / 2, v.cy = (b.y0 + b.y1) / 2;
    v.s = std::min(a.Width / std::max(b.x1 - b.x0, 1e-6), a.Height / std::max(b.y1 - b.y0, 1e-6)) / 1.08;
    return v;
}

// Gambar jalur potong sampai waktu t. Untuk _preview.png (t setelah selesai, 96 dpi)
// dan tampilan 2D interaktif di dwg2cnc-gui.exe (zoom/geser lewat v).
static void drawPreview(Gdiplus::Graphics& g, Gdiplus::REAL W, Gdiplus::REAL H, const Result& r, const Sim& sim, View2D v, double t, bool title) {
    using namespace Gdiplus;
    const Stats& st = r.st;
    const REAL k = g.GetDpiX() / 96;   // skala layar; PNG = 1
    const bool live = t < sim.total;   // animasi berjalan: tampilkan sisa jalur + torch
    const RectF a = plotArea(W, H, k, title);
    const REAL L = a.X, T = a.Y, pw = a.Width, ph = a.Height;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(255, 255, 255, 255));
    if (pw < 10 || ph < 10) return;
    if (v.s <= 0) v = fitView(sim.box, a);
    const double s = v.s, cx = v.cx, cy = v.cy;
    auto X = [&](double x) { return (REAL)(L + pw / 2 + (x - cx) * s); };
    auto Y = [&](double y) { return (REAL)(T + ph / 2 - (y - cy) * s); };

    FontFamily ff(L"Segoe UI");
    Font fSmall(&ff, 8), fTiny(&ff, 6), fBig(&ff, 11);
    SolidBrush ink(Color(255, 40, 40, 40)), red(Color(255, 214, 39, 40)), redFaint(Color(90, 214, 39, 40));
    StringFormat mid, right;
    mid.SetAlignment(StringAlignmentCenter);
    right.SetAlignment(StringAlignmentFar);

    // grid + skala mm
    Pen grid(Color(255, 228, 228, 228), 1), frame(Color(255, 140, 140, 140), 1);
    double vx0 = cx - pw / 2 / s, vx1 = cx + pw / 2 / s, vy0 = cy - ph / 2 / s, vy1 = cy + ph / 2 / s;
    double raw = std::max(vx1 - vx0, vy1 - vy0) / 12, mag = std::pow(10, std::floor(std::log10(raw)));
    double step = mag * (raw / mag >= 5 ? 5 : raw / mag >= 2 ? 2 : 1);
    for (double x = std::ceil(vx0 / step) * step; x <= vx1; x += step) {
        g.DrawLine(&grid, X(x), T, X(x), T + ph);
        g.DrawString(wide(fmt("%g", std::fabs(x) < step / 1e6 ? 0.0 : x)).c_str(), -1, &fSmall, PointF(X(x), T + ph + 4 * k), &mid, &ink);
    }
    for (double y = std::ceil(vy0 / step) * step; y <= vy1; y += step) {
        g.DrawLine(&grid, L, Y(y), L + pw, Y(y));
        g.DrawString(wide(fmt("%g", std::fabs(y) < step / 1e6 ? 0.0 : y)).c_str(), -1, &fSmall, PointF(L - 4 * k, Y(y) - 7 * k), &right, &ink);
    }
    g.DrawRectangle(&frame, L, T, pw, ph);
    g.SetClip(a);

    // abu putus-putus = gerak cepat, biru = sudah dipotong, biru muda = belum
    Pen rapid(Color(255, 168, 168, 168), k), cut(Color(255, 31, 95, 191), 1.4f * k), ghost(Color(255, 200, 212, 230), 1.2f * k);
    REAL dash[] = {3, 3};
    rapid.SetDashPattern(dash, 2);
    cut.SetLineJoin(LineJoinRound);
    vector<PointF> pf;
    vector<P> pts;
    auto line = [&](const vector<P>& p, Pen* pen) {
        if (p.size() < 2) return;
        pf.clear();
        for (auto& q : p) pf.push_back(PointF(X(q.x), Y(q.y)));
        g.DrawLines(pen, pf.data(), (INT)pf.size());
    };
    for (int pass = 0; pass < 3; pass++)
        for (auto& m : sim.moves) {
            if ((m.code == 0) != (pass == 0) || (pass == 1 && !live)) continue;
            if (pass == 2) donePts(m, t, pts);
            else { pts.clear(); for (auto& q : m.pts) pts.push_back(q.p); }
            line(pts, pass == 0 ? &rapid : pass == 1 ? &ghost : &cut);
        }

    // merah = titik tembus + urutan (pudar: belum ditembus)
    for (size_t i = 0; i < sim.pierceP.size(); i++) {
        REAL px = X(sim.pierceP[i].x), py = Y(sim.pierceP[i].y);
        SolidBrush* b = sim.pierceT[i] <= t ? &red : &redFaint;
        g.FillEllipse(b, px - 2.5f * k, py - 2.5f * k, 5 * k, 5 * k);
        g.DrawString(std::to_wstring(i + 1).c_str(), -1, &fTiny, PointF(px + 2 * k, py - 11 * k), b);
    }

    Pen blk(Color(255, 0, 0, 0), 1.5f * k);
    REAL ox = X(0), oy = Y(0);
    g.DrawLine(&blk, ox - 8 * k, oy, ox + 8 * k, oy);
    g.DrawLine(&blk, ox, oy - 8 * k, ox, oy + 8 * k);
    g.DrawString(L"titik nol torch", -1, &fSmall, PointF(ox + 6 * k, oy + 3 * k), &ink);

    if (live) {  // torch: nyala oranye saat tembus/memotong
        Torch tr = torchAt(sim, t);
        REAL px = X(tr.p.x), py = Y(tr.p.y);
        if (tr.mode) {
            REAL gr = (tr.mode == 1 ? 13 : 9) * k;
            SolidBrush glow(Color(110, 255, 140, 30)), core(Color(255, 255, 225, 130));
            g.FillEllipse(&glow, px - gr, py - gr, 2 * gr, 2 * gr);
            g.FillEllipse(&core, px - 3 * k, py - 3 * k, 6 * k, 6 * k);
        }
        Pen tp(Color(255, 30, 30, 30), 1.5f * k);
        g.DrawEllipse(&tp, px - 6 * k, py - 6 * k, 12 * k, 12 * k);
        g.DrawLine(&tp, px - 12 * k, py, px - 6 * k, py);
        g.DrawLine(&tp, px + 6 * k, py, px + 12 * k, py);
        g.DrawLine(&tp, px, py - 12 * k, px, py - 6 * k);
        g.DrawLine(&tp, px, py + 6 * k, px, py + 12 * k);
    }
    g.ResetClip();

    if (title) {
        double sp = r.cutSpeed, tm = sp > 0 ? st.cutLen / sp : 0;
        string tl = fmt("%s   %.0f x %.0f mm   |   %d lubang, %d titik tembus   |   panjang potong %.2f m (~%.0f menit @ %.0f mm/mnt, belum termasuk preheat)",
                        u8(st.name).c_str(), st.w, st.h, st.holes, st.pierces, st.cutLen / 1000, tm, sp);
        g.DrawString(wide(tl).c_str(), -1, &fBig, PointF(W / 2, 14 * k), &mid, &ink);
    }
    g.DrawString(title ? L"X (mm)    biru = potong, abu putus-putus = gerak cepat, merah = titik tembus + urutan"
                       : L"biru = sudah dipotong, biru muda = belum   ·   scroll = zoom, seret = geser, klik dua kali = pas layar, Spasi = putar/jeda",
                 -1, &fSmall, PointF(W / 2, H - 30 * k), &mid, &ink);
}

static const CLSID PNG_ENCODER = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

static bool savePreviewPng(const Result& r, const wstring& png) {
    Sim sim = buildSim(r);
    Gdiplus::Bitmap bmp(1320, 1020, PixelFormat24bppRGB);
    bmp.SetResolution(96, 96);
    {
        Gdiplus::Graphics g(&bmp);
        drawPreview(g, 1320, 1020, r, sim, View2D{}, sim.total + 1, true);
    }
    return bmp.Save(png.c_str(), &PNG_ENCODER, nullptr) == Gdiplus::Ok;
}

// --------------------------------------------------------------------------
// Simulasi 3D: OpenGL 1.1 bawaan Windows (tanpa library tambahan)
// --------------------------------------------------------------------------
struct Cam3D { double yaw = -25, pitch = 40, dist = 1000, tx = 0, ty = 0; };  // derajat, mm
struct Scene { double x0, y0, x1, y1, thick, size, R; };                      // plat + jari-jari torch (mm)
struct Spark { double x, y, z, vx, vy, vz, life; };
static vector<Spark> g_sparks;
static GLuint g_fontBase = 0;

static double guessThickness(const vector<string>& texts) {  // "PL14" di gambar = plat 14 mm
    string s;
    for (auto& x : texts) s += upper(x) + " ";
    for (size_t i = s.find("PL"); i != string::npos; i = s.find("PL", i + 1)) {
        double v = atof(s.c_str() + s.find_first_not_of(' ', i + 2));
        if (v > 0 && v < 300) return v;
    }
    return 10;
}

static Scene sceneOf(const Sim& s, const Result& r) {
    Scene c;
    c.x0 = std::min(0.0, s.cutBox.x0 - 15), c.y0 = std::min(0.0, s.cutBox.y0 - 15);
    c.x1 = s.cutBox.x1 + 15, c.y1 = s.cutBox.y1 + 15;
    c.size = std::max(c.x1 - c.x0, c.y1 - c.y0);
    c.thick = guessThickness(r.st.texts);
    c.R = std::max(c.size * 0.006, std::min(16.0, c.size * 0.03));  // torch asli ~16 mm, diskalakan untuk plat sangat besar/kecil
    return c;
}

static void resetCam(Cam3D& c, const Scene& sc) {
    c = Cam3D{};
    c.dist = sc.size * 1.5;
    c.tx = (sc.x0 + sc.x1) / 2, c.ty = (sc.y0 + sc.y1) / 2;
}

static void updateSparks(const Scene& sc, const Torch& tr, double dt, bool active) {
    double grav = sc.size * 0.8, sp = sc.size * 0.06;
    for (auto& p : g_sparks) p.vz -= grav * dt, p.x += p.vx * dt, p.y += p.vy * dt, p.z += p.vz * dt, p.life -= dt;
    g_sparks.erase(std::remove_if(g_sparks.begin(), g_sparks.end(), [](const Spark& p) { return p.life <= 0; }), g_sparks.end());
    if (!active || !tr.mode) return;
    auto rnd01 = [] { return rand() / (double)RAND_MAX; };
    for (int i = 0, n = (int)(dt * 700); i < n && g_sparks.size() < 3000; i++) {
        double a = rnd01() * TAU, h = rnd01() * sp;
        bool up = tr.mode == 1 || rnd01() < 0.3;  // tembus: cipratan ke atas; memotong: sebagian besar jatuh di bawah plat
        g_sparks.push_back({tr.p.x, tr.p.y, up ? 0 : -sc.thick, std::cos(a) * h, std::sin(a) * h,
                            up ? rnd01() * sp * 0.8 : -rnd01() * sp * 1.5, 0.3 + rnd01() * 0.5});
    }
}

static void glBox(double x0, double y0, double z0, double x1, double y1, double z1) {
    glBegin(GL_QUADS);
    glNormal3d(0, 0, 1), glVertex3d(x0, y0, z1), glVertex3d(x1, y0, z1), glVertex3d(x1, y1, z1), glVertex3d(x0, y1, z1);
    glNormal3d(0, 0, -1), glVertex3d(x0, y0, z0), glVertex3d(x0, y1, z0), glVertex3d(x1, y1, z0), glVertex3d(x1, y0, z0);
    glNormal3d(0, -1, 0), glVertex3d(x0, y0, z0), glVertex3d(x1, y0, z0), glVertex3d(x1, y0, z1), glVertex3d(x0, y0, z1);
    glNormal3d(0, 1, 0), glVertex3d(x0, y1, z0), glVertex3d(x0, y1, z1), glVertex3d(x1, y1, z1), glVertex3d(x1, y1, z0);
    glNormal3d(-1, 0, 0), glVertex3d(x0, y0, z0), glVertex3d(x0, y0, z1), glVertex3d(x0, y1, z1), glVertex3d(x0, y1, z0);
    glNormal3d(1, 0, 0), glVertex3d(x1, y0, z0), glVertex3d(x1, y1, z0), glVertex3d(x1, y1, z1), glVertex3d(x1, y0, z1);
    glEnd();
}

static void glCone(double x, double y, double z0, double z1, double r0, double r1) {  // kerucut terpotong tegak + tutup atas
    const int N = 24;
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= N; i++) {
        double a = TAU * i / N, c = std::cos(a), s = std::sin(a);
        glNormal3d(c, s, (r0 - r1) / (z1 - z0));
        glVertex3d(x + r0 * c, y + r0 * s, z0), glVertex3d(x + r1 * c, y + r1 * s, z1);
    }
    glEnd();
    glBegin(GL_TRIANGLE_FAN);
    glNormal3d(0, 0, 1), glVertex3d(x, y, z1);
    for (int i = N; i >= 0; i--) glVertex3d(x + r1 * std::cos(TAU * i / N), y + r1 * std::sin(TAU * i / N), z1);
    glEnd();
}

static void render3D(int W, int H, const Sim& s, const Scene& sc, const Cam3D& cam, double t, const wchar_t* hint) {
    glViewport(0, 0, W, H);
    glClearColor(0.80f, 0.84f, 0.87f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (W < 2 || H < 2) return;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    double n = cam.dist * 0.02, f = cam.dist * 4 + sc.size * 4, fy = std::tan(20 * DEG) * n;
    glFrustum(-fy * W / H, fy * W / H, -fy, fy, n, f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslated(0, 0, -cam.dist);
    glRotated(cam.pitch - 90, 1, 0, 0);  // pitch 90 = dari atas
    glRotated(-cam.yaw, 0, 0, 1);
    glTranslated(-cam.tx, -cam.ty, 0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_NORMALIZE);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    GLfloat lp[] = {0.4f, -0.6f, 1, 0}, amb[] = {0.45f, 0.45f, 0.47f, 1}, dif[] = {0.75f, 0.75f, 0.75f, 1};
    glLightfv(GL_LIGHT0, GL_POSITION, lp);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glColor3f(0.55f, 0.58f, 0.62f);
    glBox(sc.x0, sc.y0, -sc.thick, sc.x1, sc.y1, 0);  // plat baja

    Torch tr = torchAt(s, t);
    double lift = std::max(0.02, sc.size * 0.0005), gap = sc.R * 0.4, R = sc.R;
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // jalur yang belum dipotong: garis kapur
    glLineWidth(1);
    glColor4f(1, 1, 1, 0.65f);
    for (auto& m : s.moves) {
        if (m.code == 0 || m.pts.back().t <= t) continue;
        glBegin(GL_LINE_STRIP);
        for (auto& q : m.pts) glVertex3d(q.p.x, q.p.y, lift);
        glEnd();
    }
    // gerak cepat: putus-putus setinggi torch
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(2, 0x3333);
    glColor4f(0.25f, 0.27f, 0.32f, 0.5f);
    for (auto& m : s.moves) {
        if (m.code != 0) continue;
        glBegin(GL_LINE_STRIP);
        for (auto& q : m.pts) glVertex3d(q.p.x, q.p.y, gap);
        glEnd();
    }
    glDisable(GL_LINE_STIPPLE);
    // kerf yang sudah dipotong: hitam, membara di belakang torch
    glLineWidth(2.5f);
    double heat = R * 4 / s.cutMmS;  // detik
    for (auto& m : s.moves) {
        if (m.code == 0) continue;
        if (m.pts.front().t > t) break;
        glBegin(GL_LINE_STRIP);
        for (size_t k = 0; k < m.pts.size(); k++) {
            P p = m.pts[k].p;
            double qt = m.pts[k].t;
            if (qt > t) p = lerpPt(m.pts[k - 1], m.pts[k], t), qt = t;
            double hh = std::max(0.0, 1 - (t - qt) / heat);
            glColor4d(0.12 + 0.88 * hh, 0.12 + 0.4 * hh, 0.13, 1);
            glVertex3d(p.x, p.y, lift * 2);
            if (m.pts[k].t > t) break;
        }
        glEnd();
    }

    // torch: badan + nozzle tembaga
    glEnable(GL_LIGHTING);
    glColor3f(0.72f, 0.74f, 0.77f);
    glCone(tr.p.x, tr.p.y, gap + R * 1.3, gap + R * 7, R, R);
    glColor3f(0.80f, 0.52f, 0.28f);
    glCone(tr.p.x, tr.p.y, gap, gap + R * 1.3, R * 0.35, R);
    glDisable(GL_LIGHTING);

    // nyala api + percikan (additive)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    if (tr.mode && t < s.total) {
        double fl = tr.mode == 1 ? 1.4 : 1;  // saat tembus api lebih besar
        glBegin(GL_TRIANGLE_FAN);
        glColor4f(1, 0.95f, 0.7f, 0.9f), glVertex3d(tr.p.x, tr.p.y, -sc.thick * 0.6);
        glColor4f(0.45f, 0.65f, 1, 0.5f);
        for (int i = 0; i <= 16; i++) glVertex3d(tr.p.x + R * 0.3 * fl * std::cos(TAU * i / 16), tr.p.y + R * 0.3 * fl * std::sin(TAU * i / 16), gap);
        glEnd();
        glPointSize(14);
        glBegin(GL_POINTS);
        glColor4f(1, 0.55f, 0.15f, 0.55f), glVertex3d(tr.p.x, tr.p.y, lift * 3);
        glEnd();
    }
    glPointSize(2);
    glBegin(GL_POINTS);
    for (auto& p : g_sparks) glColor4f(1, 0.75f, 0.3f, (float)std::min(1.0, p.life * 2)), glVertex3d(p.x, p.y, p.z);
    glEnd();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    if (hint && g_fontBase) {  // petunjuk di pojok kiri bawah
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, W, 0, H, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glColor3f(0.15f, 0.17f, 0.2f);
        glRasterPos2i(8, 8);
        glListBase(g_fontBase);
        glCallLists((GLsizei)wcslen(hint), GL_UNSIGNED_SHORT, hint);
    }
}

// Konteks OpenGL untuk jendela (double buffer) atau bitmap (render ke PNG, uji tanpa layar)
static HGLRC initGL(HDC dc, bool bitmap, HFONT font) {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_SUPPORT_OPENGL | (bitmap ? PFD_DRAW_TO_BITMAP | PFD_SUPPORT_GDI : PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER);
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return nullptr;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) return nullptr;
    HGDIOBJ old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT));
    g_fontBase = glGenLists(256);
    if (!wglUseFontBitmapsW(dc, 0, 256, g_fontBase)) wglUseFontBitmapsW(dc, 0, 256, g_fontBase);  // kadang gagal di panggilan pertama
    SelectObject(dc, old);
    return rc;
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------
static string joinStr(const vector<string>& v, const char* sep) {
    string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? sep : "") + v[i];
    return s;
}

static void initExeDir() {
    wchar_t mp[32768];
    g_exeDir = dirName(wstring(mp, GetModuleFileNameW(nullptr, mp, 32768))) + L"\\";
}

// DEFAULTS <- dwg2cnc.ini (atau --config) <- opsi command line
static Cfg loadConfig(const wstring& config, const Cfg& over) {
    Cfg cfg(DEFAULTS.begin(), DEFAULTS.end());
    loadIni(config.empty() ? g_exeDir + L"dwg2cnc.ini" : config, cfg);
    for (auto& o : over) cfg[o.first] = o.second;
    return cfg;
}

// Folder diganti isinya: semua .dwg/.dxf, urut nama
static vector<wstring> collectFiles(const vector<wstring>& inputs) {
    vector<wstring> files;
    for (auto& p : inputs) {
        if (!isDir(p)) { files.push_back(p); continue; }
        vector<wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(joinPath(p, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wstring e = lowerExt(fd.cFileName);
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (e == L".dwg" || e == L".dxf")) names.push_back(fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(names.begin(), names.end());
        for (auto& n : names) files.push_back(joinPath(p, n));
    }
    return files;
}

static bool convertFile(const wstring& path, const Cfg& cfg, const wstring& outdirArg) {
    Result r = convert(path, cfg);
    const Stats& st = r.st;
    const vector<string>& errors = r.errors;
    wstring outdir = absPath(outdirArg.empty() ? dirName(absPath(path)) : outdirArg);
    SHCreateDirectoryExW(nullptr, outdir.c_str(), nullptr);
    wstring out = joinPath(outdir, st.name + wide(r.ext));
    string data = gcodeText(r.lines);
    writeFile(out, data);
    wstring png;
    if (yes(cfg.at("preview"))) {
        png = joinPath(outdir, st.name + L"_preview.png");
        if (!savePreviewPng(r, png)) png.clear();
    }
    double kb = data.size() / 1024.0;
    printf("\n[%s]\n", u8(st.name).c_str());
    printf("  Layer dipakai   : %s", joinStr(st.layers, ", ").c_str());
    if (!st.ignored.empty()) printf("   (diabaikan: %s)", joinStr(st.ignored, ", ").c_str());
    printf("\n");
    if (!st.texts.empty()) printf("  Teks di gambar  : %s\n", joinStr(st.texts, " | ").c_str());
    printf("  Ukuran          : %.1f x %.1f mm, %d part\n", st.w, st.h, st.parts);
    printf("  Lubang dipotong : %d", st.holes);
    if (st.skippedHoles) printf("  (dilewati %d)", st.skippedHoles);
    printf("\n  Panjang potong  : %.2f m, gerak cepat %.2f m, %d kali tembus\n", st.cutLen / 1000, st.rapidLen / 1000, st.pierces);
    if (st.open) printf("  Info            : %d garis terbuka diabaikan (tanda center / garis bantu)\n", st.open);
    printf("  Output          : %s  (%d baris, %.1f KB)\n", u8(out).c_str(), st.lines, kb);
    if (!png.empty()) printf("  Preview         : %s\n", u8(png).c_str());
    if (st.lines > 10000 || kb > 1024) printf("  PERINGATAN      : melebihi 10.000 baris / 1 MB (batas manual F2100B). Pecah gambarnya.\n");
    vector<string> first5(errors.begin(), errors.begin() + std::min<size_t>(errors.size(), 5));
    printf("  Cek simulasi    : %s\n", errors.empty() ? "OK" : ("GAGAL - " + joinStr(first5, "; ")).c_str());
    return errors.empty();
}

static void usage() {
    printf(
        "dwg2cnc - DWG/DXF -> G-code untuk FLSK F2100B\n\n"
        " Cara pakai: seret satu atau beberapa file DWG/DXF\n"
        " (atau satu folder) ke ikon dwg2cnc.exe ini.\n\n"
        " Hasil .TXT dan _preview.png muncul di folder yang sama dengan gambar.\n"
        " Pengaturan ada di dwg2cnc.ini (taruh di samping dwg2cnc.exe).\n\n"
        "Command line:\n"
        "  dwg2cnc.exe file.dwg|file.dxf|folder ... [opsi]\n"
        "  -o, --outdir FOLDER   folder output (default: sama dengan gambar)\n"
        "  --config FILE         file pengaturan (default: dwg2cnc.ini di samping exe)\n");
    for (auto& d : DEFAULTS) {
        string k = d.first;
        std::replace(k.begin(), k.end(), '_', '-');
        printf("  --%-24s default: %s\n", k.c_str(), d.second.c_str());
    }
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    DWORD procs[2];
    bool ownConsole = GetConsoleProcessList(procs, 2) == 1;  // dibuka lewat seret/klik dua kali
    auto finish = [&](int code) {
        if (ownConsole) { printf("\nTekan Enter untuk menutup..."); fflush(stdout); getchar(); }
        return code;
    };
    initExeDir();

    vector<wstring> inputs;
    wstring outdir, config;
    Cfg over;
    for (int i = 1; i < argc; i++) {
        wstring a = argv[i];
        if (a == L"-h" || a == L"--help" || a == L"/?") { usage(); return finish(0); }
        if (a.size() < 2 || a[0] != L'-') { inputs.push_back(a); continue; }
        string key = u8(a), val;
        size_t eq = key.find('=');
        bool hasVal = eq != string::npos;
        if (hasVal) val = key.substr(eq + 1), key.resize(eq);
        string k = key.compare(0, 2, "--") == 0 ? key.substr(2) : key;
        std::replace(k.begin(), k.end(), '-', '_');
        bool known = key == "-o" || k == "outdir" || k == "config";
        for (auto& d : DEFAULTS) known = known || d.first == k;
        if (!known) { fprintf(stderr, "Opsi tidak dikenal: %s (lihat dwg2cnc.exe --help)\n", key.c_str()); return finish(2); }
        if (!hasVal) {
            if (i + 1 >= argc) { fprintf(stderr, "Opsi %s butuh nilai\n", key.c_str()); return finish(2); }
            val = u8(argv[++i]);
        }
        if (key == "-o" || k == "outdir") outdir = wide(val);
        else if (k == "config") config = wide(val);
        else over[k] = val;
    }
    if (inputs.empty()) { usage(); return finish(0); }

    Cfg cfg = loadConfig(config, over);
    vector<wstring> files = collectFiles(inputs);

    ULONG_PTR gdip = 0;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdip, &gsi, nullptr);
    int ok = 0, bad = 0;
    for (auto& f : files) {
        try {
            (convertFile(f, cfg, outdir) ? ok : bad)++;
        } catch (const std::exception& e) {
            bad++;
            printf("\n[%s] GAGAL: %s\n", u8(baseName(f)).c_str(), e.what());
        }
        fflush(stdout);
    }
    Gdiplus::GdiplusShutdown(gdip);
    printf("\nSelesai: %d berhasil, %d bermasalah.\n", ok, bad);
    return finish(bad == 0 ? 0 : 1);
}

// --------------------------------------------------------------------------
// GUI (dwg2cnc-gui.exe): seret file, daftar hasil, animasi potong 2D/3D, Simpan .TXT
// --------------------------------------------------------------------------
enum { ID_DROP = 101, ID_LIST, ID_SAVE, ID_STATS, ID_STATUS, ID_PLAY, ID_TRACK, ID_TIME, ID_SPEED, ID_2D, ID_3D, ID_FOLLOW };
enum { WM_APP_START = WM_APP + 1, WM_APP_DONE };

struct Item { wstring path; int state = 0; string err; std::unique_ptr<Result> res; };  // state: 0 menunggu, 1 diproses, 2 selesai
struct Task { int idx = 0; wstring path; Cfg cfg; };
struct Done { int idx; std::unique_ptr<Result> res; string err; };

static vector<Item> g_items;
static int g_cur = -1;  // item yang tampil
static HWND g_wnd, g_drop, g_list, g_save, g_stats, g_status, g_prev, g_gl;
static HWND g_play, g_track, g_time, g_speedBox, g_b2d, g_b3d, g_followBox;
static HFONT g_font;
static HGLRC g_glrc;
static int g_dpi = 96;
static std::mutex g_mx;
static std::condition_variable g_cv;
static std::deque<Task> g_queue;

// animasi item yang tampil
static int g_simFor = -1;
static Sim g_sim;
static Scene g_scene;
static double g_t = 0, g_speed = 10;
static bool g_playing = false, g_mode3d = false, g_follow = false;
static View2D g_view;
static Cam3D g_cam;
static LARGE_INTEGER g_tick;
static POINT g_drag;
static int g_dragBtn = 0;
static const double SPEEDS[] = {1, 2, 5, 10, 20, 50, 100};
static const wchar_t* HINT_3D = L"seret kiri = putar kamera  ·  seret kanan = geser  ·  scroll = zoom  ·  klik dua kali = reset  ·  Spasi = putar/jeda";

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static string idNum(double v, int dec) {  // angka gaya Indonesia: 3,24
    string s = fmt("%.*f", dec, v);
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

static void setTextIfChanged(HWND h, const wstring& s) {  // hindari kedip saat diperbarui tiap frame
    wchar_t cur[512];
    GetWindowTextW(h, cur, 512);
    if (s != cur) SetWindowTextW(h, s.c_str());
}

// Satu thread konversi, file diproses berurutan; tiap DWG tetap dibaca proses LibreDWG sendiri
static void worker() {
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            t = std::move(g_queue.front());
            g_queue.pop_front();
        }
        PostMessageW(g_wnd, WM_APP_START, t.idx, 0);
        Done* d = new Done{t.idx, nullptr, ""};
        try {
            d->res = std::make_unique<Result>(convert(t.path, t.cfg));
        } catch (const std::exception& e) {
            d->err = e.what();
        } catch (...) {
            d->err = "error tidak dikenal";
        }
        PostMessageW(g_wnd, WM_APP_DONE, 0, (LPARAM)d);
    }
}

static void setRow(int i) {
    const Item& it = g_items[i];
    wstring status, info;
    if (it.state == 0) status = L"Menunggu";
    else if (it.state == 1) status = L"Memproses…";
    else if (!it.res) status = L"Gagal", info = wide(it.err);
    else {
        const Stats& st = it.res->st;
        status = it.res->errors.empty() ? L"OK" : L"Cek gagal";
        info = wide(idNum(st.w, 0) + " × " + idNum(st.h, 0) + " mm, " + std::to_string(st.holes) + " lubang, " + idNum(st.cutLen / 1000, 2) + " m");
    }
    ListView_SetItemText(g_list, i, 1, (LPWSTR)status.c_str());
    ListView_SetItemText(g_list, i, 2, (LPWSTR)info.c_str());
}

static vector<int> selectedRows() {
    vector<int> v;
    for (int i = -1; (i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) >= 0;) v.push_back(i);
    return v;
}

static void showProgress() {
    int done = 0, bad = 0;
    for (auto& it : g_items)
        if (it.state == 2) done++, bad += !it.res || !it.res->errors.empty();
    wstring s = done < (int)g_items.size() ? wide(fmt("Memproses… %d dari %d", done, (int)g_items.size()))
                                           : wide(fmt("Selesai: %d berhasil, %d bermasalah.", done - bad, bad));
    SetWindowTextW(g_status, s.c_str());
}

// ---- animasi
static string clockStr(double s) {
    int v = (int)(s + 0.5);
    return fmt("%02d:%02d", v / 60, v % 60);
}

static void redrawView() { InvalidateRect(g_mode3d ? g_gl : g_prev, nullptr, FALSE); }

static void updateTimeUi() {
    wstring tl;
    if (g_simFor >= 0) {
        Torch tr = torchAt(g_sim, g_t);
        const char* what = g_t >= g_sim.total ? "selesai" : tr.mode == 2 ? "memotong" : tr.mode == 1 ? "tembus" : "gerak cepat";
        tl = wide(clockStr(g_t) + " / " + clockStr(g_sim.total) + "  ·  " + what + fmt("  %d/%d", tr.pierces, (int)g_sim.pierceT.size()));
        LPARAM pos = (LPARAM)(g_sim.total > 0 ? g_t / g_sim.total * 1000 : 0);
        if (GetCapture() != g_track && SendMessageW(g_track, TBM_GETPOS, 0, 0) != pos) SendMessageW(g_track, TBM_SETPOS, TRUE, pos);
    }
    setTextIfChanged(g_time, tl);
    setTextIfChanged(g_play, g_playing ? L"▌▌ Jeda" : L"► Putar");
}

static void setPlaying(bool on) {
    if (on && g_t >= g_sim.total) g_t = 0, g_sparks.clear();  // sudah selesai: ulang dari awal
    g_playing = on && g_simFor >= 0;
    QueryPerformanceCounter(&g_tick);
    updateTimeUi();
    redrawView();
}

static void setMode(bool m3d) {
    g_mode3d = m3d && g_glrc;
    ShowWindow(g_prev, g_mode3d ? SW_HIDE : SW_SHOW);
    ShowWindow(g_gl, g_mode3d ? SW_SHOW : SW_HIDE);
    SendMessageW(g_b2d, BM_SETCHECK, g_mode3d ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(g_b3d, BM_SETCHECK, g_mode3d ? BST_CHECKED : BST_UNCHECKED, 0);
    g_sparks.clear();
    redrawView();
}

static void stopFollow() {  // pengguna menggeser sendiri: berhenti mengikuti torch
    g_follow = false;
    SendMessageW(g_followBox, BM_SETCHECK, BST_UNCHECKED, 0);
}

static bool viewKey(WPARAM k) {  // Spasi = putar/jeda, Home/End = awal/akhir
    if (g_simFor < 0) return false;
    if (k == VK_SPACE) setPlaying(!g_playing);
    else if (k == VK_HOME) g_t = 0, g_sparks.clear();
    else if (k == VK_END) g_t = g_sim.total, g_sparks.clear();
    else return false;
    updateTimeUi();
    redrawView();
    return true;
}

// Item terpilih: ringkasan di atas, animasi jalur potong di bawahnya
static void showCurrent() {
    vector<int> sel = selectedRows();
    int f = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED | LVNI_SELECTED);
    g_cur = f >= 0 ? f : sel.empty() ? -1 : sel[0];
    string text;
    if (g_cur < 0) {
        text = "Seret file DWG / DXF (boleh banyak, atau folder) ke jendela ini, lalu pilih hasilnya di daftar.\n"
               "Pengaturan dibaca dari dwg2cnc.ini di samping exe.";
    } else {
        const Item& it = g_items[g_cur];
        string name = u8(baseName(it.path));
        if (it.state < 2) text = name + "\nMasih diproses…";
        else if (!it.res) text = name + "  —  GAGAL\n" + it.err;
        else {
            const Result& r = *it.res;
            const Stats& st = r.st;
            size_t bytes = 0;
            for (auto& l : r.lines) bytes += l.size() + 2;
            text = u8(st.name) + "      " + idNum(st.w, 0) + " × " + idNum(st.h, 0) + " mm  ·  " + std::to_string(st.holes) + " lubang  ·  " +
                   std::to_string(st.pierces) + " titik tembus  ·  potong " + idNum(st.cutLen / 1000, 2) + " m  ·  ±" +
                   idNum(r.cutSpeed > 0 ? st.cutLen / r.cutSpeed : 0, 0) + " menit  ·  " + std::to_string(st.lines) + " baris  ·  " +
                   (r.errors.empty() ? "Cek simulasi OK" : "CEK SIMULASI GAGAL") +
                   "\nLayer dipotong: " + (st.layers.empty() ? "-" : joinStr(st.layers, ", "));
            if (!st.ignored.empty()) text += "  ·  diabaikan: " + joinStr(st.ignored, ", ");
            if (st.open) text += "  ·  " + std::to_string(st.open) + " garis terbuka diabaikan";
            if (st.lines > 10000 || bytes > 1048576) text += "\nPERINGATAN: melebihi 10.000 baris / 1 MB (batas F2100B). Pecah gambarnya.";
            if (!r.errors.empty())
                text += "\nMasalah: " + joinStr(vector<string>(r.errors.begin(), r.errors.begin() + std::min<size_t>(r.errors.size(), 3)), "; ");
            if (!st.texts.empty()) {
                string t = joinStr(st.texts, " | ");
                std::replace(t.begin(), t.end(), '\n', ' ');
                text += "\nTeks di gambar: " + t;
            }
        }
    }
    SetWindowTextW(g_stats, wide(text).c_str());
    int n = 0;
    for (int i : sel) n += g_items[i].res != nullptr;
    EnableWindow(g_save, n > 0);
    SetWindowTextW(g_save, n > 1 ? wide(fmt("Simpan %d file .TXT", n)).c_str() : L"Simpan .TXT");

    // item lain tampil: muat animasinya dan langsung putar
    int want = g_cur >= 0 && g_items[g_cur].res && !g_items[g_cur].res->moves.empty() ? g_cur : -1;
    if (want != g_simFor) {
        g_simFor = want;
        g_sim = want >= 0 ? buildSim(*g_items[want].res) : Sim{};
        if (want >= 0) {
            g_scene = sceneOf(g_sim, *g_items[want].res);
            resetCam(g_cam, g_scene);
        }
        g_view = View2D{};
        g_sparks.clear();
        g_t = 0;
        for (HWND c : {g_play, g_track}) EnableWindow(c, want >= 0);
        setPlaying(want >= 0);
    }
    redrawView();
}

static void addFiles(const vector<wstring>& inputs) {
    Cfg cfg = loadConfig(L"", {});  // dwg2cnc.ini dibaca ulang tiap kali: perubahan langsung terpakai
    int added = 0;
    for (auto& f : collectFiles(inputs)) {
        wstring e = lowerExt(f);
        if (e != L".dwg" && e != L".dxf") continue;
        int idx = (int)g_items.size();
        g_items.push_back(Item{f});
        wstring nm = baseName(f);
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = idx;
        lv.pszText = (LPWSTR)nm.c_str();
        ListView_InsertItem(g_list, &lv);
        setRow(idx);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_queue.push_back({idx, f, cfg});
        }
        added++;
    }
    if (!added) { SetWindowTextW(g_status, L"Tidak ada file .dwg / .dxf."); return; }
    g_cv.notify_one();
    showProgress();
}

static void pickFiles() {
    vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = g_wnd;
    o.lpstrFilter = L"Gambar DWG / DXF (*.dwg;*.dxf)\0*.dwg;*.dxf\0Semua file (*.*)\0*.*\0";
    o.lpstrFile = buf.data();
    o.nMaxFile = (DWORD)buf.size();
    o.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&o)) return;
    vector<wstring> parts;  // satu file: path lengkap; banyak: folder, lalu nama-nama file
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) parts.push_back(p);
    if (parts.size() == 1) { addFiles(parts); return; }
    vector<wstring> files;
    for (size_t i = 1; i < parts.size(); i++) files.push_back(joinPath(parts[0], parts[i]));
    addFiles(files);
}

// Satu file dipilih: dialog Simpan. Beberapa: file pertama lewat dialog, sisanya ke folder yang sama.
static void saveSelected() {
    vector<int> sel;
    for (int i : selectedRows()) if (g_items[i].res) sel.push_back(i);
    if (sel.empty()) return;
    auto cur = std::find(sel.begin(), sel.end(), g_cur);
    if (cur != sel.end()) std::iter_swap(sel.begin(), cur);  // yang sedang tampil jadi nama di dialog
    const Result& r0 = *g_items[sel[0]].res;
    wstring ext = wide(r0.ext), dir0 = dirName(absPath(g_items[sel[0]].path)), filter;
    for (const wstring& part : {L"G-code (*" + ext + L")", L"*" + ext, wstring(L"Semua file (*.*)"), wstring(L"*.*")}) filter += part + L'\0';
    vector<wchar_t> buf(32768, 0);
    wstring name = r0.st.name + ext;
    std::copy(name.begin(), name.end(), buf.begin());
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = g_wnd;
    o.lpstrFilter = filter.c_str();
    o.lpstrFile = buf.data();
    o.nMaxFile = (DWORD)buf.size();
    o.lpstrInitialDir = dir0.c_str();
    o.lpstrDefExt = ext.size() > 1 ? ext.c_str() + 1 : nullptr;
    o.lpstrTitle = sel.size() > 1 ? L"Simpan .TXT (file lain ikut disimpan ke folder ini)" : L"Simpan .TXT";
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&o)) return;
    wstring first = buf.data(), dir = dirName(first);
    vector<std::pair<wstring, int>> outs{{first, sel[0]}};
    int exist = 0;
    for (size_t k = 1; k < sel.size(); k++) {
        const Result& r = *g_items[sel[k]].res;
        outs.push_back({joinPath(dir, r.st.name + wide(r.ext)), sel[k]});
        exist += fileSize(outs.back().first) >= 0;
    }
    if (exist && MessageBoxW(g_wnd, wide(fmt("%d file lain sudah ada di folder itu. Timpa?", exist)).c_str(), L"Simpan", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    int ok = 0;
    string err;
    for (auto& [p, i] : outs) {
        try {
            writeFile(p, gcodeText(g_items[i].res->lines));
            ok++;
        } catch (const std::exception& e) {
            err = e.what();
        }
    }
    SetWindowTextW(g_status, (outs.size() == 1 && ok ? L"Tersimpan: " + first : wide(fmt("%d file tersimpan di ", ok)) + dir).c_str());
    if (!err.empty()) MessageBoxW(g_wnd, wide(err).c_str(), L"Gagal menyimpan", MB_OK | MB_ICONERROR);
}

static void drawDropZone(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    FillRect(dc, &rc, GetSysColorBrush((di->itemState & ODS_SELECTED) ? COLOR_BTNFACE : COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    HPEN pen = CreatePen(PS_DASH, 1, RGB(232, 135, 58));
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left + S(2), rc.top + S(2), rc.right - S(2), rc.bottom - S(2));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    wchar_t t[256];
    GetWindowTextW(di->hwndItem, t, 256);
    HGDIOBJ of = SelectObject(dc, g_font);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT tr = rc;
    DrawTextW(dc, t, -1, &tr, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    int th = tr.bottom - tr.top;
    tr = rc;
    tr.top += (rc.bottom - rc.top - th) / 2;
    DrawTextW(dc, t, -1, &tr, DT_CENTER | DT_WORDBREAK);
    SelectObject(dc, of);
    if (di->itemState & ODS_FOCUS) {
        RECT fr = rc;
        InflateRect(&fr, -S(5), -S(5));
        DrawFocusRect(dc, &fr);
    }
}

// Tampilan 2D: scroll = zoom di titik kursor, seret = geser, klik dua kali = pas layar
static LRESULT CALLBACK previewProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (viewKey(wp)) return 0;
        break;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        g_drag = {(short)LOWORD(lp), (short)HIWORD(lp)};
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == h && g_view.s > 0) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            g_view.cx -= (x - g_drag.x) / g_view.s;
            g_view.cy += (y - g_drag.y) / g_view.s;
            g_drag = {x, y};
            stopFollow();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK:
        g_view = View2D{};
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        if (g_view.s <= 0) return 0;
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(h, &pt);
        RECT rc;
        GetClientRect(h, &rc);
        Gdiplus::RectF a = plotArea((Gdiplus::REAL)rc.right, (Gdiplus::REAL)rc.bottom, g_dpi / 96.0f, false);
        double mx = a.X + a.Width / 2, my = a.Y + a.Height / 2;
        double wx = g_view.cx + (pt.x - mx) / g_view.s, wy = g_view.cy - (pt.y - my) / g_view.s;  // titik di bawah kursor tetap
        g_view.s *= std::pow(1.2, GET_WHEEL_DELTA_WPARAM(wp) / 120.0);
        g_view.cx = wx - (pt.x - mx) / g_view.s, g_view.cy = wy + (pt.y - my) / g_view.s;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            using namespace Gdiplus;
            Bitmap buf(rc.right, rc.bottom, PixelFormat32bppARGB);  // digambar di buffer dulu: tidak berkedip
            buf.SetResolution((REAL)g_dpi, (REAL)g_dpi);
            {
                Graphics g(&buf);
                if (g_simFor >= 0) {
                    RectF a = plotArea((REAL)rc.right, (REAL)rc.bottom, g_dpi / 96.0f, false);
                    if (g_view.s <= 0 && a.Width > 10 && a.Height > 10) g_view = fitView(g_sim.box, a);
                    if (g_follow && g_t < g_sim.total) {
                        P p = torchAt(g_sim, g_t).p;
                        g_view.cx = p.x, g_view.cy = p.y;
                    }
                    drawPreview(g, (REAL)rc.right, (REAL)rc.bottom, *g_items[g_simFor].res, g_sim, g_view, g_t, false);
                } else {
                    g.Clear(Color(255, 255, 255, 255));
                    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
                    FontFamily ff(L"Segoe UI");
                    Font font(&ff, 10);
                    SolidBrush grey(Color(255, 130, 130, 130));
                    StringFormat c;
                    c.SetAlignment(StringAlignmentCenter);
                    c.SetLineAlignment(StringAlignmentCenter);
                    const Item* it = g_cur >= 0 ? &g_items[g_cur] : nullptr;
                    const wchar_t* msg = !it ? L"Animasi jalur potong muncul di sini" : it->state < 2 ? L"Memproses…" : L"Tidak ada jalur potong (lihat pesan di atas)";
                    g.DrawString(msg, -1, &font, RectF(0, 0, (REAL)rc.right, (REAL)rc.bottom), &c, &grey);
                }
            }
            Graphics(dc).DrawImage(&buf, 0, 0, rc.right, rc.bottom);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

// Tampilan 3D: seret kiri = putar kamera, seret kanan = geser, scroll = zoom, klik dua kali = reset
static LRESULT CALLBACK glProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        g_glrc = initGL(GetDC(h), false, g_font);  // CS_OWNDC: DC milik jendela ini selamanya
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (viewKey(wp)) return 0;
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        g_drag = {(short)LOWORD(lp), (short)HIWORD(lp)};
        g_dragBtn = m == WM_LBUTTONDOWN ? 1 : 2;
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp), dx = x - g_drag.x, dy = y - g_drag.y;
            g_drag = {x, y};
            if (g_dragBtn == 1) {
                g_cam.yaw -= dx * 0.4;
                g_cam.pitch = std::clamp(g_cam.pitch + dy * 0.4, 5.0, 90.0);
            } else {  // geser di bidang plat, searah layar
                double k = g_cam.dist * 0.0015, ya = g_cam.yaw * DEG;
                g_cam.tx += (-std::cos(ya) * dx - std::sin(ya) * dy) * k;
                g_cam.ty += (-std::sin(ya) * dx + std::cos(ya) * dy) * k;
                stopFollow();
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK:
        resetCam(g_cam, g_scene);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        g_cam.dist = std::clamp(g_cam.dist * std::pow(0.85, GET_WHEEL_DELTA_WPARAM(wp) / 120.0), g_scene.size * 0.02, g_scene.size * 10);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        if (!g_glrc) return 0;
        RECT rc;
        GetClientRect(h, &rc);
        HDC dc = GetDC(h);
        wglMakeCurrent(dc, g_glrc);
        if (g_simFor >= 0) {
            Cam3D cam = g_cam;
            if (g_follow) {
                P p = torchAt(g_sim, g_t).p;
                cam.tx = p.x, cam.ty = p.y;
            }
            render3D(rc.right, rc.bottom, g_sim, g_scene, cam, g_t, HINT_3D);
        } else {
            glClearColor(0.80f, 0.84f, 0.87f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        SwapBuffers(dc);
        ReleaseDC(h, dc);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int W = rc.right, H = rc.bottom, p = S(10), lw = S(400), dh = S(100), sh = S(84), bh = S(32), th = S(28);
    int x = 2 * p + lw, rw = std::max(0, W - x - p), by = H - p - bh, ty = p + sh + S(4), py = ty + th + S(6);
    MoveWindow(g_drop, p, p, lw, dh, TRUE);
    MoveWindow(g_list, p, 2 * p + dh, lw, std::max(0, H - dh - 3 * p), TRUE);
    ListView_SetColumnWidth(g_list, 2, std::max(S(80), lw - S(230) - GetSystemMetrics(SM_CXVSCROLL) - S(6)));
    MoveWindow(g_stats, x, p, rw, sh, TRUE);
    // baris animasi: [Putar] [slider waktu] [waktu] [kecepatan] [2D][3D] [Ikuti torch]
    int xr = x + rw;
    MoveWindow(g_followBox, xr - S(104), ty + S(3), S(104), th - S(6), TRUE);
    xr -= S(112);
    MoveWindow(g_b3d, xr - S(40), ty, S(40), th, TRUE);
    MoveWindow(g_b2d, xr - S(80), ty, S(40), th, TRUE);
    xr -= S(88);
    MoveWindow(g_speedBox, xr - S(70), ty + S(2), S(70), S(220), TRUE);
    xr -= S(78);
    MoveWindow(g_time, xr - S(220), ty + S(6), S(220), S(20), TRUE);
    xr -= S(226);
    MoveWindow(g_play, x, ty, S(92), th, TRUE);
    MoveWindow(g_track, x + S(98), ty + S(2), std::max(S(40), xr - x - S(98)), th - S(4), TRUE);
    for (HWND v : {g_prev, g_gl}) MoveWindow(v, x, py, rw, std::max(0, by - S(8) - py), TRUE);
    MoveWindow(g_save, x, by, S(180), bh, TRUE);
    MoveWindow(g_status, x + S(192), by + S(8), std::max(0, rw - S(192)), S(20), TRUE);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = h;
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id, hi, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
            return c;
        };
        g_drop = child(L"BUTTON", L"Seret file DWG / DXF ke sini\n(boleh banyak sekaligus, atau folder)\n\natau klik untuk memilih file",
                       WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, ID_DROP);
        g_list = child(WC_LISTVIEWW, L"Daftar hasil", WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, ID_LIST, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* cols[] = {L"File", L"Status", L"Keterangan"};
        int widths[] = {S(150), S(80), S(160)};
        for (int i = 0; i < 3; i++) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (LPWSTR)cols[i];
            c.cx = widths[i];
            ListView_InsertColumn(g_list, i, &c);
        }
        g_stats = child(L"STATIC", L"", WS_VISIBLE | SS_NOPREFIX, ID_STATS);
        g_play = child(L"BUTTON", L"► Putar", WS_VISIBLE | WS_TABSTOP | WS_DISABLED | BS_PUSHBUTTON, ID_PLAY);
        g_track = child(TRACKBAR_CLASSW, L"Waktu animasi", WS_VISIBLE | WS_TABSTOP | WS_DISABLED | TBS_HORZ | TBS_NOTICKS, ID_TRACK);
        SendMessageW(g_track, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
        g_time = child(L"STATIC", L"", WS_VISIBLE | SS_NOPREFIX | SS_RIGHT, ID_TIME);
        g_speedBox = child(WC_COMBOBOXW, L"Kecepatan animasi", WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, ID_SPEED);
        for (double sp : SPEEDS) SendMessageW(g_speedBox, CB_ADDSTRING, 0, (LPARAM)wide(fmt("%g×", sp)).c_str());
        SendMessageW(g_speedBox, CB_SETCURSEL, 3, 0);  // 10×
        g_b2d = child(L"BUTTON", L"2D", WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, ID_2D);
        g_b3d = child(L"BUTTON", L"3D", WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE, ID_3D);
        g_followBox = child(L"BUTTON", L"Ikuti torch", WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX, ID_FOLLOW);
        g_prev = child(L"Dwg2CncPreview", L"Animasi jalur potong 2D", WS_VISIBLE | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
        g_gl = child(L"Dwg2CncGL", L"Simulasi potong 3D", WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
        EnableWindow(g_b3d, g_glrc != nullptr);  // tanpa OpenGL: 2D saja
        g_save = child(L"BUTTON", L"Simpan .TXT", WS_VISIBLE | WS_TABSTOP | WS_DISABLED | BS_PUSHBUTTON, ID_SAVE);
        g_status = child(L"STATIC", L"", WS_VISIBLE | SS_NOPREFIX | SS_PATHELLIPSIS, ID_STATUS);
        setMode(false);
        SetTimer(h, 1, 15, nullptr);  // detak animasi
        showCurrent();
        return 0;
    }
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {S(1100), S(600)};
        return 0;
    case WM_TIMER: {
        LARGE_INTEGER now, fq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&fq);
        double dt = std::min(0.1, double(now.QuadPart - g_tick.QuadPart) / fq.QuadPart);
        g_tick = now;
        if (g_simFor < 0 || (!g_playing && g_sparks.empty())) return 0;
        if (g_playing) {
            g_t = std::min(g_sim.total, g_t + dt * g_speed);
            g_playing = g_t < g_sim.total;
            updateTimeUi();
        }
        if (g_mode3d) updateSparks(g_scene, torchAt(g_sim, g_t), dt, g_playing);
        else g_sparks.clear();
        redrawView();
        return 0;
    }
    case WM_HSCROLL:  // slider waktu digeser
        if ((HWND)lp == g_track && g_simFor >= 0) {
            g_t = SendMessageW(g_track, TBM_GETPOS, 0, 0) / 1000.0 * g_sim.total;
            g_sparks.clear();
            updateTimeUi();
            redrawView();
        }
        return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        vector<wstring> in;
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wstring p(DragQueryFileW(d, i, nullptr, 0), 0);
            DragQueryFileW(d, i, &p[0], (UINT)p.size() + 1);
            in.push_back(p);
        }
        DragFinish(d);
        addFiles(in);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == CBN_SELCHANGE && id == ID_SPEED) {
            g_speed = SPEEDS[std::clamp((int)SendMessageW(g_speedBox, CB_GETCURSEL, 0, 0), 0, 6)];
        } else if (code == BN_CLICKED) {
            if (id == ID_DROP) pickFiles();
            else if (id == ID_SAVE) saveSelected();
            else if (id == ID_PLAY) setPlaying(!g_playing);
            else if (id == ID_2D || id == ID_3D) setMode(id == ID_3D);
            else if (id == ID_FOLLOW) {
                g_follow = SendMessageW(g_followBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (g_follow) {  // dekatkan pandangan supaya torch terlihat jelas
                    if (g_view.s > 0) g_view.s *= 3;
                    g_cam.dist = std::min(g_cam.dist, g_scene.size * 0.4);
                }
                redrawView();
            }
        }
        return 0;
    }
    case WM_DRAWITEM:
        if (wp != ID_DROP) break;
        drawDropZone((DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->hwndFrom != g_list) break;
        if (nm->code == LVN_ITEMCHANGED && (((NMLISTVIEW*)lp)->uChanged & LVIF_STATE)) {
            showCurrent();
        } else if (nm->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)lp)->wVKey == 'A' && GetKeyState(VK_CONTROL) < 0) {
            ListView_SetItemState(g_list, -1, LVIS_SELECTED, LVIS_SELECTED);  // Ctrl+A
        } else if (nm->code == NM_CUSTOMDRAW) {  // warna kolom Status
            auto* cd = (NMLVCUSTOMDRAW*)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
            if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) && cd->nmcd.dwItemSpec < g_items.size()) {
                const Item& it = g_items[cd->nmcd.dwItemSpec];
                cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
                if (cd->iSubItem == 1 && it.state == 2)
                    cd->clrText = !it.res ? RGB(200, 30, 30) : it.res->errors.empty() ? RGB(20, 130, 60) : RGB(200, 110, 0);
                return CDRF_NEWFONT;
            }
        }
        break;
    }
    case WM_APP_START:
        g_items[wp].state = 1;
        setRow((int)wp);
        if ((int)wp == g_cur) showCurrent();
        return 0;
    case WM_APP_DONE: {
        std::unique_ptr<Done> d((Done*)lp);
        Item& it = g_items[d->idx];
        it.state = 2, it.res = std::move(d->res), it.err = d->err;
        setRow(d->idx);
        showProgress();
        if (ListView_GetSelectedCount(g_list) == 0) {  // belum ada pilihan: tampilkan hasil pertama
            ListView_SetItemState(g_list, d->idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, d->idx, FALSE);
        } else if (ListView_GetItemState(g_list, d->idx, LVIS_SELECTED)) {
            showCurrent();
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// Uji tanpa layar: render 2D + 3D di 45% waktu animasi ke PNG.
//     dwg2cnc-gui.exe --snapshot FOLDER file.dwg ...   -> FOLDER\NAMA_2d.png, NAMA_3d.png
static int snapshot(const wstring& outdir, const vector<wstring>& files) {
    const int W = 1000, H = 640;
    wstring dir = absPath(outdir);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    int bad = 0;
    for (auto& f : collectFiles(files)) {
        try {
            Result r = convert(f, loadConfig(L"", {}));
            Sim s = buildSim(r);
            double t = s.total * 0.45;
            wstring base = joinPath(dir, r.st.name);
            Gdiplus::Bitmap b2(W, H, PixelFormat24bppRGB);
            b2.SetResolution(96, 96);
            {
                Gdiplus::Graphics g(&b2);
                drawPreview(g, W, H, r, s, View2D{}, t, false);
            }
            bad += b2.Save((base + L"_2d.png").c_str(), &PNG_ENCODER, nullptr) != Gdiplus::Ok;
            BITMAPINFO bi{};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, H, 1, 32, BI_RGB};
            void* bits = nullptr;
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(dc, hb);
            if (HGLRC rc = initGL(dc, true, g_font)) {
                Scene sc = sceneOf(s, r);
                Cam3D cam;
                resetCam(cam, sc);
                g_sparks.clear();
                for (int i = 0; i < 15; i++) updateSparks(sc, torchAt(s, t), 0.03, true);
                render3D(W, H, s, sc, cam, t, HINT_3D);
                glFinish();
                Gdiplus::Bitmap b3(W, H, -W * 4, PixelFormat32bppRGB, (BYTE*)bits + (H - 1) * W * 4);  // DIB bawah-ke-atas
                bad += b3.Save((base + L"_3d.png").c_str(), &PNG_ENCODER, nullptr) != Gdiplus::Ok;
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(rc);
            } else {
                bad++;
            }
            SelectObject(dc, old);
            DeleteObject(hb);
            DeleteDC(dc);
        } catch (const std::exception&) {
            bad++;
        }
    }
    return bad;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    initExeDir();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    ULONG_PTR gdip = 0;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdip, &gsi, nullptr);
    HDC sdc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    vector<wstring> args(argv + 1, argv + argc);  // file yang diseret ke ikon exe
    LocalFree(argv);
    if (args.size() >= 2 && args[0] == L"--snapshot") return snapshot(args[1], vector<wstring>(args.begin() + 2, args.end()));

    WNDCLASSW wc{};
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = previewProc;
    wc.lpszClassName = L"Dwg2CncPreview";
    RegisterClassW(&wc);
    wc.style = CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = glProc;
    wc.lpszClassName = L"Dwg2CncGL";
    RegisterClassW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = wndProc;
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));  // dwg2cnc.ico (lihat dwg2cnc.rc)
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Dwg2CncMain";
    RegisterClassW(&wc);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, L"Dwg2CncMain", L"DWG ke G-code F2100B", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                    std::min(S(1280), (int)(work.right - work.left)), std::min(S(820), (int)(work.bottom - work.top)), nullptr, nullptr, hi, nullptr);
    SendMessageW(g_wnd, WM_SETICON, ICON_SMALL,  // ikon kecil di judul jendela: ukuran pas, bukan hasil kecilkan 32 px
                 (LPARAM)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    std::thread(worker).detach();
    ShowWindow(g_wnd, show);
    if (!args.empty()) addFiles(args);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    ExitProcess(0);  // thread konversi mungkin masih menunggu dwg2dxf; tidak perlu ditunggu
}
