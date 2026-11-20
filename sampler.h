// sampler.h - MiniDAW Sampler window: edits the instrument (instrument.h) of the live MIDI track
//
// Included by daw_gui.cpp (one translation unit, so the build command does not change). It uses the
// DAW's audio data type, file loader, transport and drawing helpers.
//
// Every MIDI track has its own Sampler. This window shows the one of the "live" MIDI track (the MIDI
// track selected last); its title bar names the track. Two modes (tabs at the top):
//   Melodic  (one sample)   - load one sound (e.g. a bell). Every key plays it at that key's pitch:
//                             the sample is played faster / slower relative to its root note.
//                             Transpose, root note (detection, or right-click a key), fine tune, attack, release,
//                             volume, optional loop (with crossfade) for sustained notes.
//                             Library button: the sounds in samples\ ("Name (C4).wav" = root note C4).
//   Drum kit (multi sample) - 16 pads, each with its own sample (kick, snare, hi-hat ...), volume,
//                             pitch, pan and MIDI note. Every pad / key plays a different sound.
//
// Play it with the PC keyboard, the mouse or a MIDI keyboard; it is heard through its MIDI track.
// To record notes: arm the MIDI track (R) and press record.

#pragma once

const int SMP_PIANO_LO = 36, SMP_PIANO_HI = 96;   // on-screen keyboard: C2 .. C7

// The instrument this window edits: the live track's, if it is a Sampler (GUI thread is the only writer of gLiveInst)
Instrument* smpI() { return gLiveInst && gLiveInst->kind == INST_SAMPLER ? gLiveInst.get() : nullptr; }

// ============================================================================
// Sample loading and pitch detection
// ============================================================================
std::wstring smpBaseName(const std::wstring& path) {
    std::wstring nm = fileNameOf(path);
    size_t dot = nm.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) nm = nm.substr(0, dot);
    return nm;
}

DataPtr smpLoadFile(const std::wstring& path) {   // mono at SR, leading silence removed
    auto d = loadAudioFile(path);
    if (!d) return nullptr;
    size_t i = 0, N = d->s.size();
    while (i < N && std::fabs(d->s[i]) < 0.003f) ++i;   // about -50 dB
    if (i >= N || i <= (size_t)(SR / 1000)) return d;
    size_t st = i - SR / 1000;                           // keep 1 ms before the onset
    auto t = std::make_shared<AudioData>();
    t->append(d->s.data() + st, N - st);
    return t;
}

// ============================================================================
// Presets (presets\ folder: free CC0 samples from VCSL, see presets\CREDITS.txt)
// ============================================================================
struct SmpMelPreset {
    const wchar_t* name;
    const wchar_t* file;
    int root;                  // measured pitch of the sample
    float tune;                // cents to correct it
    bool loop;
    float loopStart, attack, release;
};
const SmpMelPreset SMP_MEL_PRESETS[] = {
    {L"Bell",  L"Melodic\\Bell.wav",  72, 3,  false, 0.25f, 2,  1500},
    {L"Organ", L"Melodic\\Organ.wav", 60, 4,  true,  0.35f, 3,  150},
    {L"Piano", L"Melodic\\Piano.wav", 60, -2, false, 0.25f, 2,  500},
};
const int SMP_MEL_PRESET_N = sizeof(SMP_MEL_PRESETS) / sizeof(SMP_MEL_PRESETS[0]);

struct SmpPadPreset { const wchar_t* name; const wchar_t* file; float pitch, db, pan; };
struct SmpKitPreset { const wchar_t* name; SmpPadPreset pads[SMP_PADS]; };   // pads 1-16 = notes 36-51 (GM drums)
const SmpKitPreset SMP_KIT_PRESETS[] = {
    {L"Drum kit", {
        {L"Kick", L"Kick", 0, 0, 0},              {L"Side stick", L"Side stick", 0, -4, 0},
        {L"Snare", L"Snare", 0, 0, 0},            {L"Snare soft", L"Snare soft", 0, -3, 0},
        {L"Snare off", L"Snare off", 0, -2, 0},   {L"Low tom", L"Tom low", 0, -2, -0.3f},
        {L"Closed hat", L"Hat closed", 0, -5, 0.3f}, {L"Low tom 2", L"Tom low", 3, -2, -0.2f},
        {L"Pedal hat", L"Hat pedal", 0, -6, 0.3f},   {L"Mid tom", L"Tom high", 0, -2, 0},
        {L"Open hat", L"Hat open", 0, -6, 0.3f},     {L"Mid tom 2", L"Tom high", 2, -2, 0.1f},
        {L"High tom", L"Tom high", 4, -2, 0.2f},     {L"Crash", L"Crash", 0, -6, -0.35f},
        {L"High tom 2", L"Tom high", 6, -2, 0.3f},   {L"Ride", L"Ride 2", 0, -7, 0.35f}}},
    {L"Jazz kit", {
        {L"Kick", L"Kick soft", 2, -2, 0},           {L"Side stick", L"Side stick", 1, -4, 0},
        {L"Snare", L"Snare medium", 2, -2, 0},       {L"Snare taps", L"Snare taps", 2, -2, 0},
        {L"Snare off", L"Snare off soft", 2, -3, 0}, {L"Low tom", L"Tom low soft", 3, -3, -0.3f},
        {L"Closed hat", L"Hat closed soft", 0, -6, 0.3f}, {L"Low tom 2", L"Tom low soft", 5, -3, -0.2f},
        {L"Pedal hat", L"Hat pedal", 0, -6, 0.3f},   {L"Mid tom", L"Tom high soft", 3, -3, 0},
        {L"Loose hat", L"Hat loose", 0, -7, 0.3f},   {L"Mid tom 2", L"Tom high soft", 5, -3, 0.1f},
        {L"High tom", L"Tom high soft", 7, -3, 0.2f}, {L"Crash", L"Crash soft", 0, -7, -0.35f},
        {L"Ride bell", L"Ride bell", 0, -8, 0.35f},  {L"Ride", L"Ride", 0, -6, 0.35f}}},
};
const int SMP_KIT_PRESET_N = sizeof(SMP_KIT_PRESETS) / sizeof(SMP_KIT_PRESETS[0]);

// A folder next to the program, or one folder up (output\daw_gui.exe -> presets\, samples\)
std::wstring smpFindDir(const wchar_t* name) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return L"";
    std::wstring d = exe;
    d = d.substr(0, d.find_last_of(L"\\/"));
    for (int up = 0; up < 2; ++up) {
        std::wstring p = d + L"\\" + name;
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return p;
        d = d.substr(0, d.find_last_of(L"\\/"));
    }
    return L"";
}

std::wstring smpPresetDir() {
    static std::wstring dir = smpFindDir(L"presets");
    return dir;
}

// samples\ : the melodic sample library (Library button). Created next to presets\ if it is missing.
std::wstring smpLibraryDir() {
    std::wstring d = smpFindDir(L"samples");
    if (d.empty()) {
        std::wstring pre = smpPresetDir();
        if (pre.empty()) return d;
        d = pre.substr(0, pre.find_last_of(L"\\/")) + L"\\samples";
        CreateDirectoryW(d.c_str(), NULL);
    }
    return d;
}

// "Marimba (C5)" -> name "Marimba", root 72. The note in brackets is the sample's pitch (C4 = middle C).
bool smpParseRoot(const std::wstring& base, std::wstring& name, int& root) {
    size_t open = base.find_last_of(L'(');
    if (open == std::wstring::npos || base.empty() || base.back() != L')') return false;
    std::wstring n = base.substr(open + 1, base.size() - open - 2);
    static const int pcs[7] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    if (n.empty()) return false;
    wchar_t L = towupper(n[0]);
    if (L < L'A' || L > L'G') return false;
    int pc = pcs[L - L'A'];
    size_t i = 1;
    if (i < n.size() && n[i] == L'#') { ++pc; ++i; }
    else if (i < n.size() && n[i] == L'b') { --pc; ++i; }
    if (i >= n.size()) return false;
    wchar_t* end = nullptr;
    long oct = wcstol(n.c_str() + i, &end, 10);
    if (*end || end == n.c_str() + i) return false;
    int r = (int)(oct + 1) * 12 + pc;
    if (r < 0 || r > 127) return false;
    root = r;
    name = base.substr(0, open);
    while (!name.empty() && name.back() == L' ') name.pop_back();
    return !name.empty();
}

