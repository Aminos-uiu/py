// synth.h - MiniDAW Synth window (the former MiniSynth): edits the instrument of the live Synth track.
// Every Synth track has its own synth (instrument.h: SynthParams / SynthEdit).
//
// The window is a sequence of steps (the keyboard at the bottom and "hold to preview" work in every step):
//   1 Waveform       - one cycle, played at every pitch: presets, invert / reverse / normalize, draw with
//                      the mouse, formula (Set / Add, e.g. sin(p) + 0.5*sin(3*p)), or one cycle of a sound file
//   2 Shape          - Smooth, Skew, Drive, Fold, Crush (applied live on top of the wave; with a whole sound
//                      they shape the recording, cycle by cycle at its root pitch)
//   3 Special        - optional: something grows while a key is held (pitch rise / drop, vibrato,
//                      tremolo, brighten), with amount, time to full strength and rate
//   4 ADSR           - attack, decay, sustain, release, volume
//   5 Play & Record  - arm the track, record what you play on the keyboard (PC keys, mouse, MIDI keyboard)
// Ctrl+Z in this window undoes changes to the wave.
//
// Also here: instOnLiveChanged / instOpenWindow - the Sampler and Synth windows follow the live track.

#pragma once
#include <cctype>
#include <random>
#include <stdexcept>

static const double kPiD = 3.14159265358979323846;

// The instrument this window edits: the live track's, if it is a Synth
Instrument* synI() { return gLiveInst && gLiveInst->kind == INST_SYNTH ? gLiveInst.get() : nullptr; }

// ============================================================================
// The waveform
// ============================================================================
float synSampleBase(const std::vector<float>& w, double phase) {   // linear interpolation, wraps around
    phase -= std::floor(phase);
    double idx = phase * SYN_N;
    int i0 = (int)idx;
    float frac = (float)(idx - i0);
    float a = w[i0 & (SYN_N - 1)], b = w[(i0 + 1) & (SYN_N - 1)];
    return a + (b - a) * frac;
}

float synFoldWave(float t) {   // triangle wave folder: values beyond +-1 are reflected back
    float u = (t + 1.0f) * 0.25f;
    u -= std::floor(u);
    return 1.0f - 4.0f * std::fabs(u - 0.5f);
}

bool synShapeNeutral(const SynthEdit& e) {
    return e.smooth <= 0.0f && std::fabs(e.skew - 0.5f) < 0.001f && e.drive <= 0.0f && e.fold <= 0.0f && e.crush <= 0.0f;
}

double synWholePeriod(const SynthEdit& e) {   // one cycle of the whole sound at its root pitch (samples at SR)
    return SR / (440.0 * std::exp2((e.wholeRoot - 69.0) / 12.0));
}

// The shape knobs applied to the whole sound. They do what they do to a single-cycle wave, cycle by cycle:
// one cycle = one period at the sound's root pitch, so Smooth and Skew act on the same harmonics as in wave mode.
DataPtr synShapeWhole(const SynthEdit& e) {
    if (!e.whole || synShapeNeutral(e)) return e.whole;
    const std::vector<float>& src = e.whole->s;
    size_t N = src.size();
    float peak = 1e-6f;
    for (float v : src) peak = std::max(peak, std::fabs(v));
    double P = synWholePeriod(e);
    std::vector<float> w(N);
    // 1) Skew: the same phase warp as on the wave, repeated every period
    if (std::fabs(e.skew - 0.5f) >= 0.001f) {
        double m = 0.5 + (e.skew - 0.5) * 0.9;
        for (size_t n = 0; n < N; ++n) {
            double c = std::floor(n / P) * P, u = (n - c) / P;
            double uw = u < m ? 0.5 * u / m : 0.5 + 0.5 * (u - m) / (1.0 - m);
            double pos = std::min((double)(N - 1), c + uw * P);
            size_t i0 = (size_t)pos;
            float fr = (float)(pos - i0), a = src[i0], b = src[std::min(N - 1, i0 + 1)];
            w[n] = (a + (b - a) * fr) / peak;
        }
    } else {
        for (size_t n = 0; n < N; ++n) w[n] = src[n] / peak;
    }
    // 2) Smooth: box blur twice; the radius is the wave mode's (up to 1/8 cycle) in samples of this sound
    int radius = (int)std::lround(e.smooth * e.smooth * 128.0 / SYN_N * P);
    if (radius > 0 && N > 1) {
        std::vector<double> cs(N + 1);
        for (int pass = 0; pass < 2; ++pass) {
            cs[0] = 0;
            for (size_t i = 0; i < N; ++i) cs[i + 1] = cs[i] + w[i];
            for (size_t i = 0; i < N; ++i) {
                size_t a = i >= (size_t)radius ? i - radius : 0, b = std::min(N, i + radius + 1);
                w[i] = (float)((cs[b] - cs[a]) / (b - a));
            }
        }
    }
    // 3) Drive  4) Fold  5) Crush: the same curves as on the wave (the sound is normalized to its peak)
    if (e.drive > 0.0f) {
        float g = 1.0f + e.drive * 9.0f, n = std::tanh(g);
        for (float& v : w) v = std::tanh(v * g) / n;
    }
    if (e.fold > 0.0f) {
        float g = 1.0f + e.fold * 5.0f;
        for (float& v : w) v = synFoldWave(v * g);
    }
    if (e.crush > 0.0f) {
        float levels = 2.0f + std::pow(1.0f - e.crush, 3.0f) * 126.0f;
        for (float& v : w) v = std::round((v + 1.0f) * 0.5f * (levels - 1.0f)) / (levels - 1.0f) * 2.0f - 1.0f;
    }
    for (float& v : w) v = clampf(v, -1.0f, 1.0f) * peak;
    auto d = std::make_shared<AudioData>();
    d->append(w.data(), N);
    return d;
}

// Where step 2 shows the whole sound: the loudest few cycles in its first 1.5 s
size_t synWholeViewStart(const SynthEdit& e) {
    if (!e.whole) return 0;
    const std::vector<float>& s = e.whole->s;
    size_t P = (size_t)std::max(1.0, synWholePeriod(e)), win = 3 * P;
    if (s.size() <= win) return 0;
    size_t lim = std::min(s.size() - win, (size_t)(1.5 * SR)), best = 0;
    double bestE = -1;
    for (size_t a = 0; a <= lim; a += std::max<size_t>(1, P / 2)) {
        double en = 0;
        for (size_t i = a; i < a + win; ++i) en += s[i] * s[i];
        if (en > bestE) { bestE = en; best = a; }
    }
    return best;
}

// Rebuild the final wavetable from the edited wave + shape knobs, hand it to the audio thread.
// A whole sound is reshaped as well (the shape knobs act on it then).
void synUpdateWave(Instrument& I) {
    SynthEdit& e = I.se;
    std::vector<float> w(SYN_N);
    // 1) Skew: move the middle of the cycle left/right (phase warp)
    float m = 0.5f + (e.skew - 0.5f) * 0.9f;
    for (int i = 0; i < SYN_N; ++i) {
        float x = (float)i / SYN_N;
        float xw = x < m ? 0.5f * x / m : 0.5f + 0.5f * (x - m) / (1.0f - m);
        w[i] = synSampleBase(e.base, xw);
    }
    // 2) Smooth: circular box blur, applied twice
    int radius = (int)(e.smooth * e.smooth * 128.0f);
    if (radius > 0) {
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> t = w;
            for (int i = 0; i < SYN_N; ++i) {
                float sum = 0;
                for (int k = -radius; k <= radius; ++k) sum += t[(i + k) & (SYN_N - 1)];
                w[i] = sum / (2 * radius + 1);
            }
        }
    }
    // 3) Drive: soft saturation
    if (e.drive > 0.0f) {
        float g = 1.0f + e.drive * 9.0f, n = std::tanh(g);
        for (float& v : w) v = std::tanh(v * g) / n;
    }
    // 4) Fold: wave folding
    if (e.fold > 0.0f) {
        float g = 1.0f + e.fold * 5.0f;
        for (float& v : w) v = synFoldWave(v * g);
    }
    // 5) Crush: reduce the number of amplitude levels
    if (e.crush > 0.0f) {
        float levels = 2.0f + std::pow(1.0f - e.crush, 3.0f) * 126.0f;
        for (float& v : w) v = std::round((v + 1.0f) * 0.5f * (levels - 1.0f)) / (levels - 1.0f) * 2.0f - 1.0f;
    }
    // "Brighten" morphs into the same wave folded harder (more harmonics); its amount sets how hard
    float g2 = 1.0f + e.specAmountK * 5.0f;
    DataPtr shaped = I.sp.smp ? synShapeWhole(e) : nullptr;   // the GUI thread is the only writer of sp.smp
    std::lock_guard<std::mutex> lk(gMx);
    for (int i = 0; i < SYN_N; ++i) {
        I.sp.table[i] = clampf(w[i], -1.0f, 1.0f);
        I.sp.table2[i] = synFoldWave(I.sp.table[i] * g2);
    }
    if (shaped) I.sp.smp = shaped;
}

float synAttackMs(float k) { return 0.5f * std::pow(4000.0f, k); }    // 0.5 ms .. 2 s
float synDecayMs(float k) { return 5.0f * std::pow(1000.0f, k); }     // 5 ms .. 5 s
float synReleaseMs(float k) { return 5.0f * std::pow(1000.0f, k); }   // 5 ms .. 5 s
float synSpecTime(float k) { return 0.05f * std::pow(200.0f, k); }    // 0.05 .. 10 s
float synSpecRate(float k) { return 0.5f * std::pow(30.0f, k); }      // 0.5 .. 15 Hz

void synApplyParams(Instrument& I) {   // ADSR / volume / special knobs -> audio thread
    std::lock_guard<std::mutex> lk(gMx);
    I.sp.attackMs = synAttackMs(I.se.attackK);
    I.sp.decayMs = synDecayMs(I.se.decayK);
    I.sp.sustain = I.se.sustainK;
    I.sp.releaseMs = synReleaseMs(I.se.releaseK);
    I.sp.volume = I.se.volumeK;
    I.sp.specAmount = I.se.specAmountK;
    I.sp.specTime = synSpecTime(I.se.specTimeK);
    I.sp.specRate = synSpecRate(I.se.specRateK);
}
void synApplyEnv(Instrument& I) { synApplyParams(I); }

void synPushUndo(Instrument& I) {
    I.se.undo.push_back(I.se.base);
    if (I.se.undo.size() > 60) I.se.undo.erase(I.se.undo.begin());
}

void synUndo(Instrument& I) {
    if (I.se.undo.empty()) return;
    I.se.base = I.se.undo.back();
    I.se.undo.pop_back();
    synUpdateWave(I);
}

void synNormalize(std::vector<float>& w, bool removeDc) {
    if (removeDc) {
        double mean = 0;
        for (float v : w) mean += v;
        mean /= w.size();
        for (float& v : w) v -= (float)mean;
    }
    float peak = 0;
    for (float v : w) peak = std::max(peak, std::fabs(v));
    if (peak > 1e-6f) for (float& v : w) v /= peak;
}

enum { SYP_SINE, SYP_SAW, SYP_SQUARE, SYP_TRI, SYP_NOISE };
void synSetPreset(Instrument& I, int kind) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    for (int i = 0; i < SYN_N; ++i) {
        float p = (float)i / SYN_N, s;
        switch (kind) {
        case SYP_SINE:   s = std::sin(2 * kPi * p); break;
        case SYP_SAW:    s = 2 * p - 1; break;
        case SYP_SQUARE: s = p < 0.5f ? 1.0f : -1.0f; break;
        case SYP_TRI:    s = 1 - 4 * std::fabs(p - 0.5f); break;
        default:         s = uni(rng); break;
        }
        I.se.base[i] = s;
    }
}

// ============================================================================
// Formula parser: "sin(p) + 0.5*sin(3*p)", w / w(x) = current wave
// ============================================================================
struct SynFormula {
    std::string src;
    const std::vector<float>* cur = nullptr;
    size_t i = 0;
    double x = 0, p = 0;
    std::mt19937 rng{12345};

