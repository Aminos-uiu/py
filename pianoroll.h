// pianoroll.h - MiniDAW piano roll: edits the notes of one MIDI clip (its own window)
//
// Included by daw_gui.cpp after sampler.h. Open it by double-clicking a MIDI clip (or an empty spot of a
// MIDI track's lane, which makes a new clip). Edits go straight into the clip, so they are heard at once
// and can be undone with Ctrl+Z (one undo step per mouse gesture / key press).
//
// Mouse   click empty: add a note (drag to place it) · drag a note: move (time + pitch) · drag its right
//         edge: length · right-click / right-drag: delete · Ctrl+drag (Shift+drag adds): select with a box
//         · Shift+click: add to / remove from the selection · velocity lane: drag the bars
//         · ruler: click to move the playhead, drag the orange handle to change the clip length
//         · keys on the left: listen · Alt while dragging: no snapping to the grid
// Keys    Del delete · Ctrl+A all · Ctrl+D duplicate · Q quantize · Up/Down transpose (Shift: octave)
//         · Left/Right move by a grid step · Esc deselect · Space play/stop · Ctrl+Z undo
// Wheel   scroll pitch · Shift+wheel scroll time · Ctrl+wheel zoom

#pragma once

HWND gPrWnd = NULL;
int gPrUid = 0;                    // the clip being edited (Clip::uid)
float gPW = 1100, gPH = 640;       // client size (logical pixels)
double gPrView = 0, gPrPx = 160;   // data time (s) at the left edge of the notes, pixels per second
float gPrScroll = 0;               // vertical scroll (pixels)
int gPrDiv = 16;                   // grid: 1/gPrDiv note, 0 = off
i64 gPrLastLen = 0;                // length of new notes (0 = one grid step)
int gPrLastVel = 100;              // velocity of new notes

struct PrNote { Note n; bool sel; };
std::vector<PrNote> gPrNotes, gPrOrig;   // working copy of the clip's notes (with selection); copy at drag start
MidiPtr gPrLoaded;                       // the clip data gPrNotes matches
std::wstring gPrMsg, gPrTitle;
DWORD gPrMsgTime = 0;

const float PR_TOP = 48, PR_RULER = 22, PR_KEYS = 64, PR_VEL = 84, PR_STATUS = 22, PR_ROW = 12;
float prNoteTop() { return PR_TOP + PR_RULER; }
float prNoteBottom() { return gPH - PR_STATUS - PR_VEL - 4; }
float prVelTop() { return gPH - PR_STATUS - PR_VEL; }
float prVelBottom() { return gPH - PR_STATUS; }

const int PR_DIVS[5] = {0, 4, 8, 16, 32};
const wchar_t* PR_DIV_NAMES[5] = {L"Off", L"1/4", L"1/8", L"1/16", L"1/32"};
RectF prGridBtn(int i) { return RectF(352 + i * 46.0f, 9, 42, 30); }
const RectF prBtnQuant(590, 9, 78, 30), prBtnSampler(674, 9, 78, 30), prBtnPlay(758, 9, 36, 30);

void prMsg(const std::wstring& m) { gPrMsg = m; gPrMsgTime = GetTickCount(); }

Clip* prClip(int* tiOut = nullptr) {
    for (int ti = 0; ti < (int)gTracks.size(); ++ti)
        for (auto& c : gTracks[ti].clips)
            if (c.uid == gPrUid && c.midi) {
                if (tiOut) *tiOut = ti;
                return &c;
            }
    return nullptr;
}

i64 prBase(const Clip& c) { return c.start - c.off; }   // timeline position of data time 0
double prStepD() { return gPrDiv ? samplesPerBeat() * 4.0 / gPrDiv : 0.0; }
bool prAlt() { return GetKeyState(VK_MENU) < 0; }
i64 prSnap(i64 dt, const Clip& c, bool floorMode) {   // to the grid of the timeline (bars of the project)
    double st = prStepD();
    if (st <= 0) return dt;
    double b = (double)prBase(c), q = (b + dt) / st;
    q = floorMode ? std::floor(q + 1e-9) : std::round(q);
    return (i64)std::llround(q * st - b);
}
i64 prNudgeStep() { double st = prStepD(); return (i64)std::llround(st > 0 ? st : samplesPerBeat() / 4); }

float prX(i64 dt) { return PR_KEYS + (float)(((double)dt / SR - gPrView) * gPrPx); }
i64 prDt(float x) { return (i64)std::llround((gPrView + (x - PR_KEYS) / gPrPx) * SR); }
float prY(int p) { return prNoteTop() + (127 - p) * PR_ROW - gPrScroll; }
int prPitchAt(float y) {
    int p = 127 - (int)std::floor((y - prNoteTop() + gPrScroll) / PR_ROW);
    return p < 0 ? 0 : (p > 127 ? 127 : p);
}

void prClampView() {
    float maxS = std::max(0.0f, 128 * PR_ROW - (prNoteBottom() - prNoteTop()));
    gPrScroll = clampf(gPrScroll, 0, maxS);
    if (gPrView < 0) gPrView = 0;
}