DataPtr smpPresetSample(const std::wstring& rel) {   // loaded once, shared by every track that uses it
    static std::map<std::wstring, DataPtr> cache;
    auto it = cache.find(rel);
    if (it != cache.end()) return it->second;
    std::wstring dir = smpPresetDir();
    DataPtr d = dir.empty() ? nullptr : DataPtr(loadAudioFile(dir + L"\\" + rel));
    if (d) cache[rel] = d;
    return d;
}

bool smpApplyMelPreset(Instrument& I, int i) {
    if (i < 0 || i >= SMP_MEL_PRESET_N) return false;
    const SmpMelPreset& m = SMP_MEL_PRESETS[i];
    DataPtr d = smpPresetSample(m.file);
    if (!d) return false;
    {
        std::lock_guard<std::mutex> lk(gMx);
        SmpParams& P = I.p;
        P.mData = d;
        P.mRoot = (float)m.root;
        P.mTune = m.tune;
        P.mLoop = m.loop;
        P.mLoopStart = m.loopStart;
        P.mAttack = m.attack;
        P.mRelease = m.release;
    }
    I.mName = m.name;
    I.melPreset = i;
    return true;
}

bool smpApplyKitPreset(Instrument& I, int k) {
    if (k < 0 || k >= SMP_KIT_PRESET_N) return false;
    const SmpKitPreset& kit = SMP_KIT_PRESETS[k];
    DataPtr d[SMP_PADS];
    for (int p = 0; p < SMP_PADS; ++p) {
        d[p] = smpPresetSample(std::wstring(L"Drums\\") + kit.pads[p].file + L".wav");
        if (!d[p]) return false;
    }
    {
        std::lock_guard<std::mutex> lk(gMx);
        for (int p = 0; p < SMP_PADS; ++p) {
            SmpPad& pd = I.p.pads[p];
            pd.data = d[p];
            pd.note = (float)(36 + p);
            pd.pitch = kit.pads[p].pitch;
            pd.vol = (kit.pads[p].db + 60.0f) / 72.0f;   // fader units (see faderDb)
            pd.pan = kit.pads[p].pan;
        }
    }
    for (int p = 0; p < SMP_PADS; ++p) I.padName[p] = kit.pads[p].name;
    I.kitPreset = k;
    return true;
}

// For the track header: the preset / sample name in the current mode
std::wstring smpSoundName(const Instrument& I) {
    if (I.p.mode == 1) {
        if (I.kitPreset >= 0) return SMP_KIT_PRESETS[I.kitPreset].name;
        for (auto& pd : I.p.pads) if (pd.data) return L"My kit";
        return L"Load sounds";
    }
    return I.p.mData && !I.mName.empty() ? I.mName : std::wstring(L"Load a sound");
}

// A new MIDI track starts with the Piano (melodic) and the Drum kit loaded, so it plays right away
void smpApplyDefaultPresets(Instrument& I) {
    smpApplyMelPreset(I, 2);
    smpApplyKitPreset(I, 0);
}

// McLeod-style pitch detection (normalized square difference) on a piece after the attack.
bool smpDetectPitch(const std::vector<float>& s, float& hz) {
    const int W = 2048, minLag = SR / 2000, maxLag = SR / 50;   // 50 Hz .. 2 kHz
    size_t N = s.size();
    if (N < (size_t)(W + maxLag + 2)) return false;
    float peak = 0.0f;
    for (float x : s) peak = std::max(peak, std::fabs(x));
    if (peak < 1e-4f) return false;
    size_t onset = 0;
    while (onset < N && std::fabs(s[onset]) < 0.2f * peak) ++onset;
    size_t start = onset + SR / 40;   // skip 25 ms of attack
    if (start + W + maxLag + 2 > N) start = N - W - maxLag - 2;
    std::vector<float> nsdf(maxLag + 2, 0.0f);
    for (int tau = minLag; tau <= maxLag + 1; ++tau) {
        double acf = 0, m = 0;
        const float* a = s.data() + start;
        for (int i = 0; i < W; ++i) {
            acf += (double)a[i] * a[i + tau];
            m += (double)a[i] * a[i] + (double)a[i + tau] * a[i + tau];
        }
        nsdf[tau] = m > 0 ? (float)(2.0 * acf / m) : 0.0f;
    }
    // key maxima: the highest point of each positive lobe after the first zero crossing
    std::vector<int> keys;
    int t = minLag;
    while (t <= maxLag && nsdf[t] > 0) ++t;
    int best = -1;
    for (; t <= maxLag; ++t) {
        if (nsdf[t] > 0) {
            if (best < 0 || nsdf[t] > nsdf[best]) best = t;
        } else if (best >= 0) {
            keys.push_back(best);
            best = -1;
        }
    }
    if (best >= 0) keys.push_back(best);
    if (keys.empty()) return false;
    float maxv = 0.0f;
    for (int k : keys) maxv = std::max(maxv, nsdf[k]);
    if (maxv < 0.5f) return false;
    int k = keys[0];
    for (int c : keys) if (nsdf[c] >= 0.9f * maxv) { k = c; break; }
    float a = nsdf[k - 1], b = nsdf[k], c = nsdf[k + 1], den = a - 2 * b + c;
    float off = std::fabs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f;
    hz = (float)SR / (k + clampf(off, -1.0f, 1.0f));
    return true;
}

// ============================================================================
// Sampler window: state and layout (logical pixels, scaled by gScale like the main window)
// ============================================================================
HWND gSmpWnd = NULL, gSmpMidiCombo = NULL;
HMIDIIN gSmpMidi = NULL;
const int ID_SMP_MIDI = 201;
const float SMP_STATUS_H = 24, SMP_PAD_W = 124;
float gSW = 1000, gSH = 640;
int gSmpSel = 0;                  // selected pad
int gSmpOct = 3;                  // PC keyboard: the lower row starts at C(gSmpOct)
int gSmpKeyHeld[256];             // per virtual key: note (melodic), 1000 + pad (kit), -1 = up
int gSmpMouseNote = -1;
int gSmpPadSeen[SMP_PADS];
DWORD gSmpPadFlash[SMP_PADS];
std::wstring gSmpMsg, gSmpTitle;
DWORD gSmpMsgTime = 0;

const char* SMP_KIT_KEYS = "ZXCVASDFQWER1234";   // pad 1 = Z (bottom-left) ... pad 16 = 4 (top-right)
const wchar_t* SMP_GM[16] = {L"Kick", L"Side stick", L"Snare", L"Clap", L"Snare 2", L"Low tom", L"Closed hat",
                             L"Low tom 2", L"Pedal hat", L"Mid tom", L"Open hat", L"Mid tom 2", L"High tom",
                             L"Crash", L"High tom 2", L"Ride"};

void smpMsg(const std::wstring& m) { gSmpMsg = m; gSmpMsgTime = GetTickCount(); }

