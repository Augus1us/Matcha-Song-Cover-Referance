// Minimal SVG <path d="..."> renderer for the player icons.
//
// The icons are authored as real SVGs (Bootstrap Icons, MIT, plus one custom
// bubble) kept in assets/icons/. Their shapes rely on small arcs to round every
// corner -- a Bootstrap play triangle has three ROUNDED tips, not three points.
// Reimplementing them as hand-placed triangles is what made the transport row
// look spiky next to the real player, so the path data is parsed and flattened
// here rather than approximated.
//
// Supported: M m L l H h V v C c S s Q q T t A a Z z -- everything the icon set
// uses, plus the shorthand forms so hand-edited files keep working.

#include "music_player_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace native_music_player::detail {

namespace {

struct Cursor { const char* p; const char* end; };

void SkipSep(Cursor& c) {
    while (c.p < c.end && (*c.p == ' ' || *c.p == ',' || *c.p == '\t' ||
                           *c.p == '\n' || *c.p == '\r'))
        ++c.p;
}

// SVG numbers can run together with no separator: "1.5.5" is 1.5 then .5, and
// "3-4" is 3 then -4. strtod alone would swallow the following number's leading
// sign or dot, so the extent is scanned explicitly first.
bool ReadNumber(Cursor& c, float& out) {
    SkipSep(c);
    if (c.p >= c.end) return false;
    const char* start = c.p;
    if (*c.p == '+' || *c.p == '-') ++c.p;
    bool sawDot = false, sawDigit = false;
    while (c.p < c.end) {
        if (*c.p >= '0' && *c.p <= '9') { sawDigit = true; ++c.p; }
        else if (*c.p == '.' && !sawDot) { sawDot = true; ++c.p; }
        else if ((*c.p == 'e' || *c.p == 'E') && sawDigit) {
            ++c.p;
            if (c.p < c.end && (*c.p == '+' || *c.p == '-')) ++c.p;
        } else break;
    }
    if (!sawDigit) { c.p = start; return false; }
    out = (float)std::strtod(std::string(start, (size_t)(c.p - start)).c_str(), nullptr);
    return true;
}

bool ReadFlag(Cursor& c, bool& out) {
    SkipSep(c);
    if (c.p >= c.end) return false;
    if (*c.p != '0' && *c.p != '1') return false;
    out = (*c.p == '1');
    ++c.p;
    return true;
}

constexpr int kCurveSegments = 14;

void Cubic(std::vector<ImVec2>& pts, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3) {
    for (int i = 1; i <= kCurveSegments; ++i) {
        const float t = (float)i / kCurveSegments, u = 1.f - t;
        const float a = u * u * u, b = 3 * u * u * t;
        const float cc = 3 * u * t * t, d = t * t * t;
        pts.push_back(ImVec2(a * p0.x + b * p1.x + cc * p2.x + d * p3.x,
                             a * p0.y + b * p1.y + cc * p2.y + d * p3.y));
    }
}

// Endpoint-parameterised arc -> centre form, per the SVG spec implementation
// notes. Every rounded corner in these icons is one of these.
void Arc(std::vector<ImVec2>& pts, ImVec2 p0, float rx, float ry,
         float rotDeg, bool largeArc, bool sweep, ImVec2 p1) {
    if (rx == 0.f || ry == 0.f) { pts.push_back(p1); return; }
    rx = std::fabs(rx); ry = std::fabs(ry);
    const float rad = rotDeg * 3.14159265358979f / 180.f;
    const float cosR = std::cos(rad), sinR = std::sin(rad);
    const float dx2 = (p0.x - p1.x) * 0.5f, dy2 = (p0.y - p1.y) * 0.5f;
    const float x1 = cosR * dx2 + sinR * dy2;
    const float y1 = -sinR * dx2 + cosR * dy2;
    float rxs = rx * rx, rys = ry * ry;
    const float x1s = x1 * x1, y1s = y1 * y1;
    const float lambda = x1s / rxs + y1s / rys;
    if (lambda > 1.f) {                    // radii too small to span the ends
        const float s = std::sqrt(lambda);
        rx *= s; ry *= s; rxs = rx * rx; rys = ry * ry;
    }
    const float sign = (largeArc != sweep) ? 1.f : -1.f;
    float num = rxs * rys - rxs * y1s - rys * x1s;
    if (num < 0.f) num = 0.f;
    const float den = rxs * y1s + rys * x1s;
    const float coef = den > 0.f ? sign * std::sqrt(num / den) : 0.f;
    const float cx1 = coef * (rx * y1 / ry);
    const float cy1 = coef * -(ry * x1 / rx);
    const float cx = cosR * cx1 - sinR * cy1 + (p0.x + p1.x) * 0.5f;
    const float cy = sinR * cx1 + cosR * cy1 + (p0.y + p1.y) * 0.5f;
    auto angle = [](float ux, float uy, float vx, float vy) {
        const float dot = ux * vx + uy * vy;
        const float len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        float a = len > 0.f
            ? std::acos(std::fmax(-1.f, std::fmin(1.f, dot / len))) : 0.f;
        if (ux * vy - uy * vx < 0.f) a = -a;
        return a;
    };
    const float ux = (x1 - cx1) / rx, uy = (y1 - cy1) / ry;
    const float vx = (-x1 - cx1) / rx, vy = (-y1 - cy1) / ry;
    const float theta = angle(1.f, 0.f, ux, uy);
    float delta = angle(ux, uy, vx, vy);
    const float twoPi = 6.28318530717959f;
    if (!sweep && delta > 0.f) delta -= twoPi;
    else if (sweep && delta < 0.f) delta += twoPi;
    const int steps = std::max(3, (int)(std::fabs(delta) / 0.25f) + 2);
    for (int i = 1; i <= steps; ++i) {
        const float t = theta + delta * ((float)i / steps);
        const float ct = std::cos(t), st = std::sin(t);
        pts.push_back(ImVec2(cosR * rx * ct - sinR * ry * st + cx,
                             sinR * rx * ct + cosR * ry * st + cy));
    }
}

}  // namespace

