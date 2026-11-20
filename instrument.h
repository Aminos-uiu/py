// instrument.h - MiniDAW instrument engines. Every MIDI track owns a Sampler, every Synth track a Synth
// (both are an Instrument, see `kind`).
//
// Included by daw_gui.cpp (one translation unit). Needs DataPtr, SR, i64, FADER_0DB, faderGain, panGains.
//
// Sampler, two modes:
//   Melodic  (one sample)   - every note plays the same sample, re-pitched relative to its root note.
//   Drum kit (multi sample) - 16 pads, each with its own sample; a pad plays when its MIDI note arrives.
// Synth (the former MiniSynth): one single-cycle waveform (1024 points) played at every pitch,
//   attack / release envelope, soft clipping.
//
// Threads: the parameters (SmpParams) are written by the GUI under gMx and read by the audio thread under
// gMx. The voices (SmpEngine) belong to the audio thread. Live input (PC keys, mouse, MIDI keyboard) is
// queued in Instrument::events under gSmpMx and goes to the "live" instrument (gLiveInst).

#pragma once

const int SMP_PADS = 16, SMP_VOICES = 48, SMP_MAX_EVENTS = 512;

struct SmpPad {
    DataPtr data;
    float note  = 36;          // MIDI note that triggers this pad
    float vol   = FADER_0DB;   // fader units (see faderGain)
    float pitch = 0.0f;        // semitones
    float pan   = 0.0f;        // -1 .. 1
};

struct SmpParams {
    int mode = 0;              // 0 = melodic, 1 = drum kit
    float master = FADER_0DB;
    // melodic
    DataPtr mData;
    float mRoot = 60, mTune = 0, mVol = FADER_0DB, mAttack = 2, mRelease = 400, mLoopStart = 0.25f;
    float mTranspose = 0;   // semitones, + = higher (mRoot is the pitch of the sample itself)
    bool mLoop = false;
    // drum kit
    SmpPad pads[SMP_PADS];
    SmpParams() { for (int i = 0; i < SMP_PADS; ++i) pads[i].note = (float)(36 + i); }
};

enum { EV_ON, EV_OFF, EV_PAD, EV_ALLOFF };
struct SmpEvent { int type, a, vel; };
enum { SRC_LIVE = 0, SRC_PLAY = 1 };   // who started a voice: live input or MIDI clip playback

struct SmpVoice {
    bool on = false;
    DataPtr data;
    double pos = 0, rate = 1;
    float gL = 0, gR = 0, env = 0, atkStep = 1, relMs = 30, relStep = 0;
    int note = -1, pad = -1, stage = 0;   // stage: 0 attack, 1 sustain, 2 release
    int src = SRC_LIVE, delay = 0;        // delay: frames to wait before starting (sample-accurate playback)
    i64 offAt = -1;                       // timeline position of the note-off (playback), -1 = none
    bool held = false, loop = false;
    double loopA = 0, loopB = 0, xf = 0;  // loop region and crossfade length (samples)
    unsigned age = 0;
};