std::wstring smpNoteName(int n) {
    static const wchar_t* nm[12] = {L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"};
    if (n < 0 || n > 127) return L"-";
    return nm[n % 12] + std::to_wstring(n / 12 - 1);
}

const RectF smpTabMel(14, 12, 172, 32), smpTabKit(192, 12, 172, 32);
const RectF smpBtnPlay(376, 12, 36, 32), smpBtnRec(416, 12, 36, 32);
RectF smpPresetBtn(int i, int mode) {   // top bar: presets of the current mode
    return mode == 0 ? RectF(520 + i * 56.0f, 15, 52, 26) : RectF(520 + i * 78.0f, 15, 74, 26);
}

RectF smpPanel() { return RectF(12, 68, gSW - 24, 150); }
RectF smpWaveRect() { RectF p = smpPanel(); return RectF(p.X + 12, p.Y + 42, p.Width - 24, p.Height - 52); }
RectF smpBtnLibrary() { RectF p = smpPanel(); return RectF(p.X + p.Width - 418, p.Y + 8, 100, 26); }
RectF smpBtnLoad() { RectF p = smpPanel(); return RectF(p.X + p.Width - 312, p.Y + 8, 96, 26); }
RectF smpBtnDetect() { RectF p = smpPanel(); return RectF(p.X + p.Width - 210, p.Y + 8, 110, 26); }
RectF smpBtnLoop() { RectF p = smpPanel(); return RectF(p.X + p.Width - 94, p.Y + 8, 82, 26); }
float smpPianoTop() { return 364; }
float smpPianoBottom() { return std::max(smpPianoTop() + 80, gSH - SMP_STATUS_H - 12); }

RectF smpPadRect(int p) {
    float avail = gSH - SMP_STATUS_H - 12 - 68, ph = std::max(60.0f, (avail - 24) / 4);
    int col = p % 4, row = 3 - p / 4;   // pad 1 bottom-left, like a drum machine
    return RectF(12 + col * (SMP_PAD_W + 8), 68 + row * (ph + 8), SMP_PAD_W, ph);
}
float smpKitX() { return 12 + 4 * SMP_PAD_W + 3 * 8 + 24; }
RectF smpKitLoad() { return RectF(smpKitX(), 100, 96, 26); }
RectF smpKitClear() { return RectF(smpKitX() + 102, 100, 76, 26); }
RectF smpKitWave() { return RectF(smpKitX(), 136, gSW - 12 - smpKitX(), 100); }

// Controls (label, slider, value)
enum { F_DB, F_SEMI, F_CENTS, F_MS, F_PAN, F_NOTE, F_PCT };
struct SmpCtl {
    const wchar_t* label;
    float* v;
    float mn, mx, def, step, wheel;
    int fmt;
    bool sq;          // square-law slider (finer at small values)
    RectF r;
    float labelW;
};

SmpCtl smpMk(const wchar_t* l, float* v, float mn, float mx, float def, float step, float wheel, int fmt, bool sq,
             const RectF& r, float lw = 96) {
    SmpCtl c;
    c.label = l; c.v = v; c.mn = mn; c.mx = mx; c.def = def; c.step = step; c.wheel = wheel;
    c.fmt = fmt; c.sq = sq; c.r = r; c.labelW = lw;
    return c;
}

void smpBuildCtls(std::vector<SmpCtl>& cs) {
    cs.clear();
    Instrument* I = smpI();
    if (!I) return;
    SmpParams& P = I->p;
    cs.push_back(smpMk(L"Volume", &P.master, 0, 1, FADER_0DB, 0, 0.01f, F_DB, false, RectF(gSW - 232, 12, 220, 32), 58));
    if (P.mode == 0) {
        float w = (gSW - 36) / 2, x1 = 12, x2 = 24 + w, y = 230, h = 34;
        // Transpose (right = higher). The sample's own pitch (root note) is set by the preset, the file name,
        // Detect root or a right-click on a key - it is the orange dot on the keyboard.
        cs.push_back(smpMk(L"Transpose", &P.mTranspose, -24, 24, 0, 1, 1, F_SEMI, false, RectF(x1, y, w, 30)));
        cs.push_back(smpMk(L"Fine tune", &P.mTune, -100, 100, 0, 1, 1, F_CENTS, false, RectF(x1, y + h, w, 30)));
        cs.push_back(smpMk(L"Level", &P.mVol, 0, 1, FADER_0DB, 0, 0.01f, F_DB, false, RectF(x1, y + 2 * h, w, 30)));
        cs.push_back(smpMk(L"Attack", &P.mAttack, 0, 2000, 2, 1, 5, F_MS, true, RectF(x2, y, w, 30)));
        cs.push_back(smpMk(L"Release", &P.mRelease, 5, 5000, 400, 1, 20, F_MS, true, RectF(x2, y + h, w, 30)));
        cs.push_back(smpMk(L"Loop start", &P.mLoopStart, 0, 0.99f, 0.25f, 0.001f, 0.01f, F_PCT, false,
                           RectF(x2, y + 2 * h, w, 30)));
    } else {
        SmpPad& pd = P.pads[gSmpSel];
        float x = smpKitX(), w = gSW - 12 - x, y = 250, h = 34;
        cs.push_back(smpMk(L"Level", &pd.vol, 0, 1, FADER_0DB, 0, 0.01f, F_DB, false, RectF(x, y, w, 30), 80));
        cs.push_back(smpMk(L"Pitch", &pd.pitch, -24, 24, 0, 0.1f, 1, F_SEMI, false, RectF(x, y + h, w, 30), 80));
        cs.push_back(smpMk(L"Pan", &pd.pan, -1, 1, 0, 0.01f, 0.05f, F_PAN, false, RectF(x, y + 2 * h, w, 30), 80));
        cs.push_back(smpMk(L"MIDI note", &pd.note, 0, 127, (float)(36 + gSmpSel), 1, 1, F_NOTE, false,
                           RectF(x, y + 3 * h, w, 30), 80));
    }
}

RectF smpSliderRect(const SmpCtl& c) { return RectF(c.r.X + c.labelW, c.r.Y, c.r.Width - c.labelW - 78, c.r.Height); }

float smpCtlPos(const SmpCtl& c) {
    float p = clampf((*c.v - c.mn) / (c.mx - c.mn), 0, 1);
    return c.sq ? std::sqrt(p) : p;
}

void smpSetV(const SmpCtl& c, float v) {
    v = clampf(v, c.mn, c.mx);
    if (c.step > 0) v = std::round(v / c.step) * c.step;
    if (c.fmt == F_DB && std::fabs(v - FADER_0DB) < 0.012f) v = FADER_0DB;   // sticky 0 dB
    std::lock_guard<std::mutex> lk(gMx);
    *c.v = v;
}

void smpCtlFromX(const SmpCtl& c, float x) {
    RectF s = smpSliderRect(c);
    float p = clampf((x - s.X) / s.Width, 0, 1);
    if (c.sq) p *= p;
    float v = c.mn + p * (c.mx - c.mn);
    if (c.fmt == F_PAN && std::fabs(v) < 0.04f) v = 0;
    smpSetV(c, v);
}

std::wstring smpFmt(const SmpCtl& c) {
    wchar_t b[48];
    float v = *c.v;
    if (std::fabs(v) < 1e-4f) v = 0;
    switch (c.fmt) {
    case F_DB:
        if (v <= 0.001f) return L"-inf dB";
        swprintf(b, 48, L"%+.1f dB", faderDb(v));
        break;
    case F_SEMI:
        if (v == std::round(v)) swprintf(b, 48, L"%+.0f st", v);
        else swprintf(b, 48, L"%+.1f st", v);
        break;
    case F_CENTS: swprintf(b, 48, L"%+.0f ct", v); break;
    case F_MS:
        if (v >= 1000) swprintf(b, 48, L"%.2f s", v / 1000);
        else swprintf(b, 48, L"%.0f ms", v);
        break;
    case F_PAN: {
        int p = (int)std::lround(v * 100);
        if (!p) return L"Centre";
        swprintf(b, 48, L"%ls %d", p < 0 ? L"L" : L"R", std::abs(p));
        break;
    }
    case F_NOTE: {
        int n = (int)std::lround(v);
        swprintf(b, 48, L"%ls (%d)", smpNoteName(n).c_str(), n);
        break;
    }
    default: swprintf(b, 48, L"%.0f %%", v * 100); break;
    }
    return b;
}

// On-screen keyboard
bool smpBlack(int n) { int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }
int smpWhiteIdx(int n) { int c = 0; for (int i = SMP_PIANO_LO; i < n; ++i) if (!smpBlack(i)) ++c; return c; }
int smpWhiteCount() { return smpWhiteIdx(SMP_PIANO_HI + 1); }
RectF smpKeyRect(int n) {
    float top = smpPianoTop(), bot = smpPianoBottom(), ww = (gSW - 24) / smpWhiteCount();
    if (!smpBlack(n)) return RectF(12 + smpWhiteIdx(n) * ww, top, ww - 1, bot - top);
    float bw = ww * 0.62f;
    return RectF(12 + smpWhiteIdx(n) * ww - bw / 2, top, bw, (bot - top) * 0.6f);
}
int smpPianoNoteAt(float x, float y) {
    if (y < smpPianoTop() || y > smpPianoBottom()) return -1;
    for (int n = SMP_PIANO_LO; n <= SMP_PIANO_HI; ++n) if (smpBlack(n) && smpKeyRect(n).Contains(x, y)) return n;
    for (int n = SMP_PIANO_LO; n <= SMP_PIANO_HI; ++n) {
        if (smpBlack(n)) continue;
        RectF r = smpKeyRect(n);
        if (x >= r.X && x < r.X + r.Width + 1) return n;
    }
    return -1;
}

// ============================================================================
// Sampler window: actions
// ============================================================================
void smpReleaseKeys() {
    for (int i = 0; i < 256; ++i) {
        if (gSmpKeyHeld[i] >= 0 && gSmpKeyHeld[i] < 1000) smpNoteOff(gSmpKeyHeld[i]);
        gSmpKeyHeld[i] = -1;
    }
    if (gSmpMouseNote >= 0) { smpNoteOff(gSmpMouseNote); gSmpMouseNote = -1; }
}

std::wstring smpWindowTitle() {
    Track* t = trackById(gLiveTrackId);
    return t && smpI() ? t->name + L"  -  MiniDAW" : std::wstring(L"Sampler  -  MiniDAW");
}

// The live MIDI track changed (setLiveTrack already sent all-notes-off to the previous instrument)
void smpOnLiveChanged() {
    for (int& h : gSmpKeyHeld) h = -1;
    gSmpMouseNote = -1;
    gSmpSel = 0;
    if (Instrument* I = smpI())
        for (int p = 0; p < SMP_PADS; ++p) { gSmpPadSeen[p] = I->padHits[p].load(); gSmpPadFlash[p] = 0; }
    if (gSmpWnd) {
        gSmpTitle = smpWindowTitle();
        SetWindowTextW(gSmpWnd, gSmpTitle.c_str());
        InvalidateRect(gSmpWnd, NULL, FALSE);
    }
}

void smpSetMode(int m) {
    Instrument* I = smpI();
    if (!I || m == I->p.mode) return;
    smpReleaseKeys();
    {
        std::lock_guard<std::mutex> lk(gMx);
        I->p.mode = m;
    }
    smpAllOff();
}

std::vector<std::wstring> smpOpenDialog(bool multi) {
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gSmpWnd;
    ofn.lpstrFilter = L"Audio files (*.wav;*.mp3;*.flac)\0*.wav;*.mp3;*.flac\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | (multi ? OFN_ALLOWMULTISELECT : 0);
    std::vector<std::wstring> files;
    if (!GetOpenFileNameW(&ofn)) return files;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!multi || !*p) files.push_back(first);
    else while (*p) { files.push_back(first + L"\\" + p); p += wcslen(p) + 1; }
    std::sort(files.begin(), files.end());
    return files;
}