    [[noreturn]] void fail(const std::string& m) {
        throw std::runtime_error(m + " (position " + std::to_string(i + 1) + ")");
    }
    char peek() { while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i; return i < src.size() ? src[i] : 0; }

    double eval(double xv) {
        x = xv; p = 2 * kPiD * xv; i = 0;
        double v = expr();
        if (peek()) fail(std::string("Unexpected '") + src[i] + "'");
        return v;
    }
    double expr() {
        double v = term();
        for (;;) {
            char c = peek();
            if (c == '+')      { ++i; v += term(); }
            else if (c == '-') { ++i; v -= term(); }
            else return v;
        }
    }
    double term() {
        double v = unary();
        for (;;) {
            char c = peek();
            if (c == '*')      { ++i; v *= unary(); }
            else if (c == '/') { ++i; v /= unary(); }
            else if (c == '%') { ++i; v = std::fmod(v, unary()); }
            else return v;
        }
    }
    double unary() {
        char c = peek();
        if (c == '-') { ++i; return -unary(); }
        if (c == '+') { ++i; return unary(); }
        return power();
    }
    double power() {
        double b = primary();
        if (peek() == '^') { ++i; return std::pow(b, unary()); }
        return b;
    }
    double primary() {
        char c = peek();
        if (std::isdigit((unsigned char)c) || c == '.') {
            char* end;
            double v = std::strtod(src.c_str() + i, &end);
            if (end == src.c_str() + i) fail("Bad number");
            i = end - src.c_str();
            return v;
        }
        if (c == '(') {
            ++i;
            double v = expr();
            if (peek() != ')') fail("Missing ')'");
            ++i;
            return v;
        }
        if (std::isalpha((unsigned char)c)) {
            std::string name;
            while (i < src.size() && (std::isalnum((unsigned char)src[i]) || src[i] == '_'))
                name += (char)std::tolower((unsigned char)src[i++]);
            if (peek() == '(') {
                ++i;
                std::vector<double> args;
                if (peek() != ')') {
                    for (;;) {
                        args.push_back(expr());
                        if (peek() == ',') { ++i; continue; }
                        break;
                    }
                }
                if (peek() != ')') fail("Missing ')'");
                ++i;
                return call(name, args);
            }
            if (name == "x" || name == "t") return x;
            if (name == "p")  return p;
            if (name == "w")  return synSampleBase(*cur, x);
            if (name == "pi") return kPiD;
            if (name == "e")  return std::exp(1.0);
            fail("Unknown name '" + name + "'");
        }
        if (!c) fail("Unexpected end of formula");
        fail(std::string("Unexpected '") + c + "'");
    }
    double call(const std::string& f, const std::vector<double>& a) {
        auto need = [&](size_t n) {
            if (a.size() != n) fail(f + "() needs " + std::to_string(n) + " argument(s)");
        };
        auto frac = [](double v) { return v - std::floor(v); };
        if (f == "sin")   { need(1); return std::sin(a[0]); }
        if (f == "cos")   { need(1); return std::cos(a[0]); }
        if (f == "tan")   { need(1); return std::tan(a[0]); }
        if (f == "abs")   { need(1); return std::fabs(a[0]); }
        if (f == "sqrt")  { need(1); return std::sqrt(a[0]); }
        if (f == "exp")   { need(1); return std::exp(a[0]); }
        if (f == "log")   { need(1); return std::log(a[0]); }
        if (f == "floor") { need(1); return std::floor(a[0]); }
        if (f == "ceil")  { need(1); return std::ceil(a[0]); }
        if (f == "sign")  { need(1); return (a[0] > 0) - (a[0] < 0); }
        if (f == "saw")   { need(1); return 2 * frac(a[0]) - 1; }
        if (f == "sq" || f == "square") { need(1); return frac(a[0]) < 0.5 ? 1.0 : -1.0; }
        if (f == "tri")   { need(1); return 1 - 4 * std::fabs(frac(a[0]) - 0.5); }
        if (f == "w")     { need(1); return synSampleBase(*cur, a[0]); }
        if (f == "rnd" || f == "noise") { need(0); return std::uniform_real_distribution<double>(-1, 1)(rng); }
        if (f == "min")   { need(2); return a[0] < a[1] ? a[0] : a[1]; }
        if (f == "max")   { need(2); return a[0] > a[1] ? a[0] : a[1]; }
        if (f == "pow")   { need(2); return std::pow(a[0], a[1]); }
        if (f == "mod")   { need(2); return std::fmod(a[0], a[1]); }
        fail("Unknown function '" + f + "'");
    }
};

// add=false: replace the wave; add=true: add the formula to the current wave.
// Returns an empty string on success, otherwise an error message.
std::string synApplyFormula(Instrument& I, const std::string& text, bool add) {
    if (text.find_first_not_of(" \t") == std::string::npos) return "Please type a formula";
    SynFormula f;
    f.src = text;
    f.cur = &I.se.base;
    std::vector<float> vals(SYN_N);
    try {
        for (int i = 0; i < SYN_N; ++i) {
            double v = f.eval((double)i / SYN_N);
            vals[i] = std::isfinite(v) ? (float)v : 0.0f;
        }
    } catch (const std::exception& e) {
        return e.what();
    }
    synPushUndo(I);
    if (add) {
        for (int i = 0; i < SYN_N; ++i) vals[i] += I.se.base[i];
        float peak = 0;
        for (float v : vals) peak = std::max(peak, std::fabs(v));
        if (peak > 1.0f) for (float& v : vals) v /= peak;
    } else {
        synNormalize(vals, false);
    }
    I.se.base = vals;
    synUpdateWave(I);
    return "";
}

// ============================================================================
// Sound file import: take one cycle out of a sound file
// ============================================================================
float synImpLenMs(const SynthEdit& e) { return 0.5f * std::pow(400.0f, e.impLen); }   // 0.5 ms .. 200 ms
void synSetImpLenSamples(SynthEdit& e, double samples) {
    double ms = samples / e.wavRate * 1000.0;
    e.impLen = clampf((float)(std::log(ms / 0.5) / std::log(400.0)), 0.0f, 1.0f);
}
size_t synImpLenSamples(const SynthEdit& e) {
    size_t n = (size_t)(synImpLenMs(e) / 1000.0f * e.wavRate);
    if (n < 4) n = 4;
    if (n > e.wav.size()) n = e.wav.size();
    return n;
}
size_t synImpStart(const SynthEdit& e) {
    size_t len = synImpLenSamples(e);
    return (size_t)(e.impPos * (double)(e.wav.size() - len));
}

void synImportCycle(Instrument& I) {
    SynthEdit& e = I.se;
    if (e.wav.size() < 8) return;
    size_t start = synImpStart(e), len = synImpLenSamples(e);
    std::vector<float> w(SYN_N);
    for (int i = 0; i < SYN_N; ++i) {
        double t = start + (double)i * len / SYN_N;
        size_t i0 = (size_t)t;
        if (i0 + 1 >= e.wav.size()) i0 = e.wav.size() - 2;
        float frac = (float)(t - i0);
        w[i] = e.wav[i0] + (e.wav[i0 + 1] - e.wav[i0]) * frac;
    }
    if (start + len < e.wav.size()) {   // the cycle repeats: bend it so its end meets its start (no click per cycle)
        float d = e.wav[start + len] - e.wav[start];
        for (int i = 0; i < SYN_N; ++i) w[i] -= d * i / SYN_N;
    }
    synNormalize(w, true);
    e.base = w;
    synUpdateWave(I);
}

// Length of one period at `start` with autocorrelation (samples, 0 if not found)
double synDetectPeriod(const SynthEdit& e, size_t start) {
    const int W = 2048;
    int minLag = (int)(e.wavRate / 2000.0f), maxLag = (int)(e.wavRate / 30.0f);
    if (minLag < 4) minLag = 4;
    if (start + W + maxLag >= e.wav.size()) {
        if (e.wav.size() < (size_t)(W + maxLag + 1)) return 0;
        start = e.wav.size() - W - maxLag - 1;
    }
    const float* d = &e.wav[start];
    std::vector<double> corr(maxLag + 1, 0.0);
    double e0 = 0;
    for (int i = 0; i < W; ++i) e0 += d[i] * d[i];
    if (e0 < 1e-9) return 0;
    double best = -1;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double c = 0, e1 = 0;
        for (int i = 0; i < W; ++i) { c += d[i] * d[i + lag]; e1 += d[i + lag] * d[i + lag]; }
        corr[lag] = e1 > 1e-12 ? c / std::sqrt(e0 * e1) : 0;
        if (corr[lag] > best) best = corr[lag];
    }
    if (best < 0.3) return 0;
    for (int lag = minLag; lag <= maxLag; ++lag)   // shortest lag that is nearly as good (avoids octave errors)
        if (corr[lag] >= best * 0.9 && (lag == maxLag || corr[lag] >= corr[lag + 1]) && corr[lag] >= corr[lag - 1])
            return lag;
    return 0;
}

void synAutoLength(SynthEdit& e) {
    double per = synDetectPeriod(e, synImpStart(e));
    if (per > 0) {
        synSetImpLenSamples(e, per);
        wchar_t b[96];
        swprintf(b, 96, L"Detected pitch: %.1f Hz", e.wavRate / per);
        e.importMsg = b;
    } else {
        e.importMsg = L"No clear pitch here - set LENGTH by hand";
    }
}

void synBuildOverview(SynthEdit& e) {
    const int COLS = 256;
    e.ovMin.assign(COLS, 0.0f);
    e.ovMax.assign(COLS, 0.0f);
    for (int c = 0; c < COLS; ++c) {
        size_t a = e.wav.size() * c / COLS, b = e.wav.size() * (c + 1) / COLS;
        if (b <= a) b = a + 1;
        float mn = 1, mx = -1;
        for (size_t i = a; i < b && i < e.wav.size(); ++i) { mn = std::min(mn, e.wav[i]); mx = std::max(mx, e.wav[i]); }
        e.ovMin[c] = mn;
        e.ovMax[c] = mx;
    }
}

bool synLoadWav(Instrument& I, const std::wstring& path) {
    SynthEdit& e = I.se;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);   // mono float, native rate
    ma_decoder dec;
    if (ma_decoder_init_file_w(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
        e.importMsg = L"Could not open file (supported: WAV, MP3, FLAC)";
        return false;
    }
    std::vector<float> data;
    float rate = (float)dec.outputSampleRate;
    const size_t maxFrames = (size_t)rate * 600;
    float buf[4096];
    for (;;) {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
        if (got == 0) break;
        data.insert(data.end(), buf, buf + got);
        if (data.size() >= maxFrames) break;
    }
    ma_decoder_uninit(&dec);
    if (data.size() < 64) {
        e.importMsg = L"File contains no audio";
        return false;
    }
    e.wav.swap(data);
    e.wavRate = rate;
    e.wavName = fileNameOf(path);
    synBuildOverview(e);

    // Start at the loudest part of the file and detect one period there
    size_t bestStart = 0;
    double bestE = -1;
    for (size_t s = 0; s + 2048 < e.wav.size(); s += 1024) {
        double en = 0;
        for (size_t i = s; i < s + 2048; ++i) en += e.wav[i] * e.wav[i];
        if (en > bestE) { bestE = en; bestStart = s; }
    }
    e.impLen = 0.5f;
    e.impPos = (float)((double)bestStart / (e.wav.size() - synImpLenSamples(e)));
    synAutoLength(e);
    e.impPos = clampf((float)((double)bestStart / (e.wav.size() - synImpLenSamples(e))), 0.0f, 1.0f);
    synPushUndo(I);
    synImportCycle(I);
    return true;
}