inline float smpAt(const float* s, i64 len, i64 i) { return (i >= 0 && i < len) ? s[i] : 0.0f; }
inline float smpRead(const float* s, i64 len, double pos) {   // 4-point cubic (Hermite) interpolation
    i64 i = (i64)std::floor(pos);
    float t = (float)(pos - (double)i);
    float xm = smpAt(s, len, i - 1), x0 = smpAt(s, len, i), x1 = smpAt(s, len, i + 1), x2 = smpAt(s, len, i + 2);
    float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

struct SmpEngine {
    SmpVoice v[SMP_VOICES];
    unsigned clock = 0;

    SmpVoice& alloc() {
        int oldest = 0;
        for (int i = 0; i < SMP_VOICES; ++i) {
            if (!v[i].on) return v[i];
            if (v[i].age < v[oldest].age) oldest = i;
        }
        return v[oldest];   // all busy: steal the oldest
    }

    static void release(SmpVoice& x, float ms) {
        x.held = false;
        float step = x.env / std::max(1.0f, ms * SR / 1000.0f);
        if (x.stage == 2 && x.relStep >= step) return;   // already fading out at least as fast
        x.stage = 2;
        x.relStep = step;
        if (x.relStep <= 0.0f) x.relStep = 1.0f;          // not started yet: end at once
    }

    void startPad(const SmpParams& st, int p, int vel, int src, int delay, std::atomic<int>* hits) {
        const SmpPad& pd = st.pads[p];
        if (!pd.data || pd.data->s.empty()) return;
        SmpVoice& x = alloc();
        x = SmpVoice();
        x.on = true;
        x.data = pd.data;
        x.rate = std::pow(2.0, pd.pitch / 12.0);
        float g = faderGain(pd.vol) * vel / 127.0f, l, r;
        panGains(pd.pan, l, r);
        x.gL = g * l;
        x.gR = g * r;
        x.atkStep = 1.0f / (0.0005f * SR);   // 0.5 ms, only to avoid clicks
        x.pad = p;                           // one-shot: plays to the end, note-off is ignored
        x.src = src;
        x.delay = delay;
        x.age = ++clock;
        if (hits) hits[p].fetch_add(1);
    }

    // at: timeline position of the note start (playback only), offAt: of its note-off
    void startMelodic(const SmpParams& st, int note, int vel, int src, int delay, i64 at, i64 offAt) {
        if (!st.mData || st.mData->s.empty()) return;
        for (auto& o : v) {   // retrigger: end the same note if it is still held
            if (!o.on || o.pad >= 0 || o.note != note || !o.held || o.src != src) continue;
            if (src == SRC_PLAY && o.offAt >= 0) o.offAt = std::min(o.offAt, at);   // ends exactly when the new one starts
            else release(o, o.relMs);
        }
        SmpVoice& x = alloc();
        x = SmpVoice();
        x.on = true;
        x.data = st.mData;
        x.rate = std::pow(2.0, (note - std::round(st.mRoot) + std::round(st.mTranspose)) / 12.0 + st.mTune / 1200.0);
        x.gL = x.gR = faderGain(st.mVol) * vel / 127.0f;
        x.atkStep = 1.0f / std::max(1.0f, st.mAttack * SR / 1000.0f);
        x.relMs = st.mRelease;
        x.note = note;
        x.held = true;
        x.src = src;
        x.delay = delay;
        x.offAt = offAt;
        x.age = ++clock;
        if (st.mLoop) {
            double len = (double)st.mData->s.size(), a = std::floor(st.mLoopStart * len);
            if (len - a >= 64) {
                x.loop = true;
                x.loopA = a;
                x.loopB = len;
                x.xf = std::floor(std::min({0.02 * SR, a, (len - a) / 2}));
            }
        }
    }

    void noteOn(const SmpParams& st, int note, int vel, int src, int delay, i64 at, i64 offAt, std::atomic<int>* hits) {
        if (note < 0 || note > 127) return;
        if (st.mode == 0) { startMelodic(st, note, vel, src, delay, at, offAt); return; }
        for (int p = 0; p < SMP_PADS; ++p)
            if ((int)std::lround(st.pads[p].note) == note) startPad(st, p, vel, src, delay, hits);
    }
    void noteOff(int note, int src) {
        for (auto& x : v) if (x.on && x.pad < 0 && x.note == note && x.held && x.src == src) release(x, x.relMs);
    }
    void releaseAll(int src) {   // like letting go of every key
        for (auto& x : v) if (x.on && x.pad < 0 && x.src == src) release(x, x.relMs);
    }
    void cut(int src) {          // quick fade of everything from src (stop, seek, mute), pads included
        for (auto& x : v) if (x.on && x.src == src) release(x, 5.0f);
    }

    // Adds n stereo frames (interleaved) to out. pos = timeline position of frame 0 (for note-offs).
    void render(float master, float* out, int n, i64 pos) {
        for (auto& x : v) {
            if (!x.on) continue;
            const float* s = x.data->s.data();
            i64 len = (i64)x.data->s.size();
            float gl = x.gL * master, gr = x.gR * master;
            for (int f = 0; f < n; ++f) {
                if (x.delay > 0) { --x.delay; continue; }
                if (x.offAt >= 0 && x.stage != 2 && pos + f >= x.offAt) release(x, x.relMs);
                if (x.stage == 0) {
                    x.env += x.atkStep;
                    if (x.env >= 1.0f) { x.env = 1.0f; x.stage = 1; }
                } else if (x.stage == 2) {
                    x.env -= x.relStep;
                    if (x.env <= 0.0f) { x.on = false; break; }
                }
                if (!x.loop && x.pos >= (double)len) { x.on = false; break; }
                float y = smpRead(s, len, x.pos);
                if (x.loop && x.xf > 0 && x.pos >= x.loopB - x.xf) {   // crossfade into the loop start
                    float t = (float)((x.pos - (x.loopB - x.xf)) / x.xf);
                    y = y * (1.0f - t) + smpRead(s, len, x.pos - (x.loopB - x.loopA)) * t;
                }
                x.pos += x.rate;
                if (x.loop && x.pos >= x.loopB) x.pos -= x.loopB - x.loopA;
                y *= x.env;
                out[2 * f]     += y * gl;
                out[2 * f + 1] += y * gr;
            }
            if (!x.on) x.data.reset();
        }
    }
};

// ============================================================================
// Synth (the former MiniSynth): one single-cycle waveform played at every pitch
// ============================================================================
const int SYN_N = 1024, SYN_VOICES = 16;   // one cycle (power of two), polyphony

// Special functions: something grows while a key is held (and stays where it got to after release)
enum { SPEC_NONE, SPEC_PITCH_UP, SPEC_PITCH_DOWN, SPEC_VIBRATO, SPEC_TREMOLO, SPEC_BRIGHTEN, SPEC_COUNT };

struct SynthParams {        // what the audio thread needs (GUI writes under gMx)
    float table[SYN_N];     // the final waveform (after the shape knobs)
    float table2[SYN_N];    // SPEC_BRIGHTEN: the wave it morphs into
    float volume = 0.7f;    // 0..1, gain = volume^2
    float attackMs = 3.0f, decayMs = 300.0f, sustain = 1.0f, releaseMs = 80.0f;   // ADSR
    int spec = SPEC_NONE;
    float specAmount = 0.5f;   // 0..1 (meaning depends on the function)
    float specTime = 2.0f;     // seconds of holding until the function is at full strength
    float specRate = 5.5f;     // Hz (vibrato, tremolo)
    // Whole sound: instead of the single-cycle wave, every key plays this recording (mono, SR) at its pitch.
    // The ADSR and the special functions still apply. null = wave mode.
    DataPtr smp;
    float smpRoot = 60;        // the recording's pitch (MIDI note, fractions = cents)
    bool smpLoop = false;      // sustained sounds: loop the part from smpLoopStart to smpLoopEnd while held
    float smpLoopStart = 0.35f, smpLoopEnd = 1.0f;   // fractions of the length
};

// Editing state of a synth (GUI thread only): the wave you edit, its undo, knob values, WAV import
struct SynthEdit {
    std::vector<float> base;                    // the drawn / generated wave, before the shape knobs
    std::vector<std::vector<float>> undo;
    float smooth = 0.0f, skew = 0.5f, drive = 0.0f, fold = 0.0f, crush = 0.0f;   // shape knobs 0..1
    float attackK = 0.216f, decayK = 0.593f, sustainK = 1.0f, releaseK = 0.401f;   // 3 ms, 300 ms, 100 %, 80 ms
    float volumeK = 0.7f;
    float specAmountK = 0.5f, specTimeK = 0.696f, specRateK = 0.705f;           // 50 %, 2 s, 5.5 Hz
    std::vector<float> wav, ovMin, ovMax;       // loaded sound file (mono, its own rate) and overview
    float wavRate = 44100.0f;
    std::wstring wavName, importMsg;
    float impPos = 0.0f, impLen = 0.5f;         // where the cycle is taken from, its length knob
    DataPtr whole;                              // the loaded sound at SR (for "Whole sound" mode), before shaping
    float wholeRoot = 60;
    bool wholeLoop = false;
    size_t wholeView = 0;                       // step 2 shows a few cycles of the whole sound from here
};

struct SynVoice {
    bool on = false, held = false;
    int note = -1, stage = 0, src = SRC_LIVE, delay = 0;   // stage: 0 attack, 1 decay / sustain, 2 release
    float env = 0, vel = 0, relStep = 0;
    double phase = 0, inc = 0;
    DataPtr data;                     // whole sound mode: the recording (the voice keeps its own reference)
    double spos = 0, srate = 1, loopA = 0, loopB = 0, xf = 0;
    bool loop = false;
    i64 heldFrames = 0, frames = 0;   // how long the key has been held / the note has sounded
    i64 offAt = -1;
    unsigned age = 0;
};

struct SynthEngine {
    SynVoice v[SYN_VOICES];
    unsigned clock = 0;

    SynVoice& alloc() {
        int best = -1;
        for (int i = 0; i < SYN_VOICES; ++i) if (!v[i].on) return v[i];
        for (int i = 0; i < SYN_VOICES; ++i)   // steal the oldest released voice, else the oldest one
            if (v[i].stage == 2 && (best < 0 || v[i].age < v[best].age)) best = i;
        if (best < 0)
            for (int i = 0; i < SYN_VOICES; ++i) if (best < 0 || v[i].age < v[best].age) best = i;
        return v[best];
    }
    static void release(SynVoice& x, const SynthParams& sp) {   // fades from where the envelope is
        x.held = false;
        if (x.stage == 2) return;
        x.stage = 2;
        x.relStep = std::max(x.env, 1e-4f) / std::max(1.0f, sp.releaseMs * SR / 1000.0f);
    }
    void noteOn(const SynthParams& sp, int note, int vel, int src, int delay, i64 at, i64 offAt) {
        if (note < 0 || note > 127) return;
        for (auto& o : v) {   // retrigger: end the same note if it is still held
            if (!o.on || o.note != note || !o.held || o.src != src) continue;
            if (src == SRC_PLAY && o.offAt >= 0) o.offAt = std::min(o.offAt, at);
            else release(o, sp);
        }
        SynVoice& x = alloc();
        x = SynVoice();
        x.on = x.held = true;
        x.note = note;
        x.vel = vel / 127.0f;
        x.src = src;
        x.delay = delay;
        x.offAt = offAt;
        x.inc = 440.0 * std::exp2((note - 69) / 12.0) / SR;
        x.age = ++clock;
        if (sp.smp && sp.smp->s.size() > 64) {
            x.data = sp.smp;
            x.srate = std::exp2((note - sp.smpRoot) / 12.0);
            double len = (double)sp.smp->s.size();
            double a = std::floor(sp.smpLoopStart * len), b = std::floor(sp.smpLoopEnd * len);
            if (sp.smpLoop && b - a >= 256) {
                x.loop = true;
                x.loopA = a;
                x.loopB = b;
                x.xf = std::floor(std::min({0.03 * SR, a, (b - a) / 2}));
            }
        }
    }
    void noteOff(const SynthParams& sp, int note, int src) {
        for (auto& x : v) if (x.on && x.note == note && x.held && x.src == src) release(x, sp);
    }
    void releaseAll(const SynthParams& sp, int src) {
        for (auto& x : v) if (x.on && x.src == src) release(x, sp);
    }
    void cut(int src) {   // quick 5 ms fade
        for (auto& x : v)
            if (x.on && x.src == src) {
                x.held = false;
                x.stage = 2;
                x.relStep = std::max(x.relStep, 1.0f / (0.005f * SR));
            }
    }
    // How strong the special function is after holding for `frames` (0..1)
    static float specProgress(const SynthParams& sp, i64 frames) {
        if (sp.specTime <= 0.001f) return 1.0f;
        return std::min(1.0f, (float)frames / (sp.specTime * SR));
    }

    // Adds n (<= 1024) stereo frames to out. Like MiniSynth: voices summed, then soft-clipped.
    void render(const SynthParams& sp, float* out, int n, i64 pos) {
        float mix[1024];
        std::memset(mix, 0, sizeof(float) * n);
        float atk = 1.0f / std::max(1.0f, sp.attackMs * SR / 1000.0f);
        float dec = (1.0f - sp.sustain) / std::max(1.0f, sp.decayMs * SR / 1000.0f);
        const double twoPi = 6.283185307179586;
        bool any = false;
        for (auto& x : v) {
            if (!x.on) continue;
            any = true;
            for (int f = 0; f < n; ++f) {
                if (x.delay > 0) { --x.delay; continue; }
                if (x.offAt >= 0 && x.stage != 2 && pos + f >= x.offAt) release(x, sp);
                if (x.stage == 0) {
                    x.env += atk;
                    if (x.env >= 1.0f) { x.env = 1.0f; x.stage = 1; }
                } else if (x.stage == 1) {
                    if (x.env > sp.sustain) x.env = std::max(sp.sustain, x.env - dec);
                    else x.env = sp.sustain;   // the sustain knob may have moved
                    if (x.env <= 0.0f) { x.on = false; break; }   // sustain 0: the note has died away
                } else {
                    x.env -= x.relStep;
                    if (x.env <= 0.0f) { x.on = false; break; }
                }
                if (x.held) ++x.heldFrames;
                ++x.frames;

                // special function: grows while the key is held
                double inc = x.inc;
                float gain = 1.0f, morph = 0.0f;
                if (sp.spec != SPEC_NONE) {
                    float m = specProgress(sp, x.heldFrames), amt = sp.specAmount;
                    double t = (double)x.frames / SR;
                    switch (sp.spec) {
                    case SPEC_PITCH_UP:   inc *= std::exp2(m * amt * 24.0 / 12.0); break;   // up to +2 octaves
                    case SPEC_PITCH_DOWN: inc *= std::exp2(-m * amt * 24.0 / 12.0); break;
                    case SPEC_VIBRATO:    inc *= std::exp2(m * amt * 2.0 * std::sin(twoPi * sp.specRate * t) / 12.0); break;
                    case SPEC_TREMOLO:    gain = 1.0f - m * amt * 0.5f * (1.0f - (float)std::cos(twoPi * sp.specRate * t)); break;
                    case SPEC_BRIGHTEN:   morph = m; break;
                    }
                }

                float s;
                if (x.data) {   // whole sound: the recording, faster / slower for the pitch
                    const float* d = x.data->s.data();
                    i64 len = (i64)x.data->s.size();
                    if (!x.loop && x.spos >= (double)len) { x.on = false; break; }   // the sound has ended
                    s = smpRead(d, len, x.spos);
                    if (x.loop && x.held && x.xf > 0 && x.spos >= x.loopB - x.xf && x.spos < x.loopB) {   // crossfade into the loop start
                        float t = (float)((x.spos - (x.loopB - x.xf)) / x.xf);
                        s = s * (1.0f - t) + smpRead(d, len, x.spos - (x.loopB - x.loopA)) * t;
                    }
                    if (morph > 0.0f) s += (std::tanh(s * 6.0f) * 0.6f - s) * morph;   // Brighten: overdrive
                    x.spos += x.srate * (inc / x.inc);   // the special functions bend the pitch
                    if (x.loop && x.held && x.spos >= x.loopB) x.spos -= x.loopB - x.loopA;
                } else {
                    double idx = x.phase * SYN_N;
                    int i0 = (int)idx;
                    float fr = (float)(idx - i0), a = sp.table[i0 & (SYN_N - 1)], b = sp.table[(i0 + 1) & (SYN_N - 1)];
                    s = a + (b - a) * fr;
                    if (morph > 0.0f) {
                        float a2 = sp.table2[i0 & (SYN_N - 1)], b2 = sp.table2[(i0 + 1) & (SYN_N - 1)];
                        s += (a2 + (b2 - a2) * fr - s) * morph;
                    }
                    x.phase += inc;
                    if (x.phase >= 1.0) x.phase -= std::floor(x.phase);
                }
                mix[f] += s * x.env * x.vel * 0.3f * gain;
            }
            if (!x.on) x.data.reset();
        }
        if (!any) return;
        float g = sp.volume * sp.volume * 1.5f;
        for (int f = 0; f < n; ++f) {
            float y = std::tanh(mix[f] * g);
            out[2 * f] += y;
            out[2 * f + 1] += y;
        }
    }
};

// ============================================================================
// Instrument: a Sampler or a Synth (one per MIDI / Synth track)
// ============================================================================
enum { INST_SAMPLER = 0, INST_SYNTH = 1 };

struct Voices { SmpEngine smp; SynthEngine syn; };   // the playing voices (live, or a copy for export)

struct Instrument {
    int kind = INST_SAMPLER;
    SmpParams p;                              // sampler: GUI writes under gMx, audio thread reads under gMx
    SynthParams sp;                           // synth: same
    std::wstring mName, padName[SMP_PADS];    // GUI thread only
    int melPreset = -1, kitPreset = -1;       // GUI thread only: preset in use (-1 = your own samples)
    SynthEdit se;                             // GUI thread only
    Voices eng;                               // audio thread only
    std::vector<SmpEvent> events;             // live input, guarded by gSmpMx
    std::atomic<bool> held[128];              // live notes held (for the on-screen keyboard)
    std::atomic<int> padHits[SMP_PADS];       // incremented on every pad trigger (pad flash)
    explicit Instrument(int k = INST_SAMPLER) : kind(k) {
        events.reserve(SMP_MAX_EVENTS);
        for (auto& h : held) h = false;
        for (auto& h : padHits) h = 0;
        se.base.resize(SYN_N);
        for (int i = 0; i < SYN_N; ++i) se.base[i] = sp.table[i] = sp.table2[i] = 2.0f * i / SYN_N - 1.0f;   // saw
    }
};
typedef std::shared_ptr<Instrument> InstPtr;

void instNoteOn(const Instrument& I, Voices& v, int note, int vel, int src, int delay, i64 at, i64 offAt,
                std::atomic<int>* hits) {
    if (I.kind == INST_SYNTH) v.syn.noteOn(I.sp, note, vel, src, delay, at, offAt);
    else v.smp.noteOn(I.p, note, vel, src, delay, at, offAt, hits);
}
void instNoteOff(const Instrument& I, Voices& v, int note, int src) {
    if (I.kind == INST_SYNTH) v.syn.noteOff(I.sp, note, src);
    else v.smp.noteOff(note, src);
}
void instReleaseAll(const Instrument& I, Voices& v, int src) {
    if (I.kind == INST_SYNTH) v.syn.releaseAll(I.sp, src);
    else v.smp.releaseAll(src);
}
void instCut(Voices& v, int src) { v.smp.cut(src); v.syn.cut(src); }
void instRender(const Instrument& I, Voices& v, float* out, int n, i64 pos) {
    if (I.kind == INST_SYNTH) v.syn.render(I.sp, out, n, pos);
    else v.smp.render(faderGain(I.p.master), out, n, pos);
}

// Live input goes to one instrument: the one of the current MIDI track ("live track")
std::mutex gSmpMx;
InstPtr gLiveInst;

void smpPost(int type, int a, int vel) {   // GUI and MIDI threads
    std::lock_guard<std::mutex> lk(gSmpMx);
    if (!gLiveInst) return;
    Instrument& I = *gLiveInst;
    if (type == EV_ON) I.held[a] = true;
    else if (type == EV_OFF) I.held[a] = false;
    else if (type == EV_ALLOFF) for (auto& h : I.held) h = false;
    if ((int)I.events.size() < SMP_MAX_EVENTS) I.events.push_back({type, a, vel});
}
void smpNoteOn(int note, int vel) { if (note >= 0 && note <= 127) smpPost(EV_ON, note, vel); }
void smpNoteOff(int note) { if (note >= 0 && note <= 127) smpPost(EV_OFF, note, 0); }
void smpPadHit(int pad, int vel) { if (pad >= 0 && pad < SMP_PADS) smpPost(EV_PAD, pad, vel); }
void smpAllOff() { smpPost(EV_ALLOFF, 0, 0); }