void smpDetect();

// fromLibrary: a sound picked from samples\ starts with plain settings (no loop, short attack)
void smpLoadMelodic(const std::wstring& path, bool fromLibrary = false) {
    Instrument* I = smpI();
    if (!I) return;
    DataPtr d = smpLoadFile(path);
    if (!d) { smpMsg(L"Could not read " + fileNameOf(path)); return; }
    std::wstring name = smpBaseName(path);
    int root = -1;
    bool named = smpParseRoot(name, name, root);   // "Harp (C3).wav" -> root C3
    {
        std::lock_guard<std::mutex> lk(gMx);
        I->p.mData = d;
        if (named) { I->p.mRoot = (float)root; I->p.mTune = 0; }
        if (fromLibrary) { I->p.mLoop = false; I->p.mAttack = 2; I->p.mRelease = 400; }
    }
    I->mName = name;
    I->melPreset = -1;   // not one of the presets at the top
    if (named) {
        smpMsg(L"Loaded " + name + L" - root note " + smpNoteName(root) + L" (from the file name)");
    } else if (fromLibrary) {
        smpDetect();   // no note in the name: find it
    } else {
        wchar_t m[300];
        swprintf(m, 300, L"Loaded %ls (%.2f s). Root note is %ls - click \"Detect root\" or right-click the key it should be so the keys play in tune.",
                 I->mName.c_str(), (double)d->s.size() / SR, smpNoteName((int)std::lround(I->p.mRoot)).c_str());
        smpMsg(m);
    }
}

// Library button: a menu of the audio files in samples\ (one submenu per subfolder)
bool smpIsAudio(const std::wstring& f) {
    size_t dot = f.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring e = f.substr(dot);
    for (auto& c : e) c = towlower(c);
    return e == L".wav" || e == L".mp3" || e == L".flac";
}

void smpListDir(const std::wstring& dir, std::vector<std::wstring>& files, std::vector<std::wstring>& subdirs) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) subdirs.push_back(n);
        else if (smpIsAudio(n)) files.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    auto ci = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
    std::sort(files.begin(), files.end(), ci);
    std::sort(subdirs.begin(), subdirs.end(), ci);
}

std::wstring smpMenuText(const std::wstring& s) {   // '&' would underline the next letter in a menu
    std::wstring r;
    for (wchar_t c : s) { if (c == L'&') r += L'&'; r += c; }
    return r;
}

void smpAddLibraryItems(HMENU m, const std::wstring& dir, std::vector<std::wstring>& paths, int depth,
                        const std::wstring& current) {
    std::vector<std::wstring> files, subdirs;
    smpListDir(dir, files, subdirs);
    for (auto& s : subdirs) {
        if (depth >= 2) break;
        HMENU sub = CreatePopupMenu();
        smpAddLibraryItems(sub, dir + L"\\" + s, paths, depth + 1, current);
        if (GetMenuItemCount(sub) > 0) AppendMenuW(m, MF_POPUP | MF_STRING, (UINT_PTR)sub, smpMenuText(s).c_str());
        else DestroyMenu(sub);
    }
    for (auto& f : files) {
        paths.push_back(dir + L"\\" + f);
        std::wstring shown = smpBaseName(f);
        int root;
        smpParseRoot(shown, shown, root);
        AppendMenuW(m, MF_STRING | (shown == current ? MF_CHECKED : 0), paths.size(), smpMenuText(shown).c_str());
    }
}