// ============================================================================
// Synth window: layout (logical pixels, fixed size) and state
// ============================================================================
HWND gSynWnd = NULL, gSynFormula = NULL;
HFONT gSynMono = NULL;
const int ID_SYN_FORMULA = 301;
// The step bar is the top bar of the window. The content below it is laid out in coordinates that start
// at y = 104 and is drawn SY_SHIFT higher (synY converts mouse positions to layout coordinates).
const float SY_W = 960, SY_H = 720, SY_STATUS_H = 24, SY_TOP_H = 56, SY_SHIFT = 48;
const float SY_WIN_H = SY_H - SY_SHIFT;   // the window's client height
float synY(float y) { return y >= SY_TOP_H ? y + SY_SHIFT : y; }
const int SY_PIANO_LO = 36, SY_PIANO_HI = 96;   // C2 .. C7

const Color SY_TRACK(255, 58, 62, 72), SY_KNOB_HI(255, 60, 65, 75), SY_WAVE(255, 79, 209, 197), SY_ERROR(255, 240, 100, 100);

// Steps
enum { STEP_WAVE, STEP_SHAPE, STEP_SPECIAL, STEP_ADSR, STEP_PLAY, STEP_COUNT };
const wchar_t* SY_STEP_NAMES[STEP_COUNT] = {L"1   Waveform", L"2   Shape", L"3   Special", L"4   ADSR", L"5   Play & Record"};
const wchar_t* SY_STEP_HINTS[STEP_COUNT] = {
    L"Step 1: choose the waveform - a preset, draw it, type a formula or take a whole sound (Samples / Load...)",
    L"Step 2: fine-tune the wave with the shape knobs (grey line = the wave before shaping)",
    L"Step 3 (optional): a special function that grows while a key is held - hold the preview button to hear it",
    L"Step 4: the envelope - how the volume starts (attack, decay), holds (sustain) and ends (release)",
    L"",
};
int gSynStep = STEP_WAVE;
RectF synStepRect(int i) { return RectF(12 + i * 166.0f, 11, 150, 34); }

// Step 1
enum { SYT_SINE, SYT_SAW, SYT_SQUARE, SYT_TRI, SYT_NOISE, SYT_INVERT, SYT_REVERSE, SYT_NORMALIZE, SYT_UNDO, SYT_COUNT };
const wchar_t* SY_TOOL_NAMES[SYT_COUNT] = {L"Sine", L"Saw", L"Square", L"Triangle", L"Noise",
                                           L"Invert", L"Reverse", L"Normalize", L"Undo"};
RectF syToolBtn[SYT_COUNT];
RectF synCanvas() { return gSynStep == STEP_WAVE ? RectF(20, 146, 630, 196) : RectF(20, 112, 920, 214); }
const RectF syWavPanel(660, 146, 280, 216), sySamplesBtn(672, 172, 88, 24), syLoadBtn(766, 172, 66, 24),
            syAutoBtn(838, 172, 90, 24);
const RectF syWholeBtn(790, 150, 78, 20), syCycleBtn(872, 150, 56, 20);   // whole sound / one cycle
const RectF syOverview(672, 202, 256, 56);
const RectF sySetBtn(546, 370, 50, 26), syAddBtn(600, 370, 50, 26);
// Step 2
const RectF syResetShape(830, 336, 110, 24);
// Step 3
const wchar_t* SY_SPEC_NAMES[SPEC_COUNT] = {L"None", L"Pitch rise", L"Pitch drop", L"Vibrato", L"Tremolo", L"Brighten"};
const wchar_t* SY_SPEC_DESC[SPEC_COUNT] = {
    L"The sound stays the same while a key is held",
    L"The pitch slides up the longer you hold the key",
    L"The pitch slides down the longer you hold the key",
    L"A pitch wobble that grows the longer you hold",
    L"A volume wobble that grows the longer you hold",
    L"The tone gets brighter / harsher the longer you hold",
};
RectF synSpecRect(int i) { return RectF(20, 112 + i * 46.0f, 300, 40); }
const RectF sySpecGraph(340, 112, 600, 236);
// Step 4
const RectF syEnvGraph(20, 112, 920, 230);
// Step 5
const RectF syPlayPanel(20, 112, 560, 370), syRecPanel(600, 112, 340, 370);
const RectF syArmBtn(616, 146, 308, 32), syClickBtn(616, 186, 308, 32);   // record / play: top bar
// Always there
const RectF syBtnPlay(864, 12, 36, 32), syBtnRec(906, 12, 36, 32);
const RectF syPreviewBtn(20, 496, 180, 26), syOctDown(212, 496, 26, 26), syOctUp(318, 496, 26, 26);
const RectF syBackBtn(760, 496, 84, 26), syNextBtn(850, 496, 90, 26);
const RectF syPiano(20, 530, 920, 150);
RectF syKeyRect[128];

std::wstring gSynFormulaMsg = L"Type a formula, then Set (replace) or Add (mix in)";
bool gSynFormulaErr = false;
std::wstring gSynMsg, gSynTitle;
DWORD gSynMsgTime = 0;
void synMsg(const std::wstring& m) { gSynMsg = m; gSynMsgTime = GetTickCount(); }

// Knobs (their values live in the instrument's SynthEdit, so the list is built for the current one and step)
enum { SK_SHAPE, SK_PARAM, SK_SPECAMT, SK_IMPORT };
struct SynKnob {
    const wchar_t* name;
    float* value;
    float def;
    float cx, cy, r;
    int kind;
};

void synBuildKnobs(Instrument& I, std::vector<SynKnob>& ks) {
    SynthEdit& e = I.se;
    ks.clear();
    switch (gSynStep) {
    case STEP_WAVE:
        if (e.wav.empty() || I.sp.smp) break;   // the cycle knobs (not used by a whole sound)
        ks.push_back({L"POSITION", &e.impPos, 0.0f, 740.0f, 322.0f, 17.0f, SK_IMPORT});
        ks.push_back({L"LENGTH", &e.impLen, 0.5f, 860.0f, 322.0f, 17.0f, SK_IMPORT});
        break;
    case STEP_SHAPE: {   // the wave, or the whole sound
        const wchar_t* names[5] = {L"SMOOTH", L"SKEW", L"DRIVE", L"FOLD", L"CRUSH"};
        float* vals[5] = {&e.smooth, &e.skew, &e.drive, &e.fold, &e.crush};
        float defs[5] = {0.0f, 0.5f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 5; ++i) ks.push_back({names[i], vals[i], defs[i], 120.0f + i * 180.0f, 420.0f, 24.0f, SK_SHAPE});
        break;
    }
    case STEP_SPECIAL:
        if (I.sp.spec == SPEC_NONE) break;
        ks.push_back({L"AMOUNT", &e.specAmountK, 0.5f, 500.0f, 420.0f, 24.0f, SK_SPECAMT});
        ks.push_back({L"TIME TO FULL", &e.specTimeK, 0.696f, 680.0f, 420.0f, 24.0f, SK_PARAM});
        if (I.sp.spec == SPEC_VIBRATO || I.sp.spec == SPEC_TREMOLO)
            ks.push_back({L"RATE", &e.specRateK, 0.705f, 860.0f, 420.0f, 24.0f, SK_PARAM});
        break;
    case STEP_ADSR:
        ks.push_back({L"ATTACK", &e.attackK, 0.216f, 120.0f, 420.0f, 24.0f, SK_PARAM});
        ks.push_back({L"DECAY", &e.decayK, 0.593f, 300.0f, 420.0f, 24.0f, SK_PARAM});
        ks.push_back({L"SUSTAIN", &e.sustainK, 1.0f, 480.0f, 420.0f, 24.0f, SK_PARAM});
        ks.push_back({L"RELEASE", &e.releaseK, 0.401f, 660.0f, 420.0f, 24.0f, SK_PARAM});
        ks.push_back({L"VOLUME", &e.volumeK, 0.7f, 840.0f, 420.0f, 24.0f, SK_PARAM});
        break;
    }
}

void synBuildLayout() {
    for (int i = 0; i < SYT_COUNT; ++i) {
        float x = 20.0f + i * 78.0f + (i >= SYT_INVERT ? 14.0f : 0.0f) + (i >= SYT_UNDO ? 14.0f : 0.0f);
        syToolBtn[i] = RectF(x, 112.0f, 72.0f, 26.0f);
    }
    int whites = 0;
    for (int n = SY_PIANO_LO; n <= SY_PIANO_HI; ++n) if (!smpBlack(n)) ++whites;
    float ww = syPiano.Width / whites;
    int wi = 0;
    for (int n = SY_PIANO_LO; n <= SY_PIANO_HI; ++n) {
        if (!smpBlack(n)) { syKeyRect[n] = RectF(syPiano.X + wi * ww, syPiano.Y, ww, syPiano.Height); ++wi; }
        else syKeyRect[n] = RectF(syPiano.X + wi * ww - ww * 0.32f, syPiano.Y, ww * 0.64f, syPiano.Height * 0.62f);
    }
}

std::wstring synMs(float ms) {
    wchar_t b[32];
    if (ms >= 1000) swprintf(b, 32, L"%.2f s", ms / 1000);
    else if (ms >= 10) swprintf(b, 32, L"%.0f ms", ms);
    else swprintf(b, 32, L"%.1f ms", ms);
    return b;
}

std::wstring synSpecAmountText(const Instrument& I) {
    wchar_t b[48];
    float a = I.se.specAmountK;
    switch (I.sp.spec) {
    case SPEC_PITCH_UP:   swprintf(b, 48, L"+%.1f st", a * 24); break;
    case SPEC_PITCH_DOWN: swprintf(b, 48, L"-%.1f st", a * 24); break;
    case SPEC_VIBRATO:    swprintf(b, 48, L"±%.2f st", a * 2); break;
    default:              swprintf(b, 48, L"%.0f%%", a * 100); break;
    }
    return b;
}

void synFormatKnob(const Instrument& I, const SynKnob& k, wchar_t* b, int sz) {
    const SynthEdit& e = I.se;
    float v = *k.value;
    if (k.value == &e.skew) swprintf(b, sz, L"%+.0f%%", (v - 0.5f) * 200.0f);
    else if (k.value == &e.crush) {
        if (v <= 0.0f) swprintf(b, sz, L"off");
        else swprintf(b, sz, L"%d levels", (int)std::round(2.0f + std::pow(1.0f - v, 3.0f) * 126.0f));
    } else if (k.value == &e.attackK) swprintf(b, sz, L"%ls", synMs(synAttackMs(v)).c_str());
    else if (k.value == &e.decayK) swprintf(b, sz, L"%ls", synMs(synDecayMs(v)).c_str());
    else if (k.value == &e.releaseK) swprintf(b, sz, L"%ls", synMs(synReleaseMs(v)).c_str());
    else if (k.value == &e.specAmountK) swprintf(b, sz, L"%ls", synSpecAmountText(I).c_str());
    else if (k.value == &e.specTimeK) swprintf(b, sz, L"%.2f s", synSpecTime(v));
    else if (k.value == &e.specRateK) swprintf(b, sz, L"%.1f Hz", synSpecRate(v));
    else if (k.value == &e.impPos) swprintf(b, sz, L"%.2f s", e.wav.empty() ? 0.0 : synImpStart(e) / e.wavRate);
    else if (k.value == &e.impLen) {
        float ms = synImpLenMs(e);
        swprintf(b, sz, L"%.1f ms · %.0f Hz", ms, 1000.0f / ms);
    } else swprintf(b, sz, L"%.0f%%", v * 100.0f);
}

void synKnobChanged(Instrument& I, const SynKnob& k) {
    switch (k.kind) {
    case SK_SHAPE: synUpdateWave(I); break;
    case SK_PARAM: synApplyParams(I); break;
    case SK_SPECAMT: synApplyParams(I); synUpdateWave(I); break;   // Brighten's target wave depends on it
    default: I.se.importMsg.clear(); synImportCycle(I); break;
    }
}

int synHitKnob(const std::vector<SynKnob>& ks, float x, float y) {
    for (size_t i = 0; i < ks.size(); ++i) {
        float dx = x - ks[i].cx, dy = y - ks[i].cy, r = ks[i].r + 6;
        if (dx * dx + dy * dy <= r * r) return (int)i;
    }
    return -1;
}