// Reload the notes if the clip changed elsewhere (undo, split, ...). Keeps the selection otherwise.
void prSync() {
    Clip* c = prClip();
    if (!c) { gPrNotes.clear(); gPrLoaded.reset(); return; }
    if (c->midi == gPrLoaded) return;
    gPrLoaded = c->midi;
    gPrNotes.clear();
    for (auto& n : c->midi->notes) gPrNotes.push_back({n, false});
}

bool gPrUndoDone = false;   // one undo step per gesture

// Writes the working notes into the clip (and lengthens the clip, to a whole bar, if notes go past its end)
void prCommit() {
    Clip* c = prClip();
    if (!c) return;
    if (!gPrUndoDone) { pushUndo(); gPrUndoDone = true; }
    auto md = std::make_shared<MidiData>();
    i64 maxEnd = 0;
    for (auto& p : gPrNotes) { md->notes.push_back(p.n); maxEnd = std::max(maxEnd, p.n.start + p.n.len); }
    std::stable_sort(md->notes.begin(), md->notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    i64 len = c->len;
    if (maxEnd > c->off + c->len) {
        double bar = 4 * samplesPerBeat();
        len = (i64)std::llround(std::ceil((prBase(*c) + maxEnd) / bar - 1e-9) * bar) - c->start;
    }
    {
        std::lock_guard<std::mutex> lk(gMx);
        c->midi = md;
        c->len = len;
    }
    gPrLoaded = md;
    gDirty = true;
}

void prSortNotes() {
    std::stable_sort(gPrNotes.begin(), gPrNotes.end(), [](const PrNote& a, const PrNote& b) { return a.n.start < b.n.start; });
}
bool prAnySel() { for (auto& p : gPrNotes) if (p.sel) return true; return false; }
void prSelectAll(bool s) { for (auto& p : gPrNotes) p.sel = s; }

// Listening to a note (mouse): goes to the clip's track instrument
int gPrAudNote = -1;
void prStopAudition() {
    if (gPrAudNote >= 0) { smpNoteOff(gPrAudNote); gPrAudNote = -1; }
}
void prAudition(int pitch, int vel) {
    prStopAudition();
    int ti;
    if (!prClip(&ti)) return;
    if (gTracks[ti].id != gLiveTrackId) setLiveTrack(gTracks[ti].id);
    smpNoteOn(pitch, vel);
    gPrAudNote = pitch;
}

// ============================================================================
// Edit commands
// ============================================================================
void prDeleteSel() {
    size_t before = gPrNotes.size();
    gPrNotes.erase(std::remove_if(gPrNotes.begin(), gPrNotes.end(), [](const PrNote& p) { return p.sel; }), gPrNotes.end());
    if (gPrNotes.size() == before) return;
    gPrUndoDone = false;
    prCommit();
}

void prQuantize() {
    Clip* c = prClip();
    if (!c) return;
    if (prStepD() <= 0) { prMsg(L"Choose a grid first (1/4 ... 1/32), then quantize"); return; }
    bool sel = prAnySel();
    int moved = 0;
    for (auto& p : gPrNotes) {
        if (sel && !p.sel) continue;
        i64 s = std::max<i64>(0, prSnap(p.n.start, *c, false));
        if (s != p.n.start) { p.n.start = s; ++moved; }
    }
    if (!moved) { prMsg(L"Already on the grid"); return; }
    gPrUndoDone = false;
    prCommit();
    prSortNotes();
    prMsg(L"Quantized " + std::to_wstring(moved) + L" notes to " + PR_DIV_NAMES[gPrDiv == 4 ? 1 : gPrDiv == 8 ? 2 : gPrDiv == 16 ? 3 : 4] +
          (sel ? L" (selection)" : L" (all notes)"));
}

void prTranspose(int d) {
    if (!prAnySel()) return;
    for (auto& p : gPrNotes) if (p.sel && (p.n.pitch + d < 0 || p.n.pitch + d > 127)) return;
    for (auto& p : gPrNotes) if (p.sel) p.n.pitch += d;
    gPrUndoDone = false;
    prCommit();
    for (auto& p : gPrNotes) if (p.sel) { prAudition(p.n.pitch, p.n.vel); break; }
}

void prNudge(int dir) {
    if (!prAnySel()) return;
    i64 d = dir * prNudgeStep();
    for (auto& p : gPrNotes) if (p.sel && p.n.start + d < 0) return;
    for (auto& p : gPrNotes) if (p.sel) p.n.start += d;
    gPrUndoDone = false;
    prCommit();
    prSortNotes();
}

void prDuplicate() {
    if (!prAnySel()) { prMsg(L"Select notes to duplicate"); return; }
    i64 lo = LLONG_MAX, hi = 0;
    for (auto& p : gPrNotes) if (p.sel) { lo = std::min(lo, p.n.start); hi = std::max(hi, p.n.start + p.n.len); }
    i64 st = prNudgeStep(), span = std::max<i64>(st, (i64)(std::ceil((double)(hi - lo) / st - 1e-9) * st));
    std::vector<PrNote> add;
    for (auto& p : gPrNotes) if (p.sel) { PrNote q = p; q.n.start += span; add.push_back(q); p.sel = false; }
    gPrNotes.insert(gPrNotes.end(), add.begin(), add.end());
    gPrUndoDone = false;
    prCommit();
    prSortNotes();
}

// ============================================================================
// Drawing
// ============================================================================
int prNoteAt(float x, float y, bool* edge) {   // topmost note under (x, y); edge: on its right end
    for (int i = (int)gPrNotes.size() - 1; i >= 0; --i) {
        const Note& n = gPrNotes[i].n;
        float x0 = prX(n.start), x1 = prX(n.start + n.len), y0 = prY(n.pitch);
        if (y >= y0 && y < y0 + PR_ROW && x >= x0 && x <= x1 + 1) {
            if (edge) *edge = x >= x1 - std::min(6.0f, (x1 - x0) / 3);
            return i;
        }
    }
    return -1;
}

const wchar_t* prPadLabel(const Instrument* I, int pitch) {   // drum kit: name of the pad on this note
    if (!I || I->kind != INST_SAMPLER || I->p.mode != 1) return nullptr;
    for (int p = 0; p < SMP_PADS; ++p)
        if ((int)std::lround(I->p.pads[p].note) == pitch) {
            if (I->p.pads[p].data && !I->padName[p].empty()) return I->padName[p].c_str();
            return pitch >= 36 && pitch <= 51 ? SMP_GM[pitch - 36] : L"pad";
        }
    return nullptr;
}

float gPrBandX0, gPrBandY0, gPrBandX1, gPrBandY1;
enum { PD_NONE, PD_MOVE, PD_RESIZE, PD_BAND, PD_ERASE, PD_VEL, PD_SEEK, PD_CLIPEND, PD_KEY };
int gPrDrag = PD_NONE;

void prDrawGrid(Graphics& g, const Clip& c, float top, float bottom, bool ruler) {
    double spb = samplesPerBeat(), bar = 4 * spb, st = prStepD();
    i64 base = prBase(c);
    double t0 = (double)(base + prDt(PR_KEYS)), t1 = (double)(base + prDt(gPW));
    Pen major(C_GRID, 1.0f), minor(C_GRID_MINOR, 1.0f), fine(Color(255, 26, 29, 34), 1.0f), tick(C_DIM, 1.0f);
    // finest lines first: grid steps, beats, bars
    if (!ruler && st > 0 && st * gPrPx / SR >= 6)
        for (double t = std::floor(t0 / st) * st; t <= t1; t += st) {
            float x = prX((i64)std::llround(t - base));
            if (x >= PR_KEYS) g.DrawLine(&fine, x, top, x, bottom);
        }
    bool beats = spb * gPrPx / SR >= 8;
    for (double b = std::floor(t0 / bar) * bar; b <= t1; b += bar) {
        float x = prX((i64)std::llround(b - base));
        if (beats)
            for (int k = 1; k < 4; ++k) {
                float xb = prX((i64)std::llround(b + k * spb - base));
                if (xb < PR_KEYS) continue;
                if (ruler) g.DrawLine(&minor, xb, bottom - 5, xb, bottom);
                else g.DrawLine(&minor, xb, top, xb, bottom);
            }
        if (x < PR_KEYS) continue;
        if (ruler) {
            g.DrawLine(&tick, x, bottom - 10, x, bottom);
            wchar_t s[16];
            swprintf(s, 16, L"%d", (int)std::llround(b / bar) + 1);
            drawText(g, s, fSmall, C_DIM, RectF(x + 3, top + 1, 40, 16), StringAlignmentNear);
        } else {
            g.DrawLine(&major, x, top, x, bottom);
        }
    }
}

void prPaint(HDC hdc) {
    RECT rc;
    GetClientRect(gPrWnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.ScaleTransform(gScale, gScale);
        g.Clear(C_BG);

        int ti = -1;
        Clip* c = prClip(&ti);
        const Track* tr = c ? &gTracks[ti] : nullptr;
        const Instrument* I = tr ? tr->inst.get() : nullptr;
        Color tc = tr ? TRACK_COLORS[tr->color % 8] : C_ACCENT;

        // toolbar
        SolidBrush bar(C_PANEL);
        g.FillRectangle(&bar, 0.0f, 0.0f, gPW, PR_TOP);
        Pen line(C_BORDER, 1.0f);
        g.DrawLine(&line, 0.0f, PR_TOP - 0.5f, gPW, PR_TOP - 0.5f);
        if (c) {   // (the track's name is in the title bar)
            wchar_t s[64];
            swprintf(s, 64, L"Clip at bar %d   ·   %d notes", (int)(c->start / (4 * samplesPerBeat())) + 1, (int)gPrNotes.size());
            drawText(g, s, fTitle, tc, RectF(14, 9, 290, 30), StringAlignmentNear);
        }
        drawText(g, L"GRID", fSmall, C_DIM, RectF(312, 9, 40, 30), StringAlignmentNear);
        for (int i = 0; i < 5; ++i) drawToggle(g, prGridBtn(i), PR_DIV_NAMES[i], gPrDiv == PR_DIVS[i], C_ACCENT);
        drawButton(g, prBtnQuant, L"Quantize", gPrDiv != 0);
        fillRound(g, prBtnSampler, 5, C_BTN, &C_ACCENT);
        drawText(g, I && I->kind == INST_SYNTH ? L"Synth" : L"Sampler", fLabel, C_ACCENT, prBtnSampler);
        drawTransportIcon(g, prBtnPlay, 1, gPlaying.load());

        float nt = prNoteTop(), nb = prNoteBottom(), vt = prVelTop(), vb = prVelBottom();
        if (!c) {
            drawText(g, L"This clip no longer exists (deleted or undone). Double-click a MIDI clip in the main window.",
                     fLabel, C_DIM, RectF(12, nt, gPW - 24, nb - nt));
        } else {
            i64 base = prBase(*c);
            // note area: rows
            RectF area(PR_KEYS, nt, gPW - PR_KEYS, nb - nt);
            g.SetClip(area);
            SolidBrush bg(C_CANVAS), blackRow(Color(255, 15, 16, 20)), padRow(mixColor(C_CANVAS, tc, 0.08f));
            g.FillRectangle(&bg, area);
            Pen cLine(Color(255, 40, 44, 52), 1.0f);
            for (int p = 0; p < 128; ++p) {
                float y = prY(p);
                if (y > nb || y + PR_ROW < nt) continue;
                if (prPadLabel(I, p)) g.FillRectangle(&padRow, PR_KEYS, y, gPW - PR_KEYS, PR_ROW);
                else if (smpBlack(p)) g.FillRectangle(&blackRow, PR_KEYS, y, gPW - PR_KEYS, PR_ROW);
                if (p % 12 == 0) g.DrawLine(&cLine, PR_KEYS, y + PR_ROW - 0.5f, gPW, y + PR_ROW - 0.5f);
            }
            prDrawGrid(g, *c, nt, nb, false);
            // outside the clip: darker (those notes are not played)
            SolidBrush shade(Color(120, 0, 0, 0));
            float cx0 = prX(c->off), cx1 = prX(c->off + c->len);
            if (cx0 > PR_KEYS) g.FillRectangle(&shade, PR_KEYS, nt, cx0 - PR_KEYS, nb - nt);
            if (cx1 < gPW) g.FillRectangle(&shade, cx1, nt, gPW - cx1, nb - nt);
            Pen edgePen(tc, 1.0f);
            g.DrawLine(&edgePen, cx0, nt, cx0, nb);
            g.DrawLine(&edgePen, cx1, nt, cx1, nb);

            // notes
            for (auto& pn : gPrNotes) {
                const Note& n = pn.n;
                float x0 = prX(n.start), x1 = prX(n.start + n.len), y = prY(n.pitch);
                if (x1 < PR_KEYS || x0 > gPW || y > nb || y + PR_ROW < nt) continue;
                bool inside = n.start < c->off + c->len && n.start + n.len > c->off;
                Color fill = mixColor(Color(255, 40, 44, 52), tc, 0.35f + 0.65f * n.vel / 127.0f);
                if (!inside) fill = mixColor(C_CANVAS, fill, 0.35f);
                RectF r(x0, y + 1, std::max(3.0f, x1 - x0), PR_ROW - 2);
                fillRound(g, r, 2, fill);
                if (pn.sel) {
                    GraphicsPath gp;
                    roundRect(gp, r, 2);
                    Pen sp(Color(255, 255, 255, 255), 1.5f);
                    g.DrawPath(&sp, &gp);
                }
            }
            // playhead
            float px = prX(gPlayPos.load() - base);
            Pen ph(gRecording.load() ? C_REC : C_TEXT, 1.0f);
            g.DrawLine(&ph, px, nt, px, nb);
            // selection box
            if (gPrDrag == PD_BAND) {
                RectF br(std::min(gPrBandX0, gPrBandX1), std::min(gPrBandY0, gPrBandY1), std::fabs(gPrBandX1 - gPrBandX0),
                         std::fabs(gPrBandY1 - gPrBandY0));
                SolidBrush bb(Color(40, 255, 255, 255));
                g.FillRectangle(&bb, br);
                Pen bp(Color(160, 255, 255, 255), 1.0f);
                g.DrawRectangle(&bp, br);
            }
            g.ResetClip();

            // keyboard
            RectF keys(0, nt, PR_KEYS, nb - nt);
            g.SetClip(keys);
            for (int p = 0; p < 128; ++p) {
                float y = prY(p);
                if (y > nb || y + PR_ROW < nt) continue;
                bool held = I && I->held[p].load();
                Color kc = held ? C_ACCENT : smpBlack(p) ? Color(255, 30, 32, 38) : Color(255, 200, 203, 210);
                SolidBrush kb(kc);
                g.FillRectangle(&kb, 0.0f, y, PR_KEYS - 1, PR_ROW - 1);
                const wchar_t* pad = prPadLabel(I, p);
                Color txt = smpBlack(p) && !held ? Color(255, 150, 154, 162) : Color(255, 40, 42, 48);
                if (pad) drawText(g, pad, fSmall, txt, RectF(3, y - 1, PR_KEYS - 6, PR_ROW + 2), StringAlignmentNear);
                else if (p % 12 == 0) drawText(g, smpNoteName(p).c_str(), fSmall, txt, RectF(3, y - 1, PR_KEYS - 6, PR_ROW + 2), StringAlignmentFar);
            }
            g.ResetClip();

            // ruler
            RectF rr(PR_KEYS, PR_TOP, gPW - PR_KEYS, PR_RULER);
            g.FillRectangle(&bar, rr);
            g.SetClip(rr);
            prDrawGrid(g, *c, PR_TOP, nt, true);
            SolidBrush handle(C_ACCENT);
            PointF tri[3] = {PointF(cx1 - 7, PR_TOP + 4), PointF(cx1, PR_TOP + 4), PointF(cx1, nt - 2)};
            g.FillPolygon(&handle, tri, 3);
            g.FillRectangle(&handle, cx1 - 1, PR_TOP + 4, 2.0f, PR_RULER - 6);
            SolidBrush pb(gRecording.load() ? C_REC : C_TEXT);
            PointF ptri[3] = {PointF(px - 5, PR_TOP + 10), PointF(px + 5, PR_TOP + 10), PointF(px, nt)};
            g.FillPolygon(&pb, ptri, 3);
            g.ResetClip();
            g.FillRectangle(&bar, 0.0f, PR_TOP, PR_KEYS, PR_RULER);
            g.DrawLine(&line, 0.0f, nt - 0.5f, gPW, nt - 0.5f);

            // velocity lane
            SolidBrush vbg(C_PANEL);
            g.FillRectangle(&vbg, 0.0f, nb, gPW, vt - nb);
            g.FillRectangle(&bg, PR_KEYS, vt, gPW - PR_KEYS, vb - vt);
            g.FillRectangle(&vbg, 0.0f, vt, PR_KEYS, vb - vt);
            drawText(g, L"VELOCITY", fSmall, C_DIM, RectF(4, vt, PR_KEYS - 6, 20), StringAlignmentNear);
            RectF va(PR_KEYS, vt, gPW - PR_KEYS, vb - vt);
            g.SetClip(va);
            for (auto& pn : gPrNotes) {
                float x = prX(pn.n.start);
                if (x < PR_KEYS - 4 || x > gPW + 4) continue;
                float h = (PR_VEL - 12) * pn.n.vel / 127.0f, y = vb - 6 - h;
                Color col = pn.sel ? Color(255, 255, 255, 255) : tc;
                Pen sp(col, 2.0f);
                g.DrawLine(&sp, x + 1, vb - 6, x + 1, y);
                SolidBrush db(col);
                g.FillEllipse(&db, x - 2.5f, y - 3.5f, 7.0f, 7.0f);
            }
            g.ResetClip();
        }

        // status bar
        RectF sr(0, gPH - PR_STATUS, gPW, PR_STATUS);
        g.FillRectangle(&bar, sr);
        g.DrawLine(&line, 0.0f, sr.Y + 0.5f, gPW, sr.Y + 0.5f);
        bool recent = !gPrMsg.empty() && GetTickCount() - gPrMsgTime < 5000;
        const wchar_t* hint = L"Click: add note  ·  drag: move  ·  drag right edge: length  ·  right-click: delete  ·  "
                              L"Ctrl+drag: select  ·  Del  ·  Ctrl+D duplicate  ·  Q quantize  ·  arrows: move  ·  Alt: no snap  ·  Ctrl+wheel zoom";
        drawText(g, recent ? gPrMsg.c_str() : hint, fSmall, recent ? C_TEXT : C_DIM, RectF(12, sr.Y, gPW - 24, sr.Height),
                 StringAlignmentNear);
    }
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ============================================================================
// Mouse
// ============================================================================
int gPrGrab = -1, gPrLastDp = 0;
i64 gPrLastDt = 0;
float gPrX0 = 0, gPrY0 = 0;

void prEraseAt(float x, float y) {
    int i = prNoteAt(x, y, nullptr);
    if (i < 0) return;
    gPrNotes.erase(gPrNotes.begin() + i);
    prCommit();
}

void prVelAt(float x, float y) {
    int vel = (int)std::lround((prVelBottom() - 6 - y) / (PR_VEL - 12) * 127);
    vel = vel < 1 ? 1 : (vel > 127 ? 127 : vel);
    bool sel = prAnySel(), changed = false;
    for (auto& p : gPrNotes) {
        if (sel && !p.sel) continue;
        if (std::fabs(prX(p.n.start) - x) <= 4 && p.n.vel != vel) { p.n.vel = vel; changed = true; }
    }
    if (changed) { gPrLastVel = vel; prCommit(); }
}

void prBeginDrag(int grab, float x, float y, int kind) {
    gPrOrig = gPrNotes;
    gPrGrab = grab;
    gPrX0 = x;
    gPrY0 = y;
    gPrLastDt = 0;
    gPrLastDp = 0;
    gPrDrag = kind;
}

void prOnDown(float x, float y, bool right) {
    prSync();
    int ti = -1;
    Clip* c = prClip(&ti);
    if (y < PR_TOP) {
        if (right) return;
        for (int i = 0; i < 5; ++i)
            if (prGridBtn(i).Contains(x, y)) { gPrDiv = PR_DIVS[i]; gPrLastLen = 0; return; }
        if (prBtnQuant.Contains(x, y)) prQuantize();
        else if (prBtnSampler.Contains(x, y) && c) { setLiveTrack(gTracks[ti].id); instOpenWindow(); }
        else if (prBtnPlay.Contains(x, y)) {
            if (gPlaying.load() || gRecState != REC_IDLE) transportStop(); else transportPlay();
        }
        return;
    }
    if (!c) return;
    gPrUndoDone = false;
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;

    if (y < prNoteTop()) {   // ruler
        if (right) return;
        if (std::fabs(x - prX(c->off + c->len)) <= 7) { gPrDrag = PD_CLIPEND; return; }
        gPrDrag = PD_SEEK;
        seekTo(std::max<i64>(0, prBase(*c) + prDt(std::max(x, PR_KEYS))));
        return;
    }
    if (y >= prVelTop()) {
        if (!right && y < prVelBottom() && x >= PR_KEYS) { gPrDrag = PD_VEL; prVelAt(x, y); }
        return;
    }
    if (y >= prNoteBottom()) return;
    if (x < PR_KEYS) {
        if (!right) { prAudition(prPitchAt(y), 100); gPrDrag = PD_KEY; }
        return;
    }
    if (right) { gPrDrag = PD_ERASE; prEraseAt(x, y); return; }

    bool edge = false;
    int i = prNoteAt(x, y, &edge);
    if (i >= 0) {
        if (shift) {
            gPrNotes[i].sel = !gPrNotes[i].sel;
            if (!gPrNotes[i].sel) return;
        } else if (!gPrNotes[i].sel) {
            prSelectAll(false);
            gPrNotes[i].sel = true;
        }
        const Note& n = gPrNotes[i].n;
        gPrLastVel = n.vel;
        gPrLastLen = n.len;
        prAudition(n.pitch, n.vel);
        prBeginDrag(i, x, y, edge ? PD_RESIZE : PD_MOVE);
        return;
    }
    if (ctrl || shift) {   // box selection (Shift keeps the current selection)
        if (!shift) prSelectAll(false);
        prBeginDrag(-1, x, y, PD_BAND);
        gPrBandX0 = gPrBandX1 = x;
        gPrBandY0 = gPrBandY1 = y;
        return;
    }
    // a new note, then drag it into place
    prSelectAll(false);
    Note n;
    i64 t = std::max<i64>(0, prDt(x));
    n.start = prAlt() ? t : std::max<i64>(0, prSnap(t, *c, true));
    n.pitch = prPitchAt(y);
    n.vel = gPrLastVel;
    double st = prStepD();
    n.len = gPrLastLen > 0 ? gPrLastLen : (i64)std::llround(st > 0 ? st : samplesPerBeat() / 4);
    gPrNotes.push_back({n, true});
    prCommit();
    prAudition(n.pitch, n.vel);
    prBeginDrag((int)gPrNotes.size() - 1, x, y, PD_MOVE);
}

void prOnMove(float x, float y) {
    if (gPrDrag == PD_NONE) return;
    Clip* c = prClip();
    if (!c) { gPrDrag = PD_NONE; return; }
    bool snap = !prAlt();
    switch (gPrDrag) {
    case PD_MOVE: {
        if (gPrGrab < 0 || gPrGrab >= (int)gPrOrig.size() || gPrOrig.size() != gPrNotes.size()) break;
        const Note g0 = gPrOrig[gPrGrab].n;
        i64 ns = g0.start + (prDt(x) - prDt(gPrX0));
        if (snap) ns = prSnap(ns, *c, false);
        i64 dt = ns - g0.start;
        int dp = prPitchAt(y) - prPitchAt(gPrY0);
        i64 minStart = LLONG_MAX;
        int minP = 127, maxP = 0;
        for (auto& p : gPrOrig)
            if (p.sel) { minStart = std::min(minStart, p.n.start); minP = std::min(minP, p.n.pitch); maxP = std::max(maxP, p.n.pitch); }
        if (minStart + dt < 0) dt = -minStart;
        dp = std::max(-minP, std::min(127 - maxP, dp));
        if (dt == gPrLastDt && dp == gPrLastDp) break;
        bool pitchChanged = dp != gPrLastDp;
        gPrLastDt = dt;
        gPrLastDp = dp;
        for (size_t i = 0; i < gPrNotes.size(); ++i)
            if (gPrOrig[i].sel) {
                gPrNotes[i].n.start = gPrOrig[i].n.start + dt;
                gPrNotes[i].n.pitch = gPrOrig[i].n.pitch + dp;
            }
        prCommit();
        if (pitchChanged) prAudition(g0.pitch + dp, g0.vel);
        break;
    }
    case PD_RESIZE: {
        if (gPrGrab < 0 || gPrGrab >= (int)gPrOrig.size() || gPrOrig.size() != gPrNotes.size()) break;
        const Note g0 = gPrOrig[gPrGrab].n;
        i64 ne = g0.start + g0.len + (prDt(x) - prDt(gPrX0));
        if (snap) ne = prSnap(ne, *c, false);
        i64 dl = ne - (g0.start + g0.len);
        if (dl == gPrLastDt) break;
        gPrLastDt = dl;
        double st = prStepD();
        i64 minLen = snap && st > 0 ? (i64)std::llround(st) : SR / 100;
        for (size_t i = 0; i < gPrNotes.size(); ++i)
            if (gPrOrig[i].sel) gPrNotes[i].n.len = std::max(std::min(minLen, gPrOrig[i].n.len), gPrOrig[i].n.len + dl);
        gPrLastLen = gPrNotes[gPrGrab].n.len;
        prCommit();
        break;
    }
    case PD_BAND: {
        gPrBandX1 = x;
        gPrBandY1 = y;
        float bx0 = std::min(gPrBandX0, x), bx1 = std::max(gPrBandX0, x), by0 = std::min(gPrBandY0, y), by1 = std::max(gPrBandY0, y);
        if (gPrOrig.size() != gPrNotes.size()) break;
        for (size_t i = 0; i < gPrNotes.size(); ++i) {
            const Note& n = gPrNotes[i].n;
            float x0 = prX(n.start), x1 = prX(n.start + n.len), ny = prY(n.pitch);
            bool in = x1 >= bx0 && x0 <= bx1 && ny + PR_ROW >= by0 && ny <= by1;
            gPrNotes[i].sel = gPrOrig[i].sel || in;
        }
        break;
    }
    case PD_ERASE: prEraseAt(x, y); break;
    case PD_VEL: prVelAt(x, std::max(prVelTop(), std::min(prVelBottom(), y))); break;
    case PD_SEEK: seekTo(std::max<i64>(0, prBase(*c) + prDt(std::max(x, PR_KEYS)))); break;
    case PD_CLIPEND: {
        i64 e = prDt(x);
        if (snap) e = prSnap(e, *c, false);
        e = std::max(e, c->off + SR / 20);
        if (e - c->off == c->len) break;
        if (!gPrUndoDone) { pushUndo(); gPrUndoDone = true; c = prClip(); }
        std::lock_guard<std::mutex> lk(gMx);
        c->len = e - c->off;
        gDirty = true;
        break;
    }
    case PD_KEY: {
        int p = prPitchAt(std::max(prNoteTop(), std::min(prNoteBottom() - 1, y)));
        if (p != gPrAudNote) prAudition(p, 100);
        break;
    }
    default: break;
    }
}

void prEndDrag() {
    if (gPrDrag == PD_MOVE || gPrDrag == PD_RESIZE) prSortNotes();
    gPrDrag = PD_NONE;
    gPrOrig.clear();
    prStopAudition();
}

// ============================================================================
// Keyboard
// ============================================================================
bool prHandleKey(const MSG& m) {
    if (!gPrWnd || m.message != WM_KEYDOWN) return false;
    if (GetAncestor(m.hwnd, GA_ROOT) != gPrWnd) return false;
    prSync();
    WPARAM vk = m.wParam;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl) {
        switch (vk) {
        case 'Z': undo(); prSync(); prMsg(L"Undo"); return true;
        case 'A': prSelectAll(true); return true;
        case 'D': prDuplicate(); return true;
        case 'R': transportRecord(); return true;
        }
        return false;
    }
    switch (vk) {
    case VK_SPACE:
        if (gPlaying.load() || gRecState != REC_IDLE) transportStop(); else transportPlay();
        return true;
    case 'R': transportRecord(); return true;   // same as the main window (Space stops)
    case VK_DELETE: case VK_BACK: prDeleteSel(); return true;
    case 'Q': prQuantize(); return true;
    case VK_UP: prTranspose(shift ? 12 : 1); return true;
    case VK_DOWN: prTranspose(shift ? -12 : -1); return true;
    case VK_LEFT: prNudge(-1); return true;
    case VK_RIGHT: prNudge(1); return true;
    case VK_ESCAPE: prSelectAll(false); return true;
    case VK_HOME: if (Clip* c = prClip()) seekTo(c->start); return true;
    }
    return false;
}

// ============================================================================
// Window
// ============================================================================
LRESULT CALLBACK PrWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        gPrWnd = hwnd;
        SetTimer(hwnd, 1, 30, NULL);
        return 0;
    case WM_TIMER: {
        if (gPrDrag == PD_NONE) {
            prSync();
            Clip* c = prClip();
            if (c && gPlaying.load()) {   // follow the playhead, page by page
                float px = prX(gPlayPos.load() - prBase(*c));
                if (px > gPW - 20 && px < gPW + 80) gPrView += (gPW - PR_KEYS - 60) / gPrPx;
            }
        }
        int ti;
        Clip* c = prClip(&ti);
        std::wstring t = (c ? gTracks[ti].name + L"  -  " : std::wstring()) + L"Piano roll  -  MiniDAW";
        if (t != gPrTitle) { gPrTitle = t; SetWindowTextW(hwnd, t.c_str()); }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_SIZE:
        gPW = LOWORD(lParam) / gScale;
        gPH = HIWORD(lParam) / gScale;
        prClampView();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = S(900);
        mm->ptMinTrackSize.y = S(480);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        prPaint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            float x = pt.x / gScale, y = pt.y / gScale;
            bool we = gPrDrag == PD_RESIZE || gPrDrag == PD_CLIPEND;
            if (gPrDrag == PD_NONE) {
                Clip* c = prClip();
                if (c && y >= PR_TOP && y < prNoteTop() && std::fabs(x - prX(c->off + c->len)) <= 7) we = true;
                bool edge = false;
                if (c && y >= prNoteTop() && y < prNoteBottom() && x >= PR_KEYS && prNoteAt(x, y, &edge) >= 0 && edge) we = true;
            }
            SetCursor(LoadCursor(NULL, we ? IDC_SIZEWE : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        SetFocus(hwnd);
        prOnDown(GET_X_LPARAM(lParam) / gScale, GET_Y_LPARAM(lParam) / gScale, msg == WM_RBUTTONDOWN);
        if (gPrDrag != PD_NONE) SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        prOnMove(GET_X_LPARAM(lParam) / gScale, GET_Y_LPARAM(lParam) / gScale);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (gPrDrag != PD_NONE) { prEndDrag(); ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
        if (gPrDrag != PD_NONE) prEndDrag();
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        float x = pt.x / gScale;
        float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        WORD keys = GET_KEYSTATE_WPARAM(wParam);
        if (msg == WM_MOUSEHWHEEL || (keys & MK_SHIFT)) {
            gPrView -= notches * 80 / gPrPx * (msg == WM_MOUSEHWHEEL ? -1 : 1);
        } else if (keys & MK_CONTROL) {   // zoom around the mouse
            double t = gPrView + (std::max(x, PR_KEYS) - PR_KEYS) / gPrPx;
            gPrPx = std::min(5000.0, std::max(10.0, gPrPx * std::pow(1.25, notches)));
            gPrView = t - (std::max(x, PR_KEYS) - PR_KEYS) / gPrPx;
        } else {
            gPrScroll -= notches * PR_ROW * 4;
        }
        prClampView();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && gPrDrag != PD_NONE) { prEndDrag(); ReleaseCapture(); }
        return 0;
    case WM_CLOSE:   // just hide; double-click a clip to show it again
        if (gPrDrag != PD_NONE) prEndDrag();
        ShowWindow(hwnd, SW_HIDE);
        SetForegroundWindow(gWnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        gPrWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Opens the piano roll on the clip with this uid (makes its track the live one, so you hear what you edit)
void prOpen(int uid) {
    gPrUid = uid;
    gPrLoaded.reset();
    gPrNotes.clear();
    gPrDrag = PD_NONE;
    prSync();
    int ti;
    Clip* c = prClip(&ti);
    if (!c) return;
    setLiveTrack(gTracks[ti].id);

    if (!gPrWnd) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc = {};
            wc.style         = CS_DBLCLKS;
            wc.lpfnWndProc   = PrWndProc;
            wc.hInstance     = GetModuleHandleW(NULL);
            wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
            wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
            wc.lpszClassName = L"MiniDAWPianoRoll";
            RegisterClassW(&wc);
            registered = true;
        }
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT r = {0, 0, S(1100), S(640)};
        AdjustWindowRect(&r, style, FALSE);
        RECT mr;
        GetWindowRect(gWnd, &mr);
        CreateWindowW(L"MiniDAWPianoRoll", L"MiniDAW - Piano roll", style, mr.left + S(40), mr.top + S(110),
                      r.right - r.left, r.bottom - r.top, gWnd, NULL, GetModuleHandleW(NULL), NULL);   // sets gPrWnd, gPW, gPH
    }

    // show the whole clip, centred on its notes
    double secs = std::max(0.25, (double)c->len / SR);
    gPrPx = std::min(3000.0, std::max(20.0, (gPW - PR_KEYS - 40) / secs));
    gPrView = (double)c->off / SR;
    int lo = 127, hi = 0;
    for (auto& p : gPrNotes) { lo = std::min(lo, p.n.pitch); hi = std::max(hi, p.n.pitch); }
    const Instrument* inst = gTracks[ti].inst.get();
    int centre = lo <= hi ? (lo + hi) / 2 : (inst && inst->kind == INST_SAMPLER && inst->p.mode == 1 ? 42 : 60);
    gPrScroll = (127 - centre) * PR_ROW - (prNoteBottom() - prNoteTop()) / 2;
    prClampView();

    if (IsIconic(gPrWnd)) ShowWindow(gPrWnd, SW_RESTORE);
    ShowWindow(gPrWnd, SW_SHOW);
    SetForegroundWindow(gPrWnd);
    SetFocus(gPrWnd);
}