void smpLibraryMenu(const RectF& at) {
    Instrument* I = smpI();
    if (!I) return;
    std::wstring dir = smpLibraryDir();
    if (dir.empty()) { smpMsg(L"No samples folder found next to the program"); return; }
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> paths;
    smpAddLibraryItems(m, dir, paths, 0, I->melPreset < 0 ? I->mName : L"");
    if (paths.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(empty - put wav / mp3 / flac files in the folder)");
    const UINT ID_OPEN = 60000;
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Open samples folder...");
    POINT pt = {(LONG)(at.X * gScale), (LONG)((at.Y + at.Height) * gScale)};
    ClientToScreen(gSmpWnd, &pt);
    UINT id = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, gSmpWnd, NULL);
    DestroyMenu(m);
    if (id == ID_OPEN) {
        ShellExecuteW(gSmpWnd, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
        smpMsg(L"Add your own sounds to this folder. Name them like \"My sound (C4).wav\" to set the root note.");
    } else if (id >= 1 && id <= paths.size()) {
        smpLoadMelodic(paths[id - 1], true);
    }
    InvalidateRect(gSmpWnd, NULL, FALSE);
}

void smpLoadPads(const std::vector<std::wstring>& files, int first) {
    Instrument* I = smpI();
    if (!I) return;
    int ok = 0;
    std::wstring failed;
    for (size_t i = 0; i < files.size() && first + (int)i < SMP_PADS; ++i) {
        DataPtr d = smpLoadFile(files[i]);
        if (!d) { failed = fileNameOf(files[i]); continue; }
        {
            std::lock_guard<std::mutex> lk(gMx);
            I->p.pads[first + i].data = d;
        }
        I->padName[first + i] = smpBaseName(files[i]);
        ++ok;
    }
    if (ok) I->kitPreset = -1;   // your own kit now
    gSmpSel = first;
    if (!failed.empty()) smpMsg(L"Could not read " + failed);
    else if (files.size() > (size_t)(SMP_PADS - first))
        smpMsg(L"Loaded " + std::to_wstring(ok) + L" pads - the remaining files did not fit (16 pads)");
    else if (ok == 1) smpMsg(L"Pad " + std::to_wstring(first + 1) + L": " + I->padName[first]);
    else smpMsg(L"Loaded " + std::to_wstring(ok) + L" samples into pads " + std::to_wstring(first + 1) + L"-" +
                std::to_wstring(first + ok));
}

void smpDetect() {
    Instrument* I = smpI();
    if (!I || !I->p.mData) { smpMsg(L"Load a sample first"); return; }
    float hz;
    if (!smpDetectPitch(I->p.mData->s, hz)) {
        smpMsg(L"No clear pitch found (noisy or inharmonic sound) - right-click the key that matches the sample");
        return;
    }
    double m = 69.0 + 12.0 * std::log2(hz / 440.0);
    int root = (int)std::lround(m);
    if (root < 0 || root > 127) { smpMsg(L"Detected pitch is out of range - right-click the key that matches the sample"); return; }
    float cents = (float)((m - root) * 100.0);
    {
        std::lock_guard<std::mutex> lk(gMx);
        I->p.mRoot = (float)root;
        I->p.mTune = -std::round(cents);
    }
    wchar_t s[200];
    swprintf(s, 200, L"Detected %.1f Hz = %ls %+.0f cents  ->  root %ls, fine tune %+.0f ct", hz,
             smpNoteName(root).c_str(), cents, smpNoteName(root).c_str(), -std::round(cents));
    smpMsg(s);
}

void smpTransport(bool rec) {
    DWORD before = gMsgTime;
    if (rec) {
        if (gRecState == REC_IDLE) {
            bool any = false;   // will anything record what is played here?
            for (auto& t : gTracks) if (t.arm && t.midi) any = true;
            transportRecord();
            if (gRecState != REC_IDLE && !any)
                smpMsg(L"Recording - but no MIDI track is armed (R), so the notes you play are not recorded");
        } else {
            transportRecord();
        }
    } else {
        if (gPlaying.load() || gRecState != REC_IDLE) transportStop();
        else transportPlay();
    }
    if (gMsgTime != before) smpMsg(gMsg);
}

// MIDI input (goes to the live MIDI track's instrument)
void CALLBACK smpMidiCallback(HMIDIIN, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR) {
    if (msg != MIM_DATA) return;
    int status = p1 & 0xFF, d1 = (p1 >> 8) & 0xFF, d2 = (p1 >> 16) & 0xFF, cmd = status & 0xF0;
    if (cmd == 0x90 && d2 > 0) smpNoteOn(d1, d2);
    else if (cmd == 0x80 || (cmd == 0x90 && d2 == 0)) smpNoteOff(d1);
    else if (cmd == 0xB0 && (d1 == 120 || d1 == 123)) smpAllOff();
}

void smpCloseMidi() {
    if (!gSmpMidi) return;
    midiInStop(gSmpMidi);
    midiInReset(gSmpMidi);
    midiInClose(gSmpMidi);
    gSmpMidi = NULL;
    smpAllOff();
}

int gSmpMidiIdx = -1;   // open MIDI input device, -1 = none

void smpOpenMidi(int idx) {
    smpCloseMidi();
    gSmpMidiIdx = -1;
    if (idx < 0) return;
    HMIDIIN h;
    if (midiInOpen(&h, (UINT)idx, (DWORD_PTR)smpMidiCallback, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        smpMsg(L"The MIDI device could not be opened (is another program using it?)");
        setMsg(L"The MIDI device could not be opened (is another program using it?)");
        if (gSmpMidiCombo) SendMessageW(gSmpMidiCombo, CB_SETCURSEL, 0, 0);
        return;
    }
    midiInStart(h);
    gSmpMidi = h;
    gSmpMidiIdx = idx;
}

void smpFillMidi() {   // lists the devices; the one already open stays selected
    SendMessageW(gSmpMidiCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(gSmpMidiCombo, CB_ADDSTRING, 0, (LPARAM)L"(No MIDI input)");
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps;
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            SendMessageW(gSmpMidiCombo, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
        else
            SendMessageW(gSmpMidiCombo, CB_ADDSTRING, 0, (LPARAM)L"(unknown device)");
    }
    SendMessageW(gSmpMidiCombo, CB_SETCURSEL, gSmpMidiIdx + 1, 0);
}

void samplerStartup() {   // the MIDI keyboard (list in the main window) plays the live MIDI / Synth track
    if (midiInGetNumDevs() > 0) smpOpenMidi(0);
    smpFillMidi();
}

// ============================================================================
// Sampler window: drawing
// ============================================================================
void smpDrawWave(Graphics& g, const AudioData* d, const RectF& r, const Color& col) {
    SolidBrush bg(C_CANVAS);
    g.FillRectangle(&bg, r);
    if (!d || d->s.empty()) return;
    int w = std::max(1, (int)r.Width);
    size_t N = d->s.size();
    double spp = (double)N / w;
    float mid = r.Y + r.Height / 2, amp = r.Height / 2 - 2;
    std::vector<PointF> pts(2 * w);
    for (int i = 0; i < w; ++i) {
        size_t s0 = std::min(N - 1, (size_t)(i * spp));
        size_t s1 = std::min(N, std::max(s0 + 1, (size_t)((i + 1) * spp)));
        float mn, mx;
        if (s1 - s0 >= 2 * (size_t)PEAK_BLOCK) {
            size_t b0 = s0 / PEAK_BLOCK, b1 = (s1 - 1) / PEAK_BLOCK;
            mn = d->pkMin[b0];
            mx = d->pkMax[b0];
            for (size_t b = b0 + 1; b <= b1; ++b) { mn = std::min(mn, d->pkMin[b]); mx = std::max(mx, d->pkMax[b]); }
        } else {
            mn = mx = d->s[s0];
            for (size_t s = s0 + 1; s < s1; ++s) { mn = std::min(mn, d->s[s]); mx = std::max(mx, d->s[s]); }
        }
        mn = clampf(mn, -1, 1);
        mx = clampf(mx, -1, 1);
        if (mx - mn < 1.0f / amp) { mx += 0.5f / amp; mn -= 0.5f / amp; }
        pts[i] = PointF(r.X + i, mid - mx * amp);
        pts[2 * w - 1 - i] = PointF(r.X + i, mid - mn * amp);
    }
    SolidBrush b(col);
    g.FillPolygon(&b, pts.data(), (INT)pts.size());
}

void smpDrawCtl(Graphics& g, const SmpCtl& c, const Color& col) {
    drawText(g, c.label, fLabel, C_DIM, RectF(c.r.X, c.r.Y, c.labelW - 6, c.r.Height), StringAlignmentNear);
    RectF s = smpSliderRect(c);
    bool bip = c.fmt == F_SEMI || c.fmt == F_CENTS || c.fmt == F_PAN;
    drawSlider(g, s, smpCtlPos(c), bip, col);
    drawText(g, smpFmt(c).c_str(), fLabel, C_TEXT, RectF(s.X + s.Width + 10, c.r.Y, 70, c.r.Height), StringAlignmentNear);
}

void smpDrawMelodic(Graphics& g, Instrument& I) {
    const SmpParams& P = I.p;
    RectF p = smpPanel();
    fillRound(g, p, 6, C_PANEL, &C_BORDER);
    // (the preset is highlighted at the top, the root note is the orange dot on the keyboard)
    std::wstring title = !P.mData ? L"No sample loaded" : I.melPreset >= 0 ? L"Preset sample" : I.mName;
    if (P.mData) {
        wchar_t s[64];
        swprintf(s, 64, L"   ·   %.2f s", (double)P.mData->s.size() / SR);
        title += s;
    }
    drawText(g, title.c_str(), fTitle, P.mData ? C_TEXT : C_DIM, RectF(p.X + 12, p.Y + 8, p.Width - 440, 26),
             StringAlignmentNear);
    drawButton(g, smpBtnLibrary(), L"Library  ▾");
    drawButton(g, smpBtnLoad(), L"Load...");
    drawButton(g, smpBtnDetect(), L"Detect root", P.mData != nullptr);
    drawToggle(g, smpBtnLoop(), L"Loop", P.mLoop, C_ACCENT);

    RectF wr = smpWaveRect();
    smpDrawWave(g, P.mData.get(), wr, C_ACCENT);
    if (!P.mData) {
        drawText(g, L"Pick a sound from Library, click Load... or drop an audio file (wav / mp3 / flac) on this window",
                 fLabel, C_DIM, wr);
    } else if (P.mLoop) {
        float lx = wr.X + P.mLoopStart * wr.Width;
        SolidBrush lb(Color(45, 255, 138, 61));
        g.FillRectangle(&lb, lx, wr.Y, wr.X + wr.Width - lx, wr.Height);
        Pen lp(C_ACCENT, 1.5f);
        g.DrawLine(&lp, lx, wr.Y, lx, wr.Y + wr.Height);
        drawText(g, L"LOOP", fSmall, C_ACCENT, RectF(lx + 4, wr.Y + 2, 40, 14), StringAlignmentNear);
    }

    std::wstring base = smpNoteName(12 * (gSmpOct + 1));
    std::wstring hint = L"PC keys:  Z S X D C V G B H N J M ,  (lower row, from " + base +
                        L")   Q 2 W 3 E R 5 T 6 Y 7 U I  (one octave up)   ·   - / =  octave   ·   "
                        L"click or drag on the keys   ·   orange dot = root note (right-click a key to move it)";
    drawText(g, hint.c_str(), fSmall, C_DIM, RectF(12, 336, gSW - 24, 22), StringAlignmentNear);

    int lo = 12 * (gSmpOct + 1), hi = lo + 28, root = (int)std::lround(P.mRoot);
    for (int pass = 0; pass < 2; ++pass) {   // white keys, then black keys on top
        for (int n = SMP_PIANO_LO; n <= SMP_PIANO_HI; ++n) {
            if (smpBlack(n) != (pass == 1)) continue;
            RectF k = smpKeyRect(n);
            bool held = I.held[n].load(), pc = n >= lo && n <= hi;
            Color c = pass == 0 ? (held ? C_ACCENT : pc ? Color(255, 238, 238, 240) : Color(255, 186, 189, 196))
                                : (held ? Color(255, 220, 110, 40) : pc ? Color(255, 46, 49, 57) : Color(255, 24, 26, 30));
            fillRound(g, k, 3, c);
            if (pass == 0 && n % 12 == 0)
                drawText(g, smpNoteName(n).c_str(), fSmall, Color(255, 90, 92, 100),
                         RectF(k.X, k.Y + k.Height - 20, k.Width, 16));
            if (n == root) {
                SolidBrush rb(held ? Color(255, 255, 255, 255) : C_ACCENT);
                float d = std::min(10.0f, k.Width * 0.5f);
                g.FillEllipse(&rb, k.X + k.Width / 2 - d / 2, k.Y + k.Height - (pass == 0 ? 34 : 16), d, d);
            }
        }
    }
}

void smpDrawKit(Graphics& g, Instrument& I) {
    const SmpParams& P = I.p;
    DWORD now = GetTickCount();
    for (int p = 0; p < SMP_PADS; ++p) {
        RectF r = smpPadRect(p);
        const SmpPad& pd = P.pads[p];
        const Color& tc = TRACK_COLORS[p % 8];
        bool has = pd.data != nullptr;
        float fl = clampf(1.0f - (now - gSmpPadFlash[p]) / 180.0f, 0, 1);
        Color fill = has ? mixColor(C_PANEL, tc, 0.16f + 0.6f * fl) : mixColor(C_PANEL, tc, 0.4f * fl);
        Color border = p == gSmpSel ? Color(255, 255, 255, 255) : C_BORDER;
        fillRound(g, r, 8, fill, &border);
        if (p == gSmpSel) {
            GraphicsPath gp;
            roundRect(gp, r, 8);
            Pen sp(Color(255, 255, 255, 255), 2.0f);
            g.DrawPath(&sp, &gp);
        }
        if (has) fillRound(g, RectF(r.X + 8, r.Y + 6, r.Width - 16, 3), 1.5f, tc);
        wchar_t key[2] = {(wchar_t)SMP_KIT_KEYS[p], 0};
        drawText(g, std::to_wstring(p + 1).c_str(), fSmall, C_DIM, RectF(r.X + 8, r.Y + 10, 30, 16), StringAlignmentNear);
        drawText(g, key, fLabel, has ? C_TEXT : C_DIM, RectF(r.X + r.Width - 30, r.Y + 10, 22, 16), StringAlignmentFar);
        drawText(g, has ? I.padName[p].c_str() : L"Empty", fLabel, has ? C_TEXT : C_DIM,
                 RectF(r.X + 6, r.Y + r.Height / 2 - 12, r.Width - 12, 24));
        int note = (int)std::lround(pd.note);
        drawText(g, smpNoteName(note).c_str(), fSmall, C_DIM, RectF(r.X + 8, r.Y + r.Height - 22, 40, 16),
                 StringAlignmentNear);
        // the usual (General MIDI) drum on this note, unless the pad already says the same
        if (note >= 36 && note <= 51 && !(has && I.padName[p] == SMP_GM[note - 36]))
            drawText(g, SMP_GM[note - 36], fSmall, Color(255, 100, 104, 112),
                     RectF(r.X + 44, r.Y + r.Height - 22, r.Width - 52, 16), StringAlignmentFar);
    }

    const SmpPad& pd = P.pads[gSmpSel];
    float x = smpKitX(), w = gSW - 12 - x;
    std::wstring title = L"Pad " + std::to_wstring(gSmpSel + 1) + L"   ·   " +
                         (pd.data ? I.padName[gSmpSel] : std::wstring(L"empty"));
    drawText(g, title.c_str(), fTitle, C_TEXT, RectF(x, 68, w, 26), StringAlignmentNear);
    drawButton(g, smpKitLoad(), L"Load...");
    drawButton(g, smpKitClear(), L"Clear", pd.data != nullptr);
    RectF wr = smpKitWave();
    smpDrawWave(g, pd.data.get(), wr, TRACK_COLORS[gSmpSel % 8]);
    if (!pd.data) drawText(g, L"Load a sample or drop a file on a pad", fLabel, C_DIM, wr);
    else {
        wchar_t s[32];
        swprintf(s, 32, L"%.2f s", (double)pd.data->s.size() / SR);
        drawText(g, s, fSmall, C_DIM, RectF(wr.X + 6, wr.Y + 2, 80, 14), StringAlignmentNear);
    }

    const wchar_t* help[] = {
        L"Keys:   Z X C V = pads 1-4    A S D F = 5-8    Q W E R = 9-12    1 2 3 4 = 13-16",
        L"Click a pad to play it  ·  double-click an empty pad or right-click any pad to load a sample",
        L"Drop several files on a pad: they fill that pad and the following ones",
        L"MIDI / piano roll: each pad plays on its MIDI note (default 36-51, the General MIDI drum notes)",
        L"Pads are one-shot: the whole sample plays even if you let go of the key",
    };
    float y = 250 + 4 * 34 + 14;
    for (auto h : help) {
        if (y + 18 > gSH - SMP_STATUS_H - 6) break;
        drawText(g, h, fSmall, C_DIM, RectF(x, y, w, 18), StringAlignmentNear);
        y += 20;
    }
}

void smpPaint(HDC hdc) {
    RECT rc;
    GetClientRect(gSmpWnd, &rc);
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
        Instrument* I = smpI();

        // top bar
        SolidBrush bar(C_PANEL);
        g.FillRectangle(&bar, 0.0f, 0.0f, gSW, 56.0f);
        Pen line(C_BORDER, 1.0f);
        g.DrawLine(&line, 0.0f, 55.5f, gSW, 55.5f);
        int mode = I ? I->p.mode : 0;
        drawToggle(g, smpTabMel, L"Melodic  ·  one sample", I && mode == 0, C_ACCENT);
        drawToggle(g, smpTabKit, L"Drum kit  ·  multi sample", I && mode == 1, C_ACCENT);
        drawTransportIcon(g, smpBtnPlay, 1, gPlaying.load());
        drawTransportIcon(g, smpBtnRec, 3, gRecState != REC_IDLE);
        if (I) {
            drawText(g, L"PRESET", fSmall, C_DIM, RectF(466, 12, 52, 32), StringAlignmentNear);
            int n = mode == 0 ? SMP_MEL_PRESET_N : SMP_KIT_PRESET_N, cur = mode == 0 ? I->melPreset : I->kitPreset;
            for (int i = 0; i < n; ++i)
                drawToggle(g, smpPresetBtn(i, mode), mode == 0 ? SMP_MEL_PRESETS[i].name : SMP_KIT_PRESETS[i].name, i == cur, C_ACCENT);
        }

        if (!I) {
            drawText(g, L"Select a MIDI track in the main window (or add one with  + MIDI ) - every MIDI track has its own Sampler.",
                     fLabel, C_DIM, RectF(12, 68, gSW - 24, gSH - 68 - SMP_STATUS_H - 12));
        } else {
            if (mode == 0) smpDrawMelodic(g, *I);
            else smpDrawKit(g, *I);
            std::vector<SmpCtl> cs;
            smpBuildCtls(cs);
            for (size_t i = 0; i < cs.size(); ++i)
                smpDrawCtl(g, cs[i], (i == 0 || mode == 0) ? C_ACCENT : TRACK_COLORS[gSmpSel % 8]);
        }

        // status bar
        RectF sr(0, gSH - SMP_STATUS_H, gSW, SMP_STATUS_H);
        g.FillRectangle(&bar, sr);
        g.DrawLine(&line, 0.0f, sr.Y + 0.5f, gSW, sr.Y + 0.5f);
        bool recent = !gSmpMsg.empty() && GetTickCount() - gSmpMsgTime < 6000;
        const wchar_t* hint = L"Space DAW play/stop  ·  Ctrl+R record  ·  Tab switch mode  ·  wheel / drag sliders, "
                              L"double-click resets  ·  to record notes: arm this MIDI track (R) in the main window";
        drawText(g, recent ? gSmpMsg.c_str() : hint, fSmall, recent ? C_TEXT : C_DIM,
                 RectF(12, sr.Y, gSW - 24, sr.Height), StringAlignmentNear);
    }
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ============================================================================
// Sampler window: input
// ============================================================================
enum { SD_NONE, SD_CTL, SD_LOOP, SD_NOTE } gSmpDrag = SD_NONE;
SmpCtl gSmpDragCtl;

void smpSetLoopFromX(float x) {
    Instrument* I = smpI();
    if (!I) return;
    RectF wr = smpWaveRect();
    std::lock_guard<std::mutex> lk(gMx);
    I->p.mLoopStart = clampf((x - wr.X) / wr.Width, 0, 0.99f);
}

void smpOnDown(float x, float y, bool dbl, bool right) {
    if (!right) {
        if (smpBtnPlay.Contains(x, y)) { smpTransport(false); return; }
        if (smpBtnRec.Contains(x, y)) { smpTransport(true); return; }
    }
    Instrument* I = smpI();
    if (!I) return;
    SmpParams& P = I->p;
    if (!right) {
        if (smpTabMel.Contains(x, y)) { smpSetMode(0); return; }
        if (smpTabKit.Contains(x, y)) { smpSetMode(1); return; }
        int n = P.mode == 0 ? SMP_MEL_PRESET_N : SMP_KIT_PRESET_N;
        for (int i = 0; i < n; ++i) {
            if (!smpPresetBtn(i, P.mode).Contains(x, y)) continue;
            bool ok = P.mode == 0 ? smpApplyMelPreset(*I, i) : smpApplyKitPreset(*I, i);
            if (!ok) smpMsg(L"Preset sounds not found - the presets folder must be next to the program (or one folder up)");
            else smpMsg(std::wstring(L"Preset: ") + (P.mode == 0 ? SMP_MEL_PRESETS[i].name : SMP_KIT_PRESETS[i].name) +
                        L"  -  load your own samples any time with Load... or by dropping files");
            return;
        }
        std::vector<SmpCtl> cs;
        smpBuildCtls(cs);
        for (auto& c : cs) {
            RectF s = smpSliderRect(c);
            s.Inflate(6, 2);
            if (!s.Contains(x, y)) continue;
            if (dbl) smpSetV(c, c.def);
            else { gSmpDrag = SD_CTL; gSmpDragCtl = c; smpCtlFromX(c, x); }
            return;
        }
    }

    if (P.mode == 0) {
        if (right) {   // right-click a key: that key is the sample's own pitch (root note)
            int n = smpPianoNoteAt(x, y);
            if (n < 0 || !P.mData) return;
            {
                std::lock_guard<std::mutex> lk(gMx);
                P.mRoot = (float)n;
                P.mTune = 0;
            }
            smpMsg(L"Root note: " + smpNoteName(n) + L" - this key now plays the sample at its own pitch");
            return;
        }
        if (smpBtnLibrary().Contains(x, y)) { smpLibraryMenu(smpBtnLibrary()); return; }
        if (smpBtnLoad().Contains(x, y)) {
            auto f = smpOpenDialog(false);
            if (!f.empty()) smpLoadMelodic(f[0]);
            return;
        }
        if (smpBtnDetect().Contains(x, y)) { smpDetect(); return; }
        if (smpBtnLoop().Contains(x, y)) {
            std::lock_guard<std::mutex> lk(gMx);
            P.mLoop = !P.mLoop;
            return;
        }
        if (smpWaveRect().Contains(x, y)) {
            if (!P.mData) {
                auto f = smpOpenDialog(false);
                if (!f.empty()) smpLoadMelodic(f[0]);
            } else if (P.mLoop) {
                smpSetLoopFromX(x);
                gSmpDrag = SD_LOOP;
            } else {   // audition at the root note
                gSmpMouseNote = (int)std::lround(P.mRoot);
                smpNoteOn(gSmpMouseNote, 100);
                gSmpDrag = SD_NOTE;
            }
            return;
        }
        int n = smpPianoNoteAt(x, y);
        if (n >= 0) {
            gSmpMouseNote = n;
            smpNoteOn(n, 100);
            gSmpDrag = SD_NOTE;
        }
        return;
    }

    // drum kit
    if (!right) {
        if (smpKitLoad().Contains(x, y)) {
            auto f = smpOpenDialog(true);
            if (!f.empty()) smpLoadPads(f, gSmpSel);
            return;
        }
        if (smpKitClear().Contains(x, y)) {
            {
                std::lock_guard<std::mutex> lk(gMx);
                P.pads[gSmpSel].data.reset();
            }
            I->padName[gSmpSel].clear();
            I->kitPreset = -1;
            return;
        }
        if (smpKitWave().Contains(x, y)) { smpPadHit(gSmpSel, 110); return; }
    }
    for (int p = 0; p < SMP_PADS; ++p) {
        if (!smpPadRect(p).Contains(x, y)) continue;
        gSmpSel = p;
        bool has = P.pads[p].data != nullptr;
        if (right || (dbl && !has)) {
            auto f = smpOpenDialog(true);
            if (!f.empty()) smpLoadPads(f, p);
        } else if (has) {
            smpPadHit(p, 110);
        } else {
            smpMsg(L"Empty pad - double-click or right-click it to load a sample, or drop a file on it");
        }
        return;
    }
}

void smpOnMove(float x, float y) {
    switch (gSmpDrag) {
    case SD_CTL: if (smpI()) smpCtlFromX(gSmpDragCtl, x); break;
    case SD_LOOP: smpSetLoopFromX(x); break;
    case SD_NOTE: {
        int n = smpPianoNoteAt(x, y);
        if (n >= 0 && n != gSmpMouseNote) {   // glissando over the keys
            if (gSmpMouseNote >= 0) smpNoteOff(gSmpMouseNote);
            gSmpMouseNote = n;
            smpNoteOn(n, 100);
        }
        break;
    }
    default: break;
    }
}

void smpEndDrag() {
    if (gSmpDrag == SD_NOTE && gSmpMouseNote >= 0) { smpNoteOff(gSmpMouseNote); gSmpMouseNote = -1; }
    gSmpDrag = SD_NONE;
}

int smpPianoKeyOffset(WPARAM vk) {   // same layout as the synth: two rows, Z row and Q row
    switch (vk) {
    case 'Z': return 0;  case 'S': return 1;  case 'X': return 2;  case 'D': return 3;
    case 'C': return 4;  case 'V': return 5;  case 'G': return 6;  case 'B': return 7;
    case 'H': return 8;  case 'N': return 9;  case 'J': return 10; case 'M': return 11;
    case VK_OEM_COMMA: return 12; case 'L': return 13; case VK_OEM_PERIOD: return 14;
    case VK_OEM_1: return 15;     case VK_OEM_2: return 16;
    case 'Q': return 12; case '2': return 13; case 'W': return 14; case '3': return 15;
    case 'E': return 16; case 'R': return 17; case '5': return 18; case 'T': return 19;
    case '6': return 20; case 'Y': return 21; case '7': return 22; case 'U': return 23;
    case 'I': return 24; case '9': return 25; case 'O': return 26; case '0': return 27;
    case 'P': return 28;
    }
    return -1;
}

int smpKitKey(WPARAM vk) {
    for (int i = 0; i < SMP_PADS; ++i) if ((WPARAM)SMP_KIT_KEYS[i] == vk) return i;
    return -1;
}

// Called from the main message loop for every message. Returns true if it was consumed.
bool samplerHandleKey(const MSG& m) {
    if (!gSmpWnd || (m.message != WM_KEYDOWN && m.message != WM_KEYUP)) return false;
    if (GetAncestor(m.hwnd, GA_ROOT) != gSmpWnd) return false;
    WPARAM vk = m.wParam & 0xFF;
    bool down = m.message == WM_KEYDOWN, repeat = (m.lParam & (1 << 30)) != 0;

    if (GetKeyState(VK_CONTROL) < 0) {
        if (vk == 'R') { if (down && !repeat) smpTransport(true); return true; }
        if (vk == 'Z') { if (down) undo(); return true; }
        return false;
    }
    if (down && vk == VK_SPACE) { if (!repeat) smpTransport(false); return true; }
    Instrument* I = smpI();
    if (!I) return false;
    if (down && vk == VK_TAB) { smpSetMode(1 - I->p.mode); return true; }

    if (!down) {
        int h = gSmpKeyHeld[vk];
        if (h < 0) return smpPianoKeyOffset(vk) >= 0 || smpKitKey(vk) >= 0;
        if (h < 1000) smpNoteOff(h);
        gSmpKeyHeld[vk] = -1;
        return true;
    }

    if (I->p.mode == 0) {
        if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { if (gSmpOct > 0) { smpReleaseKeys(); --gSmpOct; } return true; }
        if (vk == VK_OEM_PLUS || vk == VK_ADD) { if (gSmpOct < 7) { smpReleaseKeys(); ++gSmpOct; } return true; }
        int off = smpPianoKeyOffset(vk);
        if (off < 0) return false;
        if (repeat || gSmpKeyHeld[vk] >= 0) return true;
        int n = 12 * (gSmpOct + 1) + off;
        if (n > 127) return true;
        if (!I->p.mData) smpMsg(L"Load a sample first (Load... or drop a file on this window)");
        gSmpKeyHeld[vk] = n;
        smpNoteOn(n, 100);
        return true;
    }

    int p = smpKitKey(vk);
    if (p < 0) return false;
    if (repeat || gSmpKeyHeld[vk] >= 0) return true;
    gSmpKeyHeld[vk] = 1000 + p;
    gSmpSel = p;
    if (I->p.pads[p].data) smpPadHit(p, 110);
    return true;
}

LRESULT CALLBACK SmpWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        gSmpWnd = hwnd;
        for (int& h : gSmpKeyHeld) h = -1;
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, 1, 30, NULL);
        return 0;
    case WM_TIMER: {
        DWORD now = GetTickCount();
        if (Instrument* I = smpI())
            for (int p = 0; p < SMP_PADS; ++p) {
                int h = I->padHits[p].load();
                if (h != gSmpPadSeen[p]) { gSmpPadSeen[p] = h; gSmpPadFlash[p] = now; }
            }
        std::wstring t = smpWindowTitle();   // follows renames
        if (t != gSmpTitle) { gSmpTitle = t; SetWindowTextW(hwnd, t.c_str()); }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        POINT pt;
        DragQueryPoint(drop, &pt);
        float x = pt.x / gScale, y = pt.y / gScale;
        std::vector<std::wstring> files;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t f[MAX_PATH];
            if (DragQueryFileW(drop, i, f, MAX_PATH)) files.push_back(f);
        }
        DragFinish(drop);
        Instrument* I = smpI();
        if (files.empty() || !I) return 0;
        if (I->p.mode == 0) {
            smpLoadMelodic(files[0]);
        } else {
            int target = gSmpSel;
            for (int p = 0; p < SMP_PADS; ++p) if (smpPadRect(p).Contains(x, y)) target = p;
            std::sort(files.begin(), files.end());
            smpLoadPads(files, target);
        }
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_SIZE:
        gSW = LOWORD(lParam) / gScale;
        gSH = HIWORD(lParam) / gScale;
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = S(1000);
        mm->ptMinTrackSize.y = S(600);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        smpPaint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        SetFocus(hwnd);
        smpOnDown(GET_X_LPARAM(lParam) / gScale, GET_Y_LPARAM(lParam) / gScale, msg == WM_LBUTTONDBLCLK,
                  msg == WM_RBUTTONDOWN);
        if (gSmpDrag != SD_NONE) SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (gSmpDrag != SD_NONE) smpOnMove(GET_X_LPARAM(lParam) / gScale, GET_Y_LPARAM(lParam) / gScale);
        return 0;
    case WM_LBUTTONUP:
        smpEndDrag();
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        smpEndDrag();
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        float x = pt.x / gScale, y = pt.y / gScale;
        float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        std::vector<SmpCtl> cs;
        smpBuildCtls(cs);
        for (auto& c : cs)
            if (c.r.Contains(x, y)) { smpSetV(c, *c.v + notches * c.wheel); break; }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) { smpEndDrag(); smpReleaseKeys(); }
        return 0;
    case WM_CLOSE:   // keep the window (and the loaded samples), just hide it
        smpEndDrag();
        smpReleaseKeys();
        ShowWindow(hwnd, SW_HIDE);
        SetForegroundWindow(gWnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        gSmpWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Opens the Sampler window for the live MIDI track (activate = false: show it without taking the focus)
void samplerOpenWindow(bool activate = true) {
    if (gSmpWnd) {
        if (IsIconic(gSmpWnd)) ShowWindow(gSmpWnd, SW_RESTORE);
        ShowWindow(gSmpWnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        if (activate) SetForegroundWindow(gSmpWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = SmpWndProc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
        wc.lpszClassName = L"MiniDAWSampler";
        RegisterClassW(&wc);
        registered = true;
    }
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT r = {0, 0, S(1000), S(640)};
    AdjustWindowRect(&r, style, FALSE);
    RECT mr;
    GetWindowRect(gWnd, &mr);
    // owned by the main window: stays on top of it and closes with it
    gSmpTitle = smpWindowTitle();
    HWND h = CreateWindowW(L"MiniDAWSampler", gSmpTitle.c_str(), style, mr.left + S(60), mr.top + S(70),
                           r.right - r.left, r.bottom - r.top, gWnd, NULL, GetModuleHandleW(NULL), NULL);
    smpOnLiveChanged();   // pad flash counters of the current instrument
    ShowWindow(h, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetFocus(h);
}

void samplerShutdown() { smpCloseMidi(); }