int synHitPiano(float x, float y) {
    if (!syPiano.Contains(x, y)) return -1;
    for (int n = SY_PIANO_LO; n <= SY_PIANO_HI; ++n) if (smpBlack(n) && syKeyRect[n].Contains(x, y)) return n;
    for (int n = SY_PIANO_LO; n <= SY_PIANO_HI; ++n) if (!smpBlack(n) && syKeyRect[n].Contains(x, y)) return n;
    return -1;
}

int synVelocityFromY(int note, float y) {
    const RectF& r = syKeyRect[note];
    return 40 + (int)(clampf((y - r.Y) / r.Height, 0.0f, 1.0f) * 87.0f);
}

// Mouse / keyboard state
int gSynDragKnob = -1, gSynHoverKnob = -1;
float gSynDragY = 0, gSynDragVal = 0;
bool gSynDrawing = false, gSynOverviewDrag = false, gSynPianoDrag = false;
int gSynLastIdx = -1;
float gSynLastVal = 0;
int gSynMouseNote = -1, gSynPreviewNote = -1, gSynOct = 4, gSynPcHeld[256];

int synPreviewPitch() { return 12 * (gSynOct + 1); }   // C of the PC-key octave

// ============================================================================
// Synth window: actions
// ============================================================================
void synRunTool(Instrument& I, int t) {
    if (t == SYT_UNDO) { synUndo(I); return; }
    synPushUndo(I);
    std::vector<float>& b = I.se.base;
    switch (t) {
    case SYT_SINE: case SYT_SAW: case SYT_SQUARE: case SYT_TRI: case SYT_NOISE: synSetPreset(I, t - SYT_SINE + SYP_SINE); break;
    case SYT_INVERT:    for (float& v : b) v = -v; break;
    case SYT_REVERSE:   for (int i = 1; i < SYN_N / 2; ++i) std::swap(b[i], b[SYN_N - i]); break;
    case SYT_NORMALIZE: synNormalize(b, true); break;
    }
    synUpdateWave(I);
}

void synRunFormula(Instrument& I, bool add) {
    int len = GetWindowTextLengthW(gSynFormula);
    std::wstring w(len, L'\0');
    if (len > 0) GetWindowTextW(gSynFormula, &w[0], len + 1);
    std::string text(w.begin(), w.end());   // formulas are ASCII
    std::string err = synApplyFormula(I, text, add);
    gSynFormulaErr = !err.empty();
    gSynFormulaMsg = gSynFormulaErr ? std::wstring(err.begin(), err.end()) : (add ? L"Added to the wave" : L"Wave replaced");
}

bool synLoadFile(Instrument& I, const std::wstring& path);

void synChooseWav(Instrument& I) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = gSynWnd;
    ofn.lpstrFilter = L"Audio files (*.wav;*.mp3;*.flac)\0*.wav;*.mp3;*.flac\0All files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) synLoadFile(I, file);
}

// "Whole sound" mode: the synth plays the whole recording (with its ADSR and special function) instead of
// the single-cycle wave. Switching it off goes back to the wave.
bool synWhole(const Instrument& I) { return I.sp.smp != nullptr; }

void synSetWhole(Instrument& I, bool on) {
    if (on && !I.se.whole) return;
    DataPtr shaped = on ? synShapeWhole(I.se) : nullptr;   // with the shape knobs (step 2)
    std::lock_guard<std::mutex> lk(gMx);
    SynthParams& sp = I.sp;
    sp.smp = shaped;
    sp.smpRoot = I.se.wholeRoot;
    sp.smpLoop = I.se.wholeLoop;
    sp.smpLoopStart = 0.35f;
    double len = on ? (double)I.se.whole->s.size() : 0;   // the converted sounds fade out over their last 0.2 s
    sp.smpLoopEnd = len > SR ? (float)((len - 0.25 * SR) / len) : 1.0f;
}

bool synIsSustained(const AudioData& d) {   // still loud near the end (organ, winds) -> loop it while held
    size_t N = d.s.size();
    if (N < (size_t)SR) return false;
    auto rms = [&](double a, double b) {
        double s = 0;
        size_t i0 = (size_t)(a * N), i1 = (size_t)(b * N);
        for (size_t i = i0; i < i1; ++i) s += d.s[i] * d.s[i];
        return std::sqrt(s / std::max<size_t>(1, i1 - i0));
    };
    return rms(0.6, 0.8) >= 0.5 * rms(0.15, 0.35);
}

// Takes the whole recording for "Whole sound" mode and switches it on. root < 0: detect the pitch.
void synTakeWhole(Instrument& I, const std::wstring& path, float root, int loop /* -1 = auto */) {
    DataPtr d = smpLoadFile(path);
    if (!d) return;
    if (root < 0) {
        float hz;
        root = smpDetectPitch(d->s, hz) ? (float)(69.0 + 12.0 * std::log2(hz / 440.0)) : 60.0f;
    }
    I.se.whole = d;
    I.se.wholeRoot = root;
    I.se.wholeLoop = loop < 0 ? synIsSustained(*d) : loop != 0;
    I.se.wholeView = synWholeViewStart(I.se);
    synSetWhole(I, true);
}

// Load... / dropped file: whole sound (pitch detected); its "1 cycle" is taken where it is loudest
bool synLoadFile(Instrument& I, const std::wstring& path) {
    if (!synLoadWav(I, path)) return false;
    std::wstring name = smpBaseName(path);
    int root = -1;
    if (!smpParseRoot(name, name, root)) root = -1;
    synTakeWhole(I, path, (float)root, -1);
    synMsg(name + L": every key plays the whole sound (root " + smpNoteName((int)std::lround(I.se.wholeRoot)) +
           L"). Shape it in step 2, or \"1 cycle\" turns it into a wave you can draw on.");
    return true;
}

// Samples button: a Sampler preset (Bell, Organ, Piano) or a sound of the samples\ library. It plays as a
// whole recording; "1 cycle" takes one cycle out of it instead. Their pitch is known, so the cycle length is
// set exactly (no detection needed).
void synLoadSample(Instrument& I, const std::wstring& path, const std::wstring& name, int root, float tuneCents,
                   int loop = -1) {
    if (!synLoadWav(I, path)) return;
    SynthEdit& e = I.se;
    e.wavName = name;
    synTakeWhole(I, path, root >= 0 ? root - tuneCents / 100.0f : -1.0f, loop);
    if (root >= 0) {
        double hz = 440.0 * std::pow(2.0, (root - 69) / 12.0 - tuneCents / 1200.0);   // tune corrects the sample
        double P = e.wavRate / hz;                                                     // one period (samples)
        synSetImpLenSamples(e, P);
        const int K = 4;   // cycles averaged (smooths out noise and partials that are not harmonic)
        size_t N = e.wav.size(), per = (size_t)std::ceil(P);
        if (N > (K + 2) * per + 16) {
            float peak = 0;
            for (float v : e.wav) peak = std::max(peak, std::fabs(v));
            size_t onset = 0;
            while (onset < N && std::fabs(e.wav[onset]) < 0.1f * peak) ++onset;
            // the steadiest spot: one cycle looks most like the next, and the sound is still loud
            size_t best = onset, a = onset + (size_t)(0.03 * e.wavRate);
            size_t b = std::min(N - (K + 1) * per - 2, onset + (size_t)(1.5 * e.wavRate));
            double bestScore = -1;
            for (size_t s = a; s < b; s += std::max<size_t>(1, (size_t)(0.005 * e.wavRate))) {
                double c = 0, e0 = 0, e1 = 0;
                for (size_t i = 0; i < per; ++i) {
                    double x = e.wav[s + i], y = e.wav[s + (size_t)std::lround(P) + i];
                    c += x * y; e0 += x * x; e1 += y * y;
                }
                if (e0 < 1e-9 || e1 < 1e-9) continue;
                double rms = std::sqrt(e0 / per) / peak;
                double score = c / std::sqrt(e0 * e1) * std::sqrt(std::min(1.0, rms * 3));
                if (score > bestScore) { bestScore = score; best = s; }
            }
            if (bestScore > 0) {
                e.impPos = clampf((float)((double)best / (N - synImpLenSamples(e))), 0.0f, 1.0f);
                // average K cycles (fractional period) and join the ends
                std::vector<float> w(SYN_N, 0.0f);
                auto at = [&](double t) { size_t i0 = (size_t)t; float f = (float)(t - i0); return e.wav[i0] + (e.wav[i0 + 1] - e.wav[i0]) * f; };
                double endV = 0;
                for (int k = 0; k < K; ++k) {
                    for (int i = 0; i < SYN_N; ++i) w[i] += at(best + k * P + (double)i * P / SYN_N) / K;
                    endV += at(best + (k + 1) * P) / K;
                }
                float d = (float)(endV - w[0]);
                for (int i = 0; i < SYN_N; ++i) w[i] -= d * i / SYN_N;
                synNormalize(w, true);
                e.base = w;
                synUpdateWave(I);
            } else {
                synImportCycle(I);
            }
        } else {
            synImportCycle(I);
        }
        e.importMsg = name + L"  ·  one cycle of " + smpNoteName(root);
    }
    synMsg(name + L": every key plays the whole sound, with your shape, ADSR and special function. "
                  L"\"1 cycle\" turns it into a wave you can draw on instead.");
}