void FlattenSvgPath(const char* d, std::vector<std::vector<ImVec2>>& outSubpaths) {
    outSubpaths.clear();
    if (!d || !*d) return;
    Cursor c{d, d + std::strlen(d)};
    std::vector<ImVec2> cur;
    ImVec2 pos(0, 0), startPt(0, 0), lastCtrl(0, 0);
    char cmd = 0, prevCmd = 0;
    auto flush = [&]() {
        if (cur.size() > 2) outSubpaths.push_back(cur);
        cur.clear();
    };
    while (true) {
        SkipSep(c);
        if (c.p >= c.end) break;
        if (std::isalpha((unsigned char)*c.p)) { cmd = *c.p; ++c.p; }
        else if (cmd == 'M') cmd = 'L';       // extra pairs after M are lineto
        else if (cmd == 'm') cmd = 'l';
        const bool rel = std::islower((unsigned char)cmd) != 0;
        const char up = (char)std::toupper((unsigned char)cmd);
        float a, b, e, f, g, h;
        if (up == 'M') {
            if (!ReadNumber(c, a) || !ReadNumber(c, b)) break;
            flush();
            pos = rel ? ImVec2(pos.x + a, pos.y + b) : ImVec2(a, b);
            startPt = pos; cur.push_back(pos);
        } else if (up == 'L') {
            if (!ReadNumber(c, a) || !ReadNumber(c, b)) break;
            pos = rel ? ImVec2(pos.x + a, pos.y + b) : ImVec2(a, b);
            cur.push_back(pos);
        } else if (up == 'H') {
            if (!ReadNumber(c, a)) break;
            pos = ImVec2(rel ? pos.x + a : a, pos.y);
            cur.push_back(pos);
        } else if (up == 'V') {
            if (!ReadNumber(c, a)) break;
            pos = ImVec2(pos.x, rel ? pos.y + a : a);
            cur.push_back(pos);
        } else if (up == 'C' || up == 'S') {
            ImVec2 c1, c2, p3;
            if (up == 'C') {
                if (!ReadNumber(c, a) || !ReadNumber(c, b) || !ReadNumber(c, e) ||
                    !ReadNumber(c, f) || !ReadNumber(c, g) || !ReadNumber(c, h)) break;
                c1 = rel ? ImVec2(pos.x + a, pos.y + b) : ImVec2(a, b);
                c2 = rel ? ImVec2(pos.x + e, pos.y + f) : ImVec2(e, f);
                p3 = rel ? ImVec2(pos.x + g, pos.y + h) : ImVec2(g, h);
            } else {
                if (!ReadNumber(c, e) || !ReadNumber(c, f) ||
                    !ReadNumber(c, g) || !ReadNumber(c, h)) break;
                const char pu = (char)std::toupper((unsigned char)prevCmd);
                c1 = (pu == 'C' || pu == 'S')
                    ? ImVec2(2 * pos.x - lastCtrl.x, 2 * pos.y - lastCtrl.y) : pos;
                c2 = rel ? ImVec2(pos.x + e, pos.y + f) : ImVec2(e, f);
                p3 = rel ? ImVec2(pos.x + g, pos.y + h) : ImVec2(g, h);
            }
            if (cur.empty()) cur.push_back(pos);
            Cubic(cur, pos, c1, c2, p3);
            lastCtrl = c2; pos = p3;
        } else if (up == 'Q' || up == 'T') {
            ImVec2 q, p2;
            if (up == 'Q') {
                if (!ReadNumber(c, a) || !ReadNumber(c, b) ||
                    !ReadNumber(c, e) || !ReadNumber(c, f)) break;
                q = rel ? ImVec2(pos.x + a, pos.y + b) : ImVec2(a, b);
                p2 = rel ? ImVec2(pos.x + e, pos.y + f) : ImVec2(e, f);
            } else {
                if (!ReadNumber(c, e) || !ReadNumber(c, f)) break;
                const char pu = (char)std::toupper((unsigned char)prevCmd);
                q = (pu == 'Q' || pu == 'T')
                    ? ImVec2(2 * pos.x - lastCtrl.x, 2 * pos.y - lastCtrl.y) : pos;
                p2 = rel ? ImVec2(pos.x + e, pos.y + f) : ImVec2(e, f);
            }
            // Quadratic -> cubic so one flattener covers both.
            const ImVec2 c1(pos.x + 2.f / 3.f * (q.x - pos.x),
                            pos.y + 2.f / 3.f * (q.y - pos.y));
            const ImVec2 c2(p2.x + 2.f / 3.f * (q.x - p2.x),
                            p2.y + 2.f / 3.f * (q.y - p2.y));
            if (cur.empty()) cur.push_back(pos);
            Cubic(cur, pos, c1, c2, p2);
            lastCtrl = q; pos = p2;
        } else if (up == 'A') {
            float rx, ry, rot; bool large, sweep;
            if (!ReadNumber(c, rx) || !ReadNumber(c, ry) || !ReadNumber(c, rot) ||
                !ReadFlag(c, large) || !ReadFlag(c, sweep) ||
                !ReadNumber(c, g) || !ReadNumber(c, h)) break;
            const ImVec2 p1 = rel ? ImVec2(pos.x + g, pos.y + h) : ImVec2(g, h);
            if (cur.empty()) cur.push_back(pos);
            Arc(cur, pos, rx, ry, rot, large, sweep, p1);
            pos = p1;
        } else if (up == 'Z') {
            if (!cur.empty()) { cur.push_back(startPt); flush(); }
            pos = startPt;
        } else {
            break;    // unknown command: stop rather than emit garbage
        }
        prevCmd = cmd;
    }
    flush();
}