void synSampleMenu(Instrument& I, const RectF& at) {
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> paths;
    std::wstring cur = smpBaseName(I.se.wavName);
    int r0;
    smpParseRoot(cur, cur, r0);
    std::wstring pre = smpPresetDir();
    int nPre = 0;
    if (!pre.empty()) {
        for (int i = 0; i < SMP_MEL_PRESET_N; ++i) {
            paths.push_back(pre + L"\\" + SMP_MEL_PRESETS[i].file);
            AppendMenuW(m, MF_STRING | (cur == SMP_MEL_PRESETS[i].name ? MF_CHECKED : 0), paths.size(), SMP_MEL_PRESETS[i].name);
        }
        nPre = SMP_MEL_PRESET_N;
    }
    std::wstring lib = smpLibraryDir();
    if (!lib.empty()) {
        if (nPre) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        smpAddLibraryItems(m, lib, paths, 0, cur);   // same menu as the Sampler's Library button
    }
    if (paths.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(no presets or samples folder found)");
    const UINT ID_OPEN = 60000;
    if (!lib.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, ID_OPEN, L"Open samples folder...");
    }
    POINT pt = {(LONG)(at.X * gScale), (LONG)((at.Y + at.Height - SY_SHIFT) * gScale)};   // layout -> window
    ClientToScreen(gSynWnd, &pt);
    UINT id = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, gSynWnd, NULL);
    DestroyMenu(m);
    if (id == ID_OPEN) {
        ShellExecuteW(gSynWnd, L"open", lib.c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (id >= 1 && id <= paths.size()) {
        int i = (int)id - 1;
        if (i < nPre) {
            synLoadSample(I, paths[i], SMP_MEL_PRESETS[i].name, SMP_MEL_PRESETS[i].root, SMP_MEL_PRESETS[i].tune,
                          SMP_MEL_PRESETS[i].loop ? 1 : 0);
        } else {
            std::wstring name = smpBaseName(paths[i]);
            int root = -1;
            if (!smpParseRoot(name, name, root)) root = -1;   // no note in the name: detect it
            synLoadSample(I, paths[i], name, root, 0);
        }
    }
    InvalidateRect(gSynWnd, NULL, FALSE);
}

void synDrawAt(Instrument& I, float x, float y, bool first) {
    RectF r = synCanvas();
    int idx = (int)((x - r.X) / r.Width * SYN_N);
    idx = idx < 0 ? 0 : (idx > SYN_N - 1 ? SYN_N - 1 : idx);
    float val = clampf((0.5f - (y - r.Y) / r.Height) / 0.45f, -1.0f, 1.0f);
    std::vector<float>& b = I.se.base;
    if (first || gSynLastIdx < 0) {
        b[idx] = val;
    } else {
        int a = gSynLastIdx, c = idx;
        float va = gSynLastVal, vc = val;
        if (a > c) { std::swap(a, c); std::swap(va, vc); }
        for (int i = a; i <= c; ++i) b[i] = c == a ? vc : va + (vc - va) * (float)(i - a) / (c - a);
    }
    gSynLastIdx = idx;
    gSynLastVal = val;
    synUpdateWave(I);
}

void synSetPosFromOverview(Instrument& I, float x) {
    I.se.impPos = clampf((x - syOverview.X) / syOverview.Width, 0.0f, 1.0f);
    I.se.importMsg.clear();
    synImportCycle(I);
}

void synReleasePcKeys() {
    for (int i = 0; i < 256; ++i)
        if (gSynPcHeld[i] >= 0) { smpNoteOff(gSynPcHeld[i]); gSynPcHeld[i] = -1; }
}

void synEndMouse() {
    gSynDragKnob = -1;
    gSynDrawing = gSynOverviewDrag = gSynPianoDrag = false;
    gSynLastIdx = -1;
    if (gSynMouseNote >= 0) { smpNoteOff(gSynMouseNote); gSynMouseNote = -1; }
    if (gSynPreviewNote >= 0) { smpNoteOff(gSynPreviewNote); gSynPreviewNote = -1; }
}

void synSetStep(int s) {
    if (s < 0 || s >= STEP_COUNT || s == gSynStep) return;
    synEndMouse();
    gSynStep = s;
    gSynHoverKnob = -1;
    if (gSynFormula) ShowWindow(gSynFormula, synI() && s == STEP_WAVE ? SW_SHOW : SW_HIDE);
    if (gSynWnd) { SetFocus(gSynWnd); InvalidateRect(gSynWnd, NULL, FALSE); }
}

void synSetSpecial(Instrument& I, int s) {
    {
        std::lock_guard<std::mutex> lk(gMx);
        I.sp.spec = s;
    }
    synApplyParams(I);
}

void synTransport(bool rec) {
    DWORD before = gMsgTime;
    if (rec) transportRecord();
    else if (gPlaying.load() || gRecState != REC_IDLE) transportStop();
    else transportPlay();
    if (gMsgTime != before) synMsg(gMsg);
}

void synToggleArm() {
    Track* t = trackById(gLiveTrackId);
    if (!t) return;
    std::lock_guard<std::mutex> lk(gMx);
    t->arm = !t->arm;
}

void synRecord() {   // step 5: arms the track if needed, then record / stop recording
    Track* t = trackById(gLiveTrackId);
    if (!t) return;
    if (gRecState == REC_IDLE && !t->arm) {
        std::lock_guard<std::mutex> lk(gMx);
        t->arm = true;
    }
    synTransport(true);
}

// ============================================================================
// Synth window: drawing
// ============================================================================
void synDrawKnob(Graphics& g, const Instrument& I, const SynKnob& k, bool hot) {
    float v = *k.value, r = k.r;
    drawText(g, k.name, fLabel, C_TEXT, RectF(k.cx - 60, k.cy - r - 20, 120, 16));
    Pen track(SY_TRACK, 4.0f);
    track.SetStartCap(LineCapRound); track.SetEndCap(LineCapRound);
    g.DrawArc(&track, k.cx - r, k.cy - r, 2 * r, 2 * r, 135.0f, 270.0f);
    if (v > 0.002f) {
        Pen arc(C_ACCENT, 4.0f);
        arc.SetStartCap(LineCapRound); arc.SetEndCap(LineCapRound);
        g.DrawArc(&arc, k.cx - r, k.cy - r, 2 * r, 2 * r, 135.0f, 270.0f * v);
    }
    float br = r - 6;
    SolidBrush body(hot ? SY_KNOB_HI : C_BTN);
    g.FillEllipse(&body, k.cx - br, k.cy - br, 2 * br, 2 * br);
    float a = (135.0f + 270.0f * v) * kPi / 180.0f;
    Pen ptr(C_TEXT, 2.2f);
    ptr.SetStartCap(LineCapRound); ptr.SetEndCap(LineCapRound);
    g.DrawLine(&ptr, k.cx + std::cos(a) * 3, k.cy + std::sin(a) * 3, k.cx + std::cos(a) * (br - 3), k.cy + std::sin(a) * (br - 3));
    wchar_t buf[48];
    synFormatKnob(I, k, buf, 48);
    drawText(g, buf, fSmall, hot ? C_ACCENT : C_DIM, RectF(k.cx - 70, k.cy + r + 3, 140, 16));
}

void synDrawWaveIn(Graphics& g, const Instrument& I, const RectF& r, bool showBase, float width) {
    if (I.sp.smp && showBase && gSynStep == STEP_SHAPE && I.se.whole) {
        // whole sound in step 2: three cycles, close up - grey = the recording, colour = after the shape knobs
        const std::vector<float>& a = I.se.whole->s;
        const std::vector<float>& b = I.sp.smp->s;
        double P = synWholePeriod(I.se), span = 3 * P;
        size_t v0 = std::min(I.se.wholeView, a.size() > (size_t)span + 2 ? a.size() - (size_t)span - 2 : 0);
        float peak = 1e-6f;
        for (float x : a) peak = std::max(peak, std::fabs(x));
        const int M = 630;
        std::vector<PointF> pa(M), pb(M);
        for (int j = 0; j < M; ++j) {
            size_t i = std::min(a.size() - 1, v0 + (size_t)(span * j / (M - 1)));
            float x = r.X + r.Width * j / (M - 1);
            pa[j] = PointF(x, r.Y + r.Height * (0.5f - 0.45f * a[i] / peak));
            pb[j] = PointF(x, r.Y + r.Height * (0.5f - 0.45f * (i < b.size() ? b[i] : 0.0f) / peak));
        }
        Pen cyc(C_GRID, 1.0f);
        for (int k = 1; k < 3; ++k) g.DrawLine(&cyc, r.X + r.Width * k / 3, r.Y + 4, r.X + r.Width * k / 3, r.Y + r.Height - 4);
        if (!synShapeNeutral(I.se)) {
            Pen basePen(Color(110, 138, 143, 152), 1.4f);
            g.DrawLines(&basePen, pa.data(), M);
        }
        Pen pen(SY_WAVE, width);
        pen.SetLineJoin(LineJoinRound);
        g.DrawLines(&pen, pb.data(), M);
        return;
    }
    if (I.sp.smp) {   // whole sound: the recording
        const std::vector<float>& s = I.sp.smp->s;
        int cols = (int)std::max(8.0f, r.Width / 2);
        Pen pen(SY_WAVE, 1.6f);
        float peak = 1e-6f;
        for (float v : s) peak = std::max(peak, std::fabs(v));
        for (int c = 0; c < cols; ++c) {
            size_t a = s.size() * c / cols, b = std::max(a + 1, s.size() * (c + 1) / cols);
            float mn = 0, mx = 0;
            for (size_t i = a; i < b && i < s.size(); ++i) { mn = std::min(mn, s[i]); mx = std::max(mx, s[i]); }
            float x = r.X + r.Width * (c + 0.5f) / cols;
            float y1 = r.Y + r.Height * (0.5f - 0.45f * mx / peak), y2 = r.Y + r.Height * (0.5f - 0.45f * mn / peak);
            g.DrawLine(&pen, x, y1, x, std::max(y2, y1 + 1));
        }
        return;
    }
    const int N = 630;
    static PointF basePts[N], outPts[N];
    bool differs = false;
    for (int j = 0; j < N; ++j) {
        int idx = (int)((float)j / N * SYN_N);
        float bv = I.se.base[idx], ov = I.sp.table[idx];
        if (std::fabs(bv - ov) > 1e-4f) differs = true;
        float x = r.X + r.Width * j / (N - 1);
        basePts[j] = PointF(x, r.Y + r.Height * (0.5f - 0.45f * bv));
        outPts[j]  = PointF(x, r.Y + r.Height * (0.5f - 0.45f * ov));
    }
    if (showBase && differs) {   // what you drew (faint) under the shaped result
        Pen basePen(Color(110, 138, 143, 152), 1.4f);
        g.DrawLines(&basePen, basePts, N);
    }
    Pen pen(SY_WAVE, width);
    pen.SetLineJoin(LineJoinRound);
    g.DrawLines(&pen, outPts, N);
}

void synDrawCanvas(Graphics& g, const Instrument& I) {
    RectF r = synCanvas();
    fillRound(g, r, 8, C_CANVAS, &C_BORDER);
    Pen grid(C_GRID, 1.0f);
    for (int i = 1; i < 8; ++i) g.DrawLine(&grid, r.X + r.Width * i / 8, r.Y + 4, r.X + r.Width * i / 8, r.Y + r.Height - 4);
    for (int i = 1; i < 4; ++i) g.DrawLine(&grid, r.X + 4, r.Y + r.Height * i / 4, r.X + r.Width - 4, r.Y + r.Height * i / 4);
    synDrawWaveIn(g, I, r, true, 2.2f);
    const wchar_t* label = I.sp.smp && gSynStep == STEP_SHAPE
                               ? L"WHOLE SOUND  ·  3 cycles, close up  ·  grey = before the shape knobs (they shape the whole recording)"
                         : I.sp.smp ? L"WHOLE SOUND  ·  the recording every key plays (switch to 1 cycle to draw on a wave)"
                         : gSynStep == STEP_WAVE ? L"WAVEFORM  ·  click and drag to draw"
                                                 : L"WAVEFORM  ·  grey = before the shape knobs  ·  you can still draw on it";
    drawText(g, label, fSmall, C_DIM, RectF(r.X, r.Y + r.Height + 1, r.Width, 14), StringAlignmentNear);
}

void synDrawWavPanel(Graphics& g, const Instrument& I) {
    const SynthEdit& e = I.se;
    fillRound(g, syWavPanel, 8, C_PANEL, &C_BORDER);
    drawText(g, L"FROM A SOUND", fTitle, C_ACCENT, RectF(syWavPanel.X + 12, syWavPanel.Y + 4, 220, 20), StringAlignmentNear);
    drawButton(g, sySamplesBtn, L"Samples ▾");
    drawButton(g, syLoadBtn, L"Load...");
    drawButton(g, syAutoBtn, L"Auto length", !e.wav.empty());
    const RectF& o = syOverview;
    fillRound(g, o, 4, C_CANVAS, &C_BORDER);
    if (e.wav.empty()) {
        drawText(g, L"No file - load or drop one", fSmall, C_DIM, o);
    } else {
        double n = (double)e.wav.size();
        float x1 = o.X + o.Width * (float)(synImpStart(e) / n);
        float x2 = o.X + o.Width * (float)((synImpStart(e) + synImpLenSamples(e)) / n);
        if (x2 - x1 < 2) x2 = x1 + 2;
        SolidBrush sel(Color(90, 255, 138, 61));
        g.FillRectangle(&sel, x1, o.Y + 1, x2 - x1, o.Height - 2);
        Pen pen(SY_WAVE, 1.0f);
        int cols = (int)e.ovMin.size();
        for (int c = 0; c < cols; ++c) {
            float x = o.X + o.Width * (c + 0.5f) / cols;
            float y1 = o.Y + o.Height * (0.5f - 0.45f * clampf(e.ovMax[c], -1, 1));
            float y2 = o.Y + o.Height * (0.5f - 0.45f * clampf(e.ovMin[c], -1, 1));
            if (y2 - y1 < 1) y2 = y1 + 1;
            g.DrawLine(&pen, x, y1, x, y2);
        }
    }
    if (e.whole) {
        bool w = I.sp.smp != nullptr;
        drawToggle(g, syWholeBtn, L"Whole sound", w, C_ACCENT);
        drawToggle(g, syCycleBtn, L"1 cycle", !w, C_ACCENT);
    }
    if (I.sp.smp) {   // whole sound: no cycle to pick
        drawText(g, e.wavName.c_str(), fSmall, C_DIM, RectF(o.X, o.Y + o.Height + 2, o.Width, 16), StringAlignmentNear);
        wchar_t b[160];
        swprintf(b, 160, L"Every key plays the whole sound.\nRoot %ls  ·  %ls", smpNoteName((int)std::lround(I.sp.smpRoot)).c_str(),
                 I.sp.smpLoop ? L"loops while held" : L"plays to its end");
        drawText(g, b, fSmall, C_TEXT, RectF(o.X, o.Y + o.Height + 26, o.Width, 40), StringAlignmentNear);
        drawText(g, L"1 cycle: take one cycle to draw on it", fSmall, C_DIM, RectF(o.X, o.Y + o.Height + 70, o.Width, 16),
                 StringAlignmentNear);
        return;
    }
    const std::wstring& info = e.importMsg.empty() ? e.wavName : e.importMsg;
    drawText(g, info.c_str(), fSmall, C_DIM, RectF(o.X, o.Y + o.Height + 2, o.Width, 16), StringAlignmentNear);
}

// Step 3: what the special function does over the time a key is held
void synDrawSpecGraph(Graphics& g, const Instrument& I) {
    const RectF& r = sySpecGraph;
    fillRound(g, r, 8, C_CANVAS, &C_BORDER);
    int s = I.sp.spec;
    if (s == SPEC_NONE) {
        drawText(g, L"No special function - choose one on the left (or keep None and go to the next step)", fLabel, C_DIM, r);
        return;
    }
    float amt = I.se.specAmountK, T = synSpecTime(I.se.specTimeK), rate = synSpecRate(I.se.specRateK);
    float span = std::max(1.0f, T * 1.3f);
    RectF p(r.X + 46, r.Y + 26, r.Width - 62, r.Height - 52);
    Pen axis(C_BORDER, 1.0f), grid(C_GRID, 1.0f);
    g.DrawLine(&axis, p.X, p.Y + p.Height, p.X + p.Width, p.Y + p.Height);
    g.DrawLine(&axis, p.X, p.Y, p.X, p.Y + p.Height);
    // y range and labels
    float lo, hi;
    const wchar_t* unit;
    switch (s) {
    case SPEC_PITCH_UP:   lo = 0; hi = std::max(1.0f, amt * 24); unit = L"pitch (semitones)"; break;
    case SPEC_PITCH_DOWN: lo = -std::max(1.0f, amt * 24); hi = 0; unit = L"pitch (semitones)"; break;
    case SPEC_VIBRATO:    hi = std::max(0.5f, amt * 2); lo = -hi; unit = L"pitch (semitones)"; break;
    case SPEC_TREMOLO:    lo = 0; hi = 1; unit = L"volume"; break;
    default:              lo = 0; hi = 1; unit = L"brightness (morph)"; break;
    }
    auto Y = [&](float v) { return p.Y + p.Height * (1 - (v - lo) / (hi - lo)); };
    if (lo < 0 && hi > 0) g.DrawLine(&grid, p.X, Y(0), p.X + p.Width, Y(0));
    // "full strength" marker
    float xf = p.X + p.Width * std::min(1.0f, T / span);
    Pen mark(Color(120, 255, 138, 61), 1.0f);
    mark.SetDashStyle(DashStyleDash);
    g.DrawLine(&mark, xf, p.Y, xf, p.Y + p.Height);
    wchar_t b[64];
    swprintf(b, 64, L"full after %.2f s", T);
    drawText(g, b, fSmall, C_ACCENT, RectF(xf + 4, p.Y, 140, 14), StringAlignmentNear);
    // the curve
    const int N = 400;
    std::vector<PointF> pts(N);
    for (int i = 0; i < N; ++i) {
        float t = span * i / (N - 1), m = std::min(1.0f, T > 0.001f ? t / T : 1.0f), v;
        switch (s) {
        case SPEC_PITCH_UP:   v = m * amt * 24; break;
        case SPEC_PITCH_DOWN: v = -m * amt * 24; break;
        case SPEC_VIBRATO:    v = m * amt * 2 * std::sin(2 * kPi * rate * t); break;
        case SPEC_TREMOLO:    v = 1 - m * amt * 0.5f * (1 - std::cos(2 * kPi * rate * t)); break;
        default:              v = m; break;
        }
        pts[i] = PointF(p.X + p.Width * i / (N - 1), Y(v));
    }
    Pen curve(SY_WAVE, 2.0f);
    curve.SetLineJoin(LineJoinRound);
    g.DrawLines(&curve, pts.data(), N);
    drawText(g, unit, fSmall, C_DIM, RectF(r.X + 8, r.Y + 4, 300, 16), StringAlignmentNear);
    swprintf(b, 64, L"%g", hi);
    drawText(g, b, fSmall, C_DIM, RectF(r.X + 2, p.Y - 7, 42, 14), StringAlignmentFar);
    swprintf(b, 64, L"%g", lo);
    drawText(g, b, fSmall, C_DIM, RectF(r.X + 2, p.Y + p.Height - 7, 42, 14), StringAlignmentFar);
    swprintf(b, 64, L"hold time  →  %.1f s", span);
    drawText(g, b, fSmall, C_DIM, RectF(p.X, p.Y + p.Height + 4, p.Width, 16), StringAlignmentFar);
    drawText(g, L"key pressed", fSmall, C_DIM, RectF(p.X, p.Y + p.Height + 4, 120, 16), StringAlignmentNear);
}

// Step 4: the envelope shape
void synDrawEnvGraph(Graphics& g, const Instrument& I) {
    const RectF& r = syEnvGraph;
    fillRound(g, r, 8, C_CANVAS, &C_BORDER);
    const SynthEdit& e = I.se;
    float A = synAttackMs(e.attackK) / 1000, D = synDecayMs(e.decayK) / 1000, S = e.sustainK, R = synReleaseMs(e.releaseK) / 1000;
    float H = std::max(0.3f, 0.3f * (A + D + R)), total = A + D + H + R;
    RectF p(r.X + 20, r.Y + 30, r.Width - 40, r.Height - 56);
    auto X = [&](float t) { return p.X + p.Width * t / total; };
    auto Y = [&](float v) { return p.Y + p.Height * (1 - v); };
    PointF pts[6] = {PointF(X(0), Y(0)), PointF(X(A), Y(1)), PointF(X(A + D), Y(S)), PointF(X(A + D + H), Y(S)),
                     PointF(X(total), Y(0)), PointF(X(0), Y(0))};
    SolidBrush fill(Color(50, 79, 209, 197));
    g.FillPolygon(&fill, pts, 6);
    Pen line(SY_WAVE, 2.2f);
    line.SetLineJoin(LineJoinRound);
    g.DrawLines(&line, pts, 5);
    Pen mark(Color(120, 255, 138, 61), 1.0f);
    mark.SetDashStyle(DashStyleDash);
    g.DrawLine(&mark, X(A + D + H), p.Y - 6, X(A + D + H), p.Y + p.Height);
    drawText(g, L"key released", fSmall, C_ACCENT, RectF(X(A + D + H) + 4, p.Y - 20, 120, 14), StringAlignmentNear);
    struct Seg { const wchar_t* n; float a, b; } segs[4] = {{L"A", 0, A}, {L"D", A, A + D}, {L"S", A + D, A + D + H}, {L"R", A + D + H, total}};
    for (auto& sg : segs) {
        float x0 = X(sg.a), x1 = X(sg.b);
        if (x1 - x0 < 10) continue;
        drawText(g, sg.n, fLabel, C_DIM, RectF(x0, p.Y + p.Height + 4, x1 - x0, 18));
    }
    wchar_t b[128];
    swprintf(b, 128, L"attack %ls  ·  decay %ls  ·  sustain %.0f%%  ·  release %ls", synMs(A * 1000).c_str(),
             synMs(D * 1000).c_str(), S * 100, synMs(R * 1000).c_str());
    drawText(g, b, fSmall, C_DIM, RectF(r.X + 12, r.Y + 6, r.Width - 24, 16), StringAlignmentNear);
}

// Step 5: summary + recording controls
void synDrawPlay(Graphics& g, const Instrument& I) {
    const SynthEdit& e = I.se;
    fillRound(g, syPlayPanel, 8, C_PANEL, &C_BORDER);
    drawText(g, L"YOUR SOUND", fTitle, C_ACCENT, RectF(syPlayPanel.X + 14, syPlayPanel.Y + 8, 300, 20), StringAlignmentNear);
    RectF thumb(syPlayPanel.X + 14, syPlayPanel.Y + 36, syPlayPanel.Width - 28, 110);
    fillRound(g, thumb, 6, C_CANVAS, &C_BORDER);
    synDrawWaveIn(g, I, thumb, false, 1.8f);
    wchar_t b[200];
    float y = thumb.Y + thumb.Height + 14;
    auto line = [&](const wchar_t* label, const std::wstring& text, int step) {
        drawText(g, label, fLabel, C_DIM, RectF(syPlayPanel.X + 14, y, 90, 20), StringAlignmentNear);
        drawText(g, text.c_str(), fLabel, C_TEXT, RectF(syPlayPanel.X + 104, y, syPlayPanel.Width - 190, 20), StringAlignmentNear);
        RectF edit(syPlayPanel.X + syPlayPanel.Width - 74, y, 60, 20);
        fillRound(g, edit, 4, C_BTN, &C_BORDER);
        drawText(g, L"edit", fSmall, C_DIM, edit);
        (void)step;
        y += 30;
    };
    if (I.sp.smp) {   // these rows must match the "edit" buttons in synOnDown (STEP_PLAY)
        swprintf(b, 200, L"whole sound: %ls  ·  root %ls", e.wavName.c_str(), smpNoteName((int)std::lround(I.sp.smpRoot)).c_str());
        line(L"Sound", b, STEP_WAVE);
    }
    swprintf(b, 200, L"smooth %.0f%%  ·  skew %+.0f%%  ·  drive %.0f%%  ·  fold %.0f%%  ·  crush %.0f%%", e.smooth * 100,
             (e.skew - 0.5f) * 200, e.drive * 100, e.fold * 100, e.crush * 100);
    line(L"Shape", b, STEP_SHAPE);
    if (I.sp.spec == SPEC_NONE) swprintf(b, 200, L"none");
    else swprintf(b, 200, L"%ls  ·  %ls  ·  full after %.2f s", SY_SPEC_NAMES[I.sp.spec], synSpecAmountText(I).c_str(),
                  synSpecTime(e.specTimeK));
    line(L"Special", b, STEP_SPECIAL);
    swprintf(b, 200, L"A %ls  ·  D %ls  ·  S %.0f%%  ·  R %ls", synMs(synAttackMs(e.attackK)).c_str(),
             synMs(synDecayMs(e.decayK)).c_str(), e.sustainK * 100, synMs(synReleaseMs(e.releaseK)).c_str());
    line(L"Envelope", b, STEP_ADSR);
    swprintf(b, 200, L"%.0f%%", e.volumeK * 100);
    line(L"Volume", b, STEP_ADSR);

    // recording
    fillRound(g, syRecPanel, 8, C_PANEL, &C_BORDER);
    drawText(g, L"RECORD", fTitle, C_ACCENT, RectF(syRecPanel.X + 16, syRecPanel.Y + 8, 200, 20), StringAlignmentNear);
    Track* t = trackById(gLiveTrackId);
    if (!t) return;
    if (t->arm) drawToggle(g, syArmBtn, L"✓  Armed  -  click to disarm", true, C_REC);
    else drawToggle(g, syArmBtn, L"Arm this track for recording", false, C_REC);
    drawToggle(g, syClickBtn, L"Click (metronome)", gMetronome.load(), C_ACCENT);
    double sec = (double)gPlayPos.load() / SR, beats = sec * gBpm.load() / 60.0;
    int mins = (int)(sec / 60);
    swprintf(b, 200, L"%02d:%06.3f   ·   bar %d . %d   ·   %.0f BPM", mins, sec - mins * 60, (int)(beats / 4) + 1,
             (int)beats % 4 + 1, gBpm.load());
    drawText(g, b, fLabel, gRecording.load() ? C_REC : C_TEXT, RectF(syRecPanel.X + 16, 232, syRecPanel.Width - 32, 22), StringAlignmentNear);
    const wchar_t* help[] = {L"Press ● at the top right (or Ctrl+R) to record, then play",
                             L"the keyboard below (PC keys, mouse or a MIDI keyboard).",
                             L"The track is armed automatically. The notes become a clip:",
                             L"double-click it in the main window for the piano roll.",
                             L"Recording starts at the playhead."};
    for (int i = 0; i < 5; ++i)
        drawText(g, help[i], fSmall, C_DIM, RectF(syRecPanel.X + 16, 270 + i * 18.0f, syRecPanel.Width - 32, 16), StringAlignmentNear);
}

void synDrawPiano(Graphics& g, const Instrument& I) {
    SolidBrush heldW(C_ACCENT), heldB(Color(255, 220, 110, 40));
    SolidBrush white(Color(255, 236, 236, 236)), black(Color(255, 30, 32, 36));
    Pen keyBorder(Color(255, 90, 94, 100), 1.0f);
    wchar_t buf[16];
    int base = 12 * (gSynOct + 1), top = base + 28;
    int lo = std::max(base, SY_PIANO_LO), hi = std::min(top, SY_PIANO_HI);
    if (lo <= hi) {   // the PC-key range
        SolidBrush b(Color(160, 255, 138, 61));
        float x1 = syKeyRect[lo].X, x2 = syKeyRect[hi].X + syKeyRect[hi].Width;
        g.FillRectangle(&b, x1, syPiano.Y - 5, x2 - x1, 3.0f);
    }
    for (int n = SY_PIANO_LO; n <= SY_PIANO_HI; ++n) {
        if (smpBlack(n)) continue;
        const RectF& r = syKeyRect[n];
        g.FillRectangle(I.held[n].load() ? &heldW : &white, r);
        g.DrawRectangle(&keyBorder, r);
        if (n % 12 == 0) {
            swprintf(buf, 16, L"C%d", n / 12 - 1);
            drawText(g, buf, fSmall, Color(255, 120, 120, 120), RectF(r.X, r.Y + r.Height - 20, r.Width, 16));
        }
    }
    for (int n = SY_PIANO_LO; n <= SY_PIANO_HI; ++n) {
        if (!smpBlack(n)) continue;
        GraphicsPath p;
        roundRect(p, syKeyRect[n], 2);
        g.FillPath(I.held[n].load() ? &heldB : &black, &p);
    }
}

void synPaint(HDC hdc) {
    RECT rc;
    GetClientRect(gSynWnd, &rc);
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
        Instrument* I = synI();

        // top bar: the steps and the DAW transport (the track's name is in the title bar)
        SolidBrush bar(C_PANEL);
        g.FillRectangle(&bar, 0.0f, 0.0f, SY_W, SY_TOP_H);
        Pen line(C_BORDER, 1.0f);
        g.DrawLine(&line, 0.0f, SY_TOP_H - 0.5f, SY_W, SY_TOP_H - 0.5f);
        if (I)
            for (int i = 0; i < STEP_COUNT; ++i) {
                RectF r = synStepRect(i);
                bool cur = i == gSynStep;
                fillRound(g, r, 6, cur ? C_ACCENT : (i < gSynStep ? mixColor(C_BTN, C_ACCENT, 0.18f) : C_BTN), &C_BORDER);
                drawText(g, SY_STEP_NAMES[i], fLabel, cur ? Color(255, 20, 20, 24) : (i < gSynStep ? C_TEXT : C_DIM), r);
                if (i + 1 < STEP_COUNT) drawText(g, L"›", fTitle, C_DIM, RectF(r.X + r.Width, r.Y, 16, r.Height));
            }
        drawTransportIcon(g, syBtnPlay, 1, gPlaying.load());
        drawTransportIcon(g, syBtnRec, 3, gRecState != REC_IDLE);

        g.TranslateTransform(0, -SY_SHIFT);   // the content (layout coordinates, see SY_SHIFT)
        if (!I) {
            drawText(g, L"Select a Synth track in the main window (or add one with  + Synth ) - every Synth track has its own synth.",
                     fLabel, C_DIM, RectF(12, 104, SY_W - 24, SY_H - 104 - SY_STATUS_H - 12));
        } else {

            // the step
            std::vector<SynKnob> ks;
            synBuildKnobs(*I, ks);
            switch (gSynStep) {
            case STEP_WAVE:
                for (int i = 0; i < SYT_COUNT; ++i) drawButton(g, syToolBtn[i], SY_TOOL_NAMES[i], i != SYT_UNDO || !I->se.undo.empty());
                synDrawCanvas(g, *I);
                synDrawWavPanel(g, *I);
                drawButton(g, sySetBtn, L"Set");
                drawButton(g, syAddBtn, L"Add");
                drawText(g, gSynFormulaMsg.c_str(), fSmall, gSynFormulaErr ? SY_ERROR : C_DIM, RectF(660, 370, 280, 26), StringAlignmentNear);
                drawText(g, L"FORMULA:  x = 0→1 over one cycle,  p = 2πx,  w = current wave    ·    "
                            L"sin cos tan abs sqrt exp log pow min max floor mod saw(x) sq(x) tri(x) w(x) rnd()    ·    + - * / ^",
                         fSmall, C_DIM, RectF(20, 400, 920, 16), StringAlignmentNear);
                break;
            case STEP_SHAPE:
                synDrawCanvas(g, *I);
                drawButton(g, syResetShape, L"Reset shape");
                break;
            case STEP_SPECIAL:
                for (int i = 0; i < SPEC_COUNT; ++i) {
                    RectF r = synSpecRect(i);
                    bool on = I->sp.spec == i;
                    fillRound(g, r, 6, on ? mixColor(C_PANEL, C_ACCENT, 0.35f) : C_PANEL, on ? &C_ACCENT : &C_BORDER);
                    drawText(g, SY_SPEC_NAMES[i], fLabel, on ? C_TEXT : C_DIM, RectF(r.X + 12, r.Y + 3, r.Width - 24, 18), StringAlignmentNear);
                    drawText(g, SY_SPEC_DESC[i], fSmall, C_DIM, RectF(r.X + 12, r.Y + 20, r.Width - 24, 16), StringAlignmentNear);
                }
                synDrawSpecGraph(g, *I);
                break;
            case STEP_ADSR:
                synDrawEnvGraph(g, *I);
                break;
            case STEP_PLAY:
                synDrawPlay(g, *I);
                break;
            }
            for (size_t i = 0; i < ks.size(); ++i) synDrawKnob(g, *I, ks[i], (int)i == gSynDragKnob || (int)i == gSynHoverKnob);
            if (gSynStep != STEP_PLAY)
                drawText(g, SY_STEP_HINTS[gSynStep], fSmall, C_DIM, RectF(20, 470, 920, 18), StringAlignmentNear);

            // always there: preview, octave, back / next, keyboard
            bool prev = gSynPreviewNote >= 0;
            fillRound(g, syPreviewBtn, 5, prev ? C_ACCENT : C_BTN, &C_ACCENT);
            wchar_t pb[48];
            swprintf(pb, 48, L"▶  Hold to preview %ls", smpNoteName(synPreviewPitch()).c_str());
            drawText(g, pb, fLabel, prev ? Color(255, 20, 20, 24) : C_ACCENT, syPreviewBtn);
            fillRound(g, syOctDown, 4, C_BTN, &C_BORDER);
            fillRound(g, syOctUp, 4, C_BTN, &C_BORDER);
            drawText(g, L"−", fLabel, C_TEXT, syOctDown);
            drawText(g, L"+", fLabel, C_TEXT, syOctUp);
            swprintf(pb, 48, L"OCT C%d", gSynOct);
            drawText(g, pb, fLabel, C_TEXT, RectF(syOctDown.X + syOctDown.Width, syOctDown.Y, syOctUp.X - syOctDown.X - syOctDown.Width, 26));
            drawText(g, L"PC keys: Z S X D C V G B H N J M / Q 2 W 3 E R 5 T 6 Y 7 U I  ·  - = octave", fSmall, C_DIM,
                     RectF(354, 496, 400, 26), StringAlignmentNear);
            drawButton(g, syBackBtn, L"‹  Back", gSynStep > 0);
            if (gSynStep + 1 < STEP_COUNT) {
                fillRound(g, syNextBtn, 5, C_ACCENT);
                drawText(g, L"Next  ›", fLabel, Color(255, 20, 20, 24), syNextBtn);
            } else {
                drawButton(g, syNextBtn, L"Next  ›", false);
            }
            synDrawPiano(g, *I);
        }

        // status bar
        RectF sr(0, SY_H - SY_STATUS_H, SY_W, SY_STATUS_H);
        g.FillRectangle(&bar, sr);
        g.DrawLine(&line, 0.0f, sr.Y + 0.5f, SY_W, sr.Y + 0.5f);
        bool recent = !gSynMsg.empty() && GetTickCount() - gSynMsgTime < 6000;
        const wchar_t* hint = L"Space DAW play/stop  ·  Ctrl+R record  ·  Ctrl+Z undo the wave  ·  "
                              L"knobs: drag (Shift = fine), wheel, double-click resets  ·  click a step to jump to it";
        drawText(g, recent ? gSynMsg.c_str() : hint, fSmall, recent ? C_TEXT : C_DIM, RectF(12, sr.Y, SY_W - 24, sr.Height),
                 StringAlignmentNear);
    }
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ============================================================================
// Synth window: input
// ============================================================================
int synPcKeyOffset(WPARAM vk) { return smpPianoKeyOffset(vk); }   // same layout as the Sampler

// Called from the main message loop for every message. Returns true if it was consumed.
bool synHandleKey(const MSG& m) {
    if (!gSynWnd) return false;
    Instrument* I = synI();
    if (m.hwnd == gSynFormula && m.wParam == VK_RETURN) {   // Enter in the formula box = Set
        if (m.message == WM_KEYDOWN && I) { synRunFormula(*I, false); return true; }
        if (m.message == WM_CHAR) return true;   // no beep
    }
    if (m.message != WM_KEYDOWN && m.message != WM_KEYUP) return false;
    if (GetAncestor(m.hwnd, GA_ROOT) != gSynWnd) return false;
    if (GetFocus() == gSynFormula) return false;   // typing a formula, not playing
    WPARAM vk = m.wParam & 0xFF;
    bool down = m.message == WM_KEYDOWN, repeat = (m.lParam & (1 << 30)) != 0;
    if (GetKeyState(VK_CONTROL) < 0) {
        if (vk == 'Z') { if (down && I) synUndo(*I); return true; }
        if (vk == 'R') { if (down && !repeat) { if (I) synRecord(); else synTransport(true); } return true; }
        return false;
    }
    if (down && vk == VK_SPACE) { if (!repeat) synTransport(false); return true; }
    if (!I) return false;
    if (down) {
        if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { if (gSynOct > 0) { synReleasePcKeys(); --gSynOct; } return true; }
        if (vk == VK_OEM_PLUS || vk == VK_ADD) { if (gSynOct < 7) { synReleasePcKeys(); ++gSynOct; } return true; }
        int off = synPcKeyOffset(vk);
        if (off < 0) return false;
        if (repeat || gSynPcHeld[vk] >= 0) return true;
        int n = 12 * (gSynOct + 1) + off;
        if (n > 127) return true;
        gSynPcHeld[vk] = n;
        smpNoteOn(n, 100);
        return true;
    }
    if (gSynPcHeld[vk] >= 0) { smpNoteOff(gSynPcHeld[vk]); gSynPcHeld[vk] = -1; return true; }
    return synPcKeyOffset(vk) >= 0;
}

void synOnDown(float x, float y, bool dbl) {
    if (syBtnPlay.Contains(x, y)) { synTransport(false); return; }
    if (syBtnRec.Contains(x, y)) { if (synI()) synRecord(); else synTransport(true); return; }
    Instrument* I = synI();
    if (!I) return;
    for (int i = 0; i < STEP_COUNT; ++i) if (synStepRect(i).Contains(x, y)) { synSetStep(i); return; }
    if (syBackBtn.Contains(x, y)) { synSetStep(gSynStep - 1); return; }
    if (syNextBtn.Contains(x, y)) { synSetStep(gSynStep + 1); return; }
    if (syPreviewBtn.Contains(x, y)) {   // held: the special function can be heard growing
        gSynPreviewNote = synPreviewPitch();
        smpNoteOn(gSynPreviewNote, 100);
        return;
    }
    if (syOctDown.Contains(x, y)) { if (gSynOct > 0) { synReleasePcKeys(); --gSynOct; } return; }
    if (syOctUp.Contains(x, y)) { if (gSynOct < 7) { synReleasePcKeys(); ++gSynOct; } return; }
    int n = synHitPiano(x, y);
    if (n >= 0) {
        gSynPianoDrag = true;
        gSynMouseNote = n;
        smpNoteOn(n, synVelocityFromY(n, y));
        return;
    }

    std::vector<SynKnob> ks;
    synBuildKnobs(*I, ks);
    int k = synHitKnob(ks, x, y);
    if (k >= 0) {
        SynKnob& kn = ks[k];
        if (kn.kind == SK_IMPORT) synPushUndo(*I);
        if (dbl) {
            *kn.value = kn.def;
            synKnobChanged(*I, kn);
        } else {
            gSynDragKnob = k;
            gSynDragY = y;
            gSynDragVal = *kn.value;
        }
        return;
    }

    switch (gSynStep) {
    case STEP_WAVE:
        // a wave tool or formula means you want a wave again: whole sound off
        for (int i = 0; i < SYT_COUNT; ++i) if (syToolBtn[i].Contains(x, y)) { synSetWhole(*I, false); synRunTool(*I, i); return; }
        if (sySetBtn.Contains(x, y)) { synSetWhole(*I, false); synRunFormula(*I, false); return; }
        if (syAddBtn.Contains(x, y)) { synSetWhole(*I, false); synRunFormula(*I, true); return; }
        if (sySamplesBtn.Contains(x, y)) { synSampleMenu(*I, sySamplesBtn); return; }
        if (syLoadBtn.Contains(x, y)) { synChooseWav(*I); return; }
        if (I->se.whole && syWholeBtn.Contains(x, y)) { synSetWhole(*I, true); return; }
        if (I->se.whole && syCycleBtn.Contains(x, y)) {
            synSetWhole(*I, false);
            synMsg(L"1 cycle: one cycle of the sound is the wave now - drag in the small picture to take another spot, shape it in step 2");
            return;
        }
        if (syAutoBtn.Contains(x, y) && !I->se.wav.empty()) {
            synSetWhole(*I, false);
            synPushUndo(*I); synAutoLength(I->se); synImportCycle(*I);
            return;
        }
        if (syOverview.Contains(x, y)) {
            if (I->se.wav.empty()) { synChooseWav(*I); return; }
            synSetWhole(*I, false);   // picking a spot = taking one cycle
            synPushUndo(*I);
            gSynOverviewDrag = true;
            synSetPosFromOverview(*I, x);
            return;
        }
        break;
    case STEP_SHAPE:
        if (syResetShape.Contains(x, y)) {
            I->se.smooth = I->se.drive = I->se.fold = I->se.crush = 0.0f;
            I->se.skew = 0.5f;
            synUpdateWave(*I);
            return;
        }
        break;
    case STEP_SPECIAL:
        for (int i = 0; i < SPEC_COUNT; ++i) if (synSpecRect(i).Contains(x, y)) { synSetSpecial(*I, i); return; }
        break;
    case STEP_PLAY:
        if (syArmBtn.Contains(x, y)) { synToggleArm(); return; }
        if (syClickBtn.Contains(x, y)) { gMetronome = !gMetronome.load(); return; }
        if (syPlayPanel.Contains(x, y)) {   // the "edit" buttons jump back to a step
            float rowY = syPlayPanel.Y + 36 + 110 + 14;
            // the rows of synDrawPlay: [Sound (whole sound only)], Shape, Special, Envelope, Volume
            std::vector<int> steps = {STEP_SHAPE, STEP_SPECIAL, STEP_ADSR, STEP_ADSR};
            if (I->sp.smp) steps.insert(steps.begin(), STEP_WAVE);
            for (size_t i = 0; i < steps.size(); ++i)
                if (RectF(syPlayPanel.X + syPlayPanel.Width - 74, rowY + i * 30, 60, 20).Contains(x, y)) { synSetStep(steps[i]); return; }
            if (RectF(syPlayPanel.X + 14, syPlayPanel.Y + 36, syPlayPanel.Width - 28, 110).Contains(x, y)) synSetStep(STEP_WAVE);
        }
        return;
    }
    if (gSynStep == STEP_WAVE || gSynStep == STEP_SHAPE) {
        if (synCanvas().Contains(x, y) && I->sp.smp) {
            synMsg(L"This is the whole sound - choose \"1 cycle\" (step 1) to draw on a wave");
            return;
        }
        if (synCanvas().Contains(x, y)) {
            synPushUndo(*I);
            gSynDrawing = true;
            synDrawAt(*I, x, y, true);
        }
    }
}

void synOnMove(float x, float y, bool shift) {
    Instrument* I = synI();
    if (!I) return;
    if (gSynDragKnob >= 0) {
        std::vector<SynKnob> ks;
        synBuildKnobs(*I, ks);
        if (gSynDragKnob >= (int)ks.size()) { gSynDragKnob = -1; return; }
        float range = shift ? 800.0f : 180.0f;
        *ks[gSynDragKnob].value = clampf(gSynDragVal + (gSynDragY - y) / range, 0.0f, 1.0f);
        synKnobChanged(*I, ks[gSynDragKnob]);
    } else if (gSynDrawing) {
        synDrawAt(*I, x, y, false);
    } else if (gSynOverviewDrag) {
        synSetPosFromOverview(*I, x);
    } else if (gSynPianoDrag) {
        int n = synHitPiano(x, y);
        if (n != gSynMouseNote) {
            if (gSynMouseNote >= 0) smpNoteOff(gSynMouseNote);
            gSynMouseNote = n;
            if (n >= 0) smpNoteOn(n, synVelocityFromY(n, y));
        }
    } else if (gSynPreviewNote < 0) {
        std::vector<SynKnob> ks;
        synBuildKnobs(*I, ks);
        gSynHoverKnob = synHitKnob(ks, x, y);
    }
}

bool synDragging() { return gSynDragKnob >= 0 || gSynDrawing || gSynOverviewDrag || gSynPianoDrag || gSynPreviewNote >= 0; }

std::wstring synWindowTitle() {
    Track* t = trackById(gLiveTrackId);
    return t && synI() ? t->name + L"  -  MiniDAW" : std::wstring(L"Synth  -  MiniDAW");
}

LRESULT CALLBACK SynWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        gSynWnd = hwnd;
        for (int& h : gSynPcHeld) h = -1;
        gSynMono = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        gSynFormula = CreateWindowExW(0, L"EDIT", L"sin(p) + 0.5*sin(2*p) + 0.25*sin(3*p)", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                      S(20), S(370 - SY_SHIFT), S(520), S(26), hwnd, (HMENU)(INT_PTR)ID_SYN_FORMULA, NULL, NULL);
        SendMessageW(gSynFormula, WM_SETFONT, (WPARAM)gSynMono, TRUE);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, 1, 30, NULL);
        return 0;
    case WM_CTLCOLOREDIT:
        if ((HWND)lParam == gSynFormula) {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(230, 230, 230));
            SetBkColor(dc, RGB(18, 20, 24));
            return (LRESULT)gEditBrush;
        }
        break;
    case WM_TIMER: {
        std::wstring t = synWindowTitle();   // follows renames
        if (t != gSynTitle) { gSynTitle = t; SetWindowTextW(hwnd, t.c_str()); }
        bool showF = synI() && gSynStep == STEP_WAVE;
        if (showF != (IsWindowVisible(gSynFormula) != 0)) ShowWindow(gSynFormula, showF ? SW_SHOW : SW_HIDE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        wchar_t file[MAX_PATH];
        Instrument* I = synI();
        if (I && DragQueryFileW(drop, 0, file, MAX_PATH) && synLoadFile(*I, file)) synSetStep(STEP_WAVE);
        DragFinish(drop);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        synPaint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(hwnd);
        synOnDown(GET_X_LPARAM(lParam) / gScale, synY(GET_Y_LPARAM(lParam) / gScale), msg == WM_LBUTTONDBLCLK);
        if (synDragging()) SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        synOnMove(GET_X_LPARAM(lParam) / gScale, synY(GET_Y_LPARAM(lParam) / gScale), (wParam & MK_SHIFT) != 0);
        return 0;
    case WM_LBUTTONUP:
        synEndMouse();
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        synEndMouse();
        return 0;
    case WM_MOUSEWHEEL: {
        Instrument* I = synI();
        if (!I) return 0;
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        std::vector<SynKnob> ks;
        synBuildKnobs(*I, ks);
        int k = synHitKnob(ks, pt.x / gScale, synY(pt.y / gScale));
        if (k >= 0) {
            float step = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) ? 0.005f : 0.02f;
            *ks[k].value = clampf(*ks[k].value + step * GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA, 0.0f, 1.0f);
            synKnobChanged(*I, ks[k]);
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) { synEndMouse(); synReleasePcKeys(); }   // no stuck notes
        return 0;
    case WM_CLOSE:   // keep the window, just hide it
        synEndMouse();
        synReleasePcKeys();
        ShowWindow(hwnd, SW_HIDE);
        SetForegroundWindow(gWnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (gSynMono) { DeleteObject(gSynMono); gSynMono = NULL; }
        gSynWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Opens the Synth window for the live Synth track (activate = false: show it without taking the focus)
void synthOpenWindow(bool activate = true) {
    if (!gSynWnd) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc = {};
            wc.style         = CS_DBLCLKS;
            wc.lpfnWndProc   = SynWndProc;
            wc.hInstance     = GetModuleHandleW(NULL);
            wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
            wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
            wc.lpszClassName = L"MiniDAWSynth";
            RegisterClassW(&wc);
            registered = true;
            synBuildLayout();
        }
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        RECT r = {0, 0, S(SY_W), S(SY_WIN_H)};
        AdjustWindowRect(&r, style, FALSE);
        RECT mr;
        GetWindowRect(gWnd, &mr);
        gSynTitle = synWindowTitle();
        CreateWindowW(L"MiniDAWSynth", gSynTitle.c_str(), style, mr.left + S(80), mr.top + S(40), r.right - r.left,
                      r.bottom - r.top, gWnd, NULL, GetModuleHandleW(NULL), NULL);   // sets gSynWnd
    }
    if (IsIconic(gSynWnd)) ShowWindow(gSynWnd, SW_RESTORE);
    ShowWindow(gSynWnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) { SetForegroundWindow(gSynWnd); SetFocus(gSynWnd); }
}

// ============================================================================
// Both instrument windows
// ============================================================================
void synOnLiveChanged() {
    for (int& h : gSynPcHeld) h = -1;   // setLiveTrack already sent all-notes-off to the previous instrument
    gSynMouseNote = gSynPreviewNote = -1;
    gSynDragKnob = -1;
    gSynDrawing = gSynOverviewDrag = gSynPianoDrag = false;
    if (gSynWnd) {
        gSynTitle = synWindowTitle();
        SetWindowTextW(gSynWnd, gSynTitle.c_str());
        InvalidateRect(gSynWnd, NULL, FALSE);
    }
}

// The live track changed: both windows show its instrument; an open instrument window switches to the
// other kind (Sampler <-> Synth) when the new live track has the other kind
void instOnLiveChanged() {
    smpOnLiveChanged();
    synOnLiveChanged();
    Instrument* L = gLiveInst.get();
    if (!L) return;
    bool smpVis = gSmpWnd && IsWindowVisible(gSmpWnd), synVis = gSynWnd && IsWindowVisible(gSynWnd);
    if (L->kind == INST_SYNTH && smpVis && !synVis) { ShowWindow(gSmpWnd, SW_HIDE); synthOpenWindow(false); }
    else if (L->kind == INST_SAMPLER && synVis && !smpVis) { ShowWindow(gSynWnd, SW_HIDE); samplerOpenWindow(false); }
}

void instOpenWindow() {
    Instrument* L = gLiveInst.get();
    if (L && L->kind == INST_SYNTH) synthOpenWindow(true);
    else samplerOpenWindow(true);
}