namespace {

// ImGui's concave fill (ImTriangulator) classifies a corner as reflex unless
// ImTriangleIsClockwise(prev, cur, next), so it only triangulates CLOCKWISE
// polygons. Hand a counter-clockwise outline to it and every vertex looks
// reflex, no ear is ever found, and the glyph collapses to its bounding box --
// which is why the skip marks first filled as solid rectangles.
//
// SVG has no winding guarantee, so the winding is normalised here instead.
// In screen space (Y down) a positive shoelace sum IS clockwise.
float SignedArea(const std::vector<ImVec2>& p) {
    float a = 0.f;
    for (size_t i = 0, n = p.size(); i < n; ++i) {
        const ImVec2& u = p[i];
        const ImVec2& v = p[(i + 1) % n];
        a += u.x * v.y - v.x * u.y;
    }
    return a * 0.5f;
}

}  // namespace

void DrawSvgIcon(ImDrawList* dl, const char* pathData, ImVec2 center,
                 float size, float viewBox, ImU32 col) {
    static std::vector<std::vector<ImVec2>> subpaths;
    FlattenSvgPath(pathData, subpaths);
    const float k = size / viewBox;
    const float half = viewBox * 0.5f;
    std::vector<ImVec2> poly;
    for (const std::vector<ImVec2>& sp : subpaths) {
        if (sp.size() < 3) continue;
        poly.clear();
        poly.reserve(sp.size());
        for (size_t i = 0; i < sp.size(); ++i)
            poly.push_back(ImVec2(center.x + (sp[i].x - half) * k,
                                  center.y + (sp[i].y - half) * k));
        // Drop a duplicated closing vertex: it makes one corner degenerate and
        // the triangulator can stall on it.
        if (poly.size() > 1) {
            const float dx = poly.front().x - poly.back().x;
            const float dy = poly.front().y - poly.back().y;
            if (dx * dx + dy * dy < 1e-6f) poly.pop_back();
        }
        if (poly.size() < 3) continue;
        if (SignedArea(poly) < 0.f)
            std::reverse(poly.begin(), poly.end());   // -> clockwise
        // One fill with one outer fringe. Filling triangle-by-triangle instead
        // leaves an AA seam on every shared edge, which at icon size reads as
        // the glyph being shredded.
        dl->AddConcavePolyFilled(poly.data(), (int)poly.size(), col);
    }
}

void StrokeSvgPath(ImDrawList* dl, const char* pathData, ImVec2 center,
                   float size, float viewBox, ImU32 col, float thickness) {
    static std::vector<std::vector<ImVec2>> subpaths;
    FlattenSvgPath(pathData, subpaths);
    const float k = size / viewBox;
    const float half = viewBox * 0.5f;
    for (const std::vector<ImVec2>& sp : subpaths) {
        if (sp.size() < 2) continue;
        for (size_t i = 0; i < sp.size(); ++i)
            dl->PathLineTo(ImVec2(center.x + (sp[i].x - half) * k,
                                  center.y + (sp[i].y - half) * k));
        dl->PathStroke(col, 0, thickness);
    }
}

}  // namespace native_music_player::detail
