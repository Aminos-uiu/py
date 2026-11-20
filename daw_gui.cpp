// daw_gui.cpp - MiniDAW: a small multitrack audio recorder (Win32 / GDI+ UI, miniaudio)
//
// Build:
//   g++ -O2 daw_gui.cpp -o output\daw_gui.exe -static -mwindows -lwinmm -lgdiplus -lgdi32 -lole32 -lcomdlg32 -lshell32
//
// Features
//   - Four kinds of tracks (chosen when the track is added):
//       Audio track - records audio (mono) from the input device: In 1 / In 2 / In 1+2
//       Vocal track - an audio track kept apart for vocals (mic): records like an audio track, but its clips
//                     stay on vocal tracks (solo it and Export to get a dry vocal file, e.g. for voice conversion)
//       MIDI track  - records notes and plays them with its own Sampler instrument (instrument.h, sampler.h):
//                     melodic (one sample played at every pitch) or drum kit (16 pads, one sample each)
//       Synth track - records notes and plays them with its own Synth (the former MiniSynth, synth.h):
//                     one drawn / formula / imported waveform, shape knobs, attack / release
//   - Per track: Record-arm (R), Mute (M), Solo (S), input monitor (I, audio), volume, pan, meter
//   - Multitrack recording: every armed track records at the same time
//   - Overdub: record while hearing the other tracks; press Record while playing to punch in/out
//   - Audio: a newer clip covers older ones on the same track (like a punch-in on tape).
//     MIDI: clips on the same track all play (layered)
//   - Metronome + BPM, bar/beat ruler, adjustable latency compensation for recorded takes
//   - Move clips (also to other tracks of the same kind), split at playhead, delete, undo
//   - Piano roll editor for MIDI clips (pianoroll.h): double-click a MIDI clip
//   - Import audio files (wav / mp3 / flac, also drag & drop), export the mix as a 16-bit WAV
//
// Keys: Space play/stop · R record · Home go to start · S split · Del delete clip · Ctrl+Z undo
//       Ctrl+T add audio track · Ctrl+Shift+T add MIDI track · Ctrl+Y add Synth track · Ctrl+V add Vocal track
//       Ctrl+wheel zoom · Shift+wheel scroll time

#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Gdiplus;
typedef long long i64;

static const int   SR         = 48000;   // project sample rate (devices are resampled to this)
static const int   MAX_TRACKS = 64;
static const int   PEAK_BLOCK = 256;     // waveform overview resolution (samples per min/max pair)
static const float kPi        = 3.14159265358979f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ============================================================================
// Audio data, clips, tracks
// ============================================================================
struct AudioData {
    std::vector<float> s;              // mono samples at SR
    std::vector<float> pkMin, pkMax;   // one pair per PEAK_BLOCK samples, for fast waveform drawing
    void append(const float* src, size_t n) {
        if (!n) return;
        size_t old = s.size();
        s.insert(s.end(), src, src + n);
        size_t first = old / PEAK_BLOCK, nb = (s.size() + PEAK_BLOCK - 1) / PEAK_BLOCK;
        pkMin.resize(nb);
        pkMax.resize(nb);
        for (size_t b = first; b < nb; ++b) {
            size_t a = b * PEAK_BLOCK, e = std::min(s.size(), a + PEAK_BLOCK);
            float mn = s[a], mx = s[a];
            for (size_t i = a + 1; i < e; ++i) { mn = std::min(mn, s[i]); mx = std::max(mx, s[i]); }
            pkMin[b] = mn;
            pkMax[b] = mx;
        }
    }
};
typedef std::shared_ptr<const AudioData> DataPtr;

// MIDI notes. Times are in samples, relative to the start of the clip's data (like audio samples).
struct Note {
    i64 start = 0, len = 0;
    int pitch = 60, vel = 100;
};
struct MidiData { std::vector<Note> notes; };   // sorted by start; never changed once shared
typedef std::shared_ptr<const MidiData> MidiPtr;

struct Clip {
    i64 start = 0;   // position on the timeline (samples)
    i64 off   = 0;   // first sample used from the data
    i64 len   = 0;   // number of samples used
    DataPtr data;    // audio clip
    MidiPtr midi;    // MIDI clip
    int uid = 0;     // identifies the clip for the piano roll (kept when the clip is moved or edited)
    i64 end() const { return start + len; }
};
int gNextClipUid = 1;

const float FADER_0DB = 60.0f / 72.0f;
float faderDb(float f) { return f * 72.0f - 60.0f; }
float faderGain(float f) { return f <= 0.001f ? 0.0f : std::pow(10.0f, faderDb(f) / 20.0f); }
void panGains(float pan, float& l, float& r) {   // constant power, unity in the centre
    float a = (pan + 1.0f) * kPi * 0.25f;
    l = std::cos(a) * 1.41421356f;
    r = std::sin(a) * 1.41421356f;
}

#include "instrument.h"

struct Track {
    int id = 0;
    std::wstring name;
    int color = 0;
    float fader = 0.8333f;   // 0..1 -> -60..+12 dB (0 dB at 0.8333)
    float pan = 0.0f;        // -1..1
    bool mute = false, solo = false, arm = false, mon = false;
    int input = 0;           // audio tracks: 0 = In 1, 1 = In 2, 2 = In 1+2
    bool vocal = false;      // vocal track: an audio track (never midi) whose clips stay on vocal tracks
    bool midi = false;       // note track (MIDI or Synth): clips hold notes, played by inst
    InstPtr inst;            // note tracks only: a Sampler (MIDI track) or a Synth (Synth track)
    bool synth() const { return midi && inst && inst->kind == INST_SYNTH; }
    std::vector<Clip> clips; // audio: later clips cover earlier ones; MIDI: all clips play
};
// An input frame has 2 values: In 1, In 2
const int IN_CH = 2;
inline float inputSample(const float* frame, int sel) {
    return sel == 0 ? frame[0] : sel == 1 ? frame[1] : 0.5f * (frame[0] + frame[1]);
}

// ============================================================================
// Shared state (GUI thread <-> audio thread)
// ============================================================================
std::mutex gMx;                 // guards gTracks: the GUI writes under it, the audio thread try-locks it
std::vector<Track> gTracks;
int gNextTrackId = 1;

std::atomic<float> gMeter[MAX_TRACKS];
std::atomic<float> gMasterPeak[2];
std::atomic<i64>   gPlayPos{0};
std::atomic<bool>  gPlaying{false}, gRecording{false};
std::atomic<i64>   gSeekReq{-1};
std::atomic<bool>  gPlayReq{false}, gStopReq{false}, gRecOnReq{false}, gRecOffReq{false};
std::atomic<int>   gRecStarts{0};   // incremented each time the audio thread handles a record request
std::atomic<i64>   gRecStart{0};    // timeline position where the current recording began
std::atomic<i64>   gRecEnd{0};      // timeline position where it ended
std::atomic<bool>  gMetronome{false};
std::atomic<float> gBpm{120.0f};
std::atomic<int>   gOverflows{0};

// Lock-free single-producer / single-consumer ring: recorded input (IN_CH values per frame) -> GUI
struct Ring {
    std::vector<float> buf;
    size_t cap = 0;
    std::atomic<size_t> w{0}, r{0};
    void init(size_t c) { buf.assign(c, 0.0f); cap = c; w = 0; r = 0; }
    bool push(const float* s, size_t n) {   // all or nothing
        size_t wi = w.load(std::memory_order_relaxed), ri = r.load(std::memory_order_acquire);
        if (cap - (wi - ri) < n) return false;
        for (size_t i = 0; i < n; ++i) buf[(wi + i) % cap] = s[i];
        w.store(wi + n, std::memory_order_release);
        return true;
    }
    size_t pop(float* d, size_t n) {
        size_t ri = r.load(std::memory_order_relaxed), wi = w.load(std::memory_order_acquire);
        n = std::min(n, wi - ri);
        for (size_t i = 0; i < n; ++i) d[i] = buf[(ri + i) % cap];
        r.store(ri + n, std::memory_order_release);
        return n;
    }
} gRing;

// Live MIDI notes played while recording (audio thread -> GUI), same kind of ring
struct MidiRecEv { i64 t; int trackId, type, note, vel; };
struct MidiRing {
    std::vector<MidiRecEv> buf;
    size_t cap = 0;
    std::atomic<size_t> w{0}, r{0};
    void init(size_t c) { buf.assign(c, MidiRecEv()); cap = c; w = 0; r = 0; }
    bool push(const MidiRecEv& e) {
        size_t wi = w.load(std::memory_order_relaxed), ri = r.load(std::memory_order_acquire);
        if (wi - ri >= cap) return false;
        buf[wi % cap] = e;
        w.store(wi + 1, std::memory_order_release);
        return true;
    }
    bool pop(MidiRecEv& e) {
        size_t ri = r.load(std::memory_order_relaxed), wi = w.load(std::memory_order_acquire);
        if (ri == wi) return false;
        e = buf[ri % cap];
        r.store(ri + 1, std::memory_order_release);
        return true;
    }
} gMidiRing;

// Adds the tracks' playback for [pos, pos+n) into out (interleaved stereo).
// On each track a later clip covers earlier ones where they overlap.
void mixTracks(const std::vector<Track>& tracks, i64 pos, int n, float* out, float* peaks) {
    bool anySolo = false;
    for (auto& t : tracks) anySolo |= t.solo;
    const int CH = 1024;
    float buf[CH];
    unsigned char filled[CH];
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const Track& t = tracks[ti];
        if (t.midi || t.mute || (anySolo && !t.solo) || t.clips.empty()) continue;
        float g = faderGain(t.fader), pl, pr;
        panGains(t.pan, pl, pr);
        pl *= g;
        pr *= g;
        float peak = 0.0f;
        for (int done = 0; done < n; done += CH) {
            int c = std::min(CH, n - done);
            i64 p0 = pos + done, p1 = p0 + c;
            std::memset(filled, 0, c);
            bool any = false;
            for (size_t ci = t.clips.size(); ci-- > 0;) {
                const Clip& cl = t.clips[ci];
                i64 a = std::max(p0, cl.start), b = std::min(p1, cl.end());
                if (a >= b) continue;
                const float* src = cl.data->s.data() + cl.off + (a - cl.start);
                for (i64 p = a; p < b; ++p, ++src) {
                    int k = (int)(p - p0);
                    if (!filled[k]) { filled[k] = 1; buf[k] = *src; }
                }
                any = true;
            }
            if (!any) continue;
            for (int k = 0; k < c; ++k) {
                if (!filled[k]) continue;
                float L = buf[k] * pl, R = buf[k] * pr;
                out[2 * (done + k)]     += L;
                out[2 * (done + k) + 1] += R;
                peak = std::max(peak, std::max(std::fabs(L), std::fabs(R)));
            }
        }
        if (peaks && ti < (size_t)MAX_TRACKS) peaks[ti] = std::max(peaks[ti], peak);
    }
}

void addClicks(float* out, i64 pos, ma_uint32 n) {   // metronome, 4/4, accent on beat 1
    static float env = 0.0f, phase = 0.0f, freq = 1000.0f;
    double spb = SR * 60.0 / gBpm.load();
    const float decay = std::exp(-1.0f / (0.015f * SR));
    for (ma_uint32 f = 0; f < n; ++f) {
        i64 p = pos + f;
        i64 b = (i64)std::floor(p / spb), bPrev = (i64)std::floor((p - 1) / spb);
        if (b != bPrev) { env = 1.0f; phase = 0.0f; freq = (b % 4 == 0) ? 1760.0f : 1175.0f; }
        if (env > 0.001f) {
            float s = 0.35f * env * std::sin(phase);
            phase += 2.0f * kPi * freq / SR;
            if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
            env *= decay;
            out[2 * f] += s;
            out[2 * f + 1] += s;
        }
    }
}

const ma_uint32 CB_BLOCK = 1024;   // the callback is processed in blocks of at most this many frames

bool trackAudible(const Track& t, bool anySolo) { return !t.mute && !(anySolo && !t.solo); }

// Starts the notes of a MIDI track's clips that begin in [from, to). pos = timeline position of frame 0
// of the block being rendered (notes start sample-accurately inside it).
void midiTrigger(const Track& t, Voices& eng, i64 from, i64 to, i64 pos, std::atomic<int>* hits) {
    if (!t.inst) return;
    for (const Clip& c : t.clips) {
        if (!c.midi) continue;
        i64 a = std::max(from, c.start), b = std::min(to, c.end());
        if (a >= b) continue;
        i64 base = c.start - c.off;   // timeline position of data time 0
        const auto& ns = c.midi->notes;
        auto it = std::lower_bound(ns.begin(), ns.end(), a - base, [](const Note& x, i64 v) { return x.start < v; });
        for (; it != ns.end() && it->start + base < b; ++it) {
            i64 T = it->start + base, off = std::min(T + std::max<i64>(1, it->len), c.end());
            instNoteOn(*t.inst, eng, it->pitch, it->vel, SRC_PLAY, (int)std::max<i64>(0, T - pos), T, off, hits);
        }
    }
}

// Renders a MIDI track's instrument (n <= CB_BLOCK) through the track's volume and pan into out.
// Returns the peak.
float renderMidiTrack(const Track& t, Voices& eng, i64 pos, int n, float* out) {
    float buf[2 * CB_BLOCK];
    std::memset(buf, 0, sizeof(float) * 2 * n);
    instRender(*t.inst, eng, buf, n, pos);
    float g = faderGain(t.fader), pl, pr, peak = 0.0f;
    panGains(t.pan, pl, pr);
    pl *= g;
    pr *= g;
    for (int f = 0; f < n; ++f) {
        float L = buf[2 * f] * pl, R = buf[2 * f + 1] * pr;
        out[2 * f] += L;
        out[2 * f + 1] += R;
        peak = std::max(peak, std::max(std::fabs(L), std::fabs(R)));
    }
    return peak;
}

void processBlock(float* out, const float* in, ma_uint32 n) {   // in is NULL when the device has no input
    std::memset(out, 0, sizeof(float) * 2 * n);
    static i64 scanPos = 0;          // MIDI clips: notes starting before this position were already started
    static bool cutPending = false;  // end the notes started by MIDI clips (after stop / seek)

    // Transport requests from the GUI
    i64 seek = gSeekReq.exchange(-1);
    if (seek >= 0) { gPlayPos.store(seek); scanPos = seek; cutPending = true; }
    if (gPlayReq.exchange(false) && !gPlaying.load()) { gPlaying.store(true); scanPos = gPlayPos.load(); }
    if (gRecOnReq.exchange(false)) {
        if (gPlaying.load()) { gRecStart.store(gPlayPos.load()); gRecording.store(true); }
        gRecStarts.fetch_add(1);
    }
    if (gRecOffReq.exchange(false)) {
        if (gRecording.load()) gRecEnd.store(gPlayPos.load());
        gRecording.store(false);
    }
    if (gStopReq.exchange(false)) {
        if (gRecording.load()) gRecEnd.store(gPlayPos.load());
        gPlaying.store(false);
        gRecording.store(false);
        cutPending = true;
    }

    bool playing = gPlaying.load(), recording = gRecording.load();
    i64 pos = gPlayPos.load();

    // Input frames: In 1, In 2 (silence when the device has no input)
    float inBuf[IN_CH * CB_BLOCK];
    if (in) std::memcpy(inBuf, in, sizeof(float) * IN_CH * n);
    else std::memset(inBuf, 0, sizeof(float) * IN_CH * n);

    float peaks[MAX_TRACKS] = {};
    std::unique_lock<std::mutex> lk(gMx, std::try_to_lock);
    if (lk.owns_lock()) {
        bool anySolo = false;
        for (auto& t : gTracks) anySolo |= t.solo;

        // Live input (keys, mouse, MIDI keyboard) for the instruments; recorded on armed MIDI tracks
        {
            std::unique_lock<std::mutex> sl(gSmpMx, std::try_to_lock);
            if (sl.owns_lock()) {
                for (auto& t : gTracks) {
                    if (!t.midi || !t.inst) continue;
                    Instrument& I = *t.inst;
                    for (const SmpEvent& e : I.events) {
                        int note = e.a;
                        switch (e.type) {
                        case EV_ON:  instNoteOn(I, I.eng, e.a, e.vel, SRC_LIVE, 0, pos, -1, I.padHits); break;
                        case EV_OFF: instNoteOff(I, I.eng, e.a, SRC_LIVE); break;
                        case EV_PAD:   // drum pads (Sampler window)
                            if (I.kind != INST_SAMPLER) continue;
                            I.eng.smp.startPad(I.p, e.a, e.vel, SRC_LIVE, 0, I.padHits);
                            note = (int)std::lround(I.p.pads[e.a].note);
                            break;
                        case EV_ALLOFF: instReleaseAll(I, I.eng, SRC_LIVE); break;
                        }
                        if (recording && t.arm && !gMidiRing.push({pos, t.id, e.type, note, e.vel})) gOverflows.fetch_add(1);
                    }
                    I.events.clear();
                }
            }
        }

        // MIDI tracks: start the clips' notes, render the instruments
        i64 from = std::max(scanPos, pos - 4 * (i64)CB_BLOCK);
        for (size_t ti = 0; ti < gTracks.size() && ti < (size_t)MAX_TRACKS; ++ti) {
            const Track& t = gTracks[ti];
            if (!t.midi || !t.inst) continue;
            Voices& eng = t.inst->eng;
            if (cutPending || !playing || !trackAudible(t, anySolo)) instCut(eng, SRC_PLAY);
            else midiTrigger(t, eng, from, pos + n, pos, t.inst->padHits);
            peaks[ti] = renderMidiTrack(t, eng, pos, (int)n, out);
        }
        cutPending = false;
        if (playing) scanPos = pos + n;

        // Audio tracks
        if (playing) mixTracks(gTracks, pos, (int)n, out, peaks);
        for (size_t ti = 0; ti < gTracks.size() && ti < (size_t)MAX_TRACKS; ++ti) {
            const Track& t = gTracks[ti];
            if (!t.arm || t.midi) continue;
            bool mon = t.mon && !t.mute;
            float g = faderGain(t.fader), pl, pr;
            panGains(t.pan, pl, pr);
            float pk = 0.0f;
            for (ma_uint32 f = 0; f < n; ++f) {
                float s = inputSample(inBuf + IN_CH * f, t.input);
                pk = std::max(pk, std::fabs(s));
                if (mon) { out[2 * f] += s * g * pl; out[2 * f + 1] += s * g * pr; }
            }
            peaks[ti] = std::max(peaks[ti], pk);   // armed tracks show their input level
        }
        for (size_t ti = 0; ti < gTracks.size() && ti < (size_t)MAX_TRACKS; ++ti)
            if (peaks[ti] > gMeter[ti].load()) gMeter[ti].store(peaks[ti]);
        lk.unlock();
    }

    // Recording: hand the input to the GUI thread, which appends it to the takes
    if (recording && !gRing.push(inBuf, (size_t)n * IN_CH)) gOverflows.fetch_add(1);

    if (playing && gMetronome.load()) addClicks(out, pos, n);

    float pkL = 0.0f, pkR = 0.0f;
    for (ma_uint32 f = 0; f < n; ++f) {
        pkL = std::max(pkL, std::fabs(out[2 * f]));
        pkR = std::max(pkR, std::fabs(out[2 * f + 1]));
    }
    if (pkL > gMasterPeak[0].load()) gMasterPeak[0].store(pkL);
    if (pkR > gMasterPeak[1].load()) gMasterPeak[1].store(pkR);

    if (playing) gPlayPos.store(pos + n);
}

std::atomic<i64> gCbFrames{0};   // frames the device asked for (health check of exclusive mode)

// Input from a separate capture device. A duplex stream over two different devices (e.g. the laptop's mic and
// an audio interface's output) runs at the wrong speed, because the two clocks differ. Then the input is opened
// as its own device and its audio reaches the output callback through this ring.
Ring gInRing;
std::atomic<bool> gSplitIn{false};
const size_t SPLIT_PREFILL = 960;   // frames kept in the ring (20 ms), so the two clocks can drift without gaps

void capture_callback(ma_device*, void*, const void* pIn, ma_uint32 n) {
    if (pIn) gInRing.push((const float*)pIn, (size_t)n * IN_CH);   // dropped if the ring is full
}

void splitInput(float* dst, ma_uint32 n) {   // n frames of input from the ring (silence until it is filled)
    static bool primed = false;
    static float scratch[IN_CH * CB_BLOCK];
    size_t avail = (gInRing.w.load(std::memory_order_acquire) - gInRing.r.load(std::memory_order_relaxed)) / IN_CH;
    while (avail > 3 * SPLIT_PREFILL + n) {   // too far behind (capture runs faster): drop the surplus
        size_t drop = std::min<size_t>(CB_BLOCK, avail - SPLIT_PREFILL);
        gInRing.pop(scratch, drop * IN_CH);
        avail -= drop;
    }
    if (!primed && avail >= SPLIT_PREFILL) primed = true;
    size_t got = 0;
    if (primed) {
        got = gInRing.pop(dst, (size_t)n * IN_CH) / IN_CH;
        if (got < n) primed = false;   // ran dry: fill up again before continuing
    }
    if (got < n) std::memset(dst + got * IN_CH, 0, sizeof(float) * IN_CH * (n - got));
}

void data_callback(ma_device*, void* pOut, const void* pIn, ma_uint32 n) {
    gCbFrames.fetch_add(n);
    float* out = (float*)pOut;
    const float* in = (const float*)pIn;
    static float split[IN_CH * CB_BLOCK];
    bool sp = gSplitIn.load();
    for (ma_uint32 done = 0; done < n;) {
        ma_uint32 c = std::min(CB_BLOCK, n - done);
        const float* blk = in ? in + IN_CH * done : NULL;
        if (sp) { splitInput(split, c); blk = split; }
        processBlock(out + 2 * done, blk, c);
        done += c;
    }
}

// ============================================================================
// GUI-side state: messages, selection, undo
// ============================================================================
HWND gWnd;
std::wstring gMsg;
DWORD gMsgTime = 0;
bool gDirty = false;            // recordings / imports not exported yet
int gSelTrack = -1;             // selected track index
int gSelClipT = -1, gSelClipI = -1;   // selected clip (track index, clip index)

void setMsg(const std::wstring& m) { gMsg = m; gMsgTime = GetTickCount(); }

bool selClipValid() {
    return gSelClipT >= 0 && gSelClipT < (int)gTracks.size() &&
           gSelClipI >= 0 && gSelClipI < (int)gTracks[gSelClipT].clips.size();
}
void clearClipSel() { gSelClipT = gSelClipI = -1; }

std::vector<std::vector<Track>> gUndo;
void pushUndoState(std::vector<Track>&& s) {
    gUndo.push_back(std::move(s));
    if (gUndo.size() > 100) gUndo.erase(gUndo.begin());
}
void pushUndo() { pushUndoState(std::vector<Track>(gTracks)); }

int trackIndexById(int id) {
    for (size_t i = 0; i < gTracks.size(); ++i) if (gTracks[i].id == id) return (int)i;
    return -1;
}

// ============================================================================
// Recording (GUI side): takes collect the input while recording
// ============================================================================
struct Take {
    int trackId = 0, input = 0;
    bool midi = false;
    std::shared_ptr<AudioData> data;   // audio take
    std::vector<Note> notes;           // MIDI take: start = timeline position (latency compensated)
    i64 heldAt[128];                   // MIDI take: start of a note still held, -1 = not held
    int heldVel[128];
    Take() { for (auto& h : heldAt) h = -1; }
};
std::vector<Take> gTakes;
enum { REC_IDLE, REC_WAIT, REC_ACTIVE } gRecState = REC_IDLE;
int gRecStartsSeen = 0;
i64 gTakeStart = 0;
float gLatCompMs = 0.0f;        // recorded audio is moved this much earlier
float gOutLatMs = 0.0f;         // output latency only (used for MIDI takes)
int gOverflowsSeen = 0;

i64 latSamples() { return (i64)std::llround(gLatCompMs * SR / 1000.0); }
// The sampler is played in time with what you hear, so only the output latency is compensated
i64 takeLat(const Take&) { return latSamples(); }

void drainRing() {
    static std::vector<float> tmp(8192 * IN_CH), mono(8192);
    for (;;) {
        size_t got = gRing.pop(tmp.data(), tmp.size());
        if (!got) break;
        size_t frames = got / IN_CH;
        for (auto& tk : gTakes) {
            if (tk.midi) continue;
            for (size_t f = 0; f < frames; ++f) mono[f] = inputSample(&tmp[IN_CH * f], tk.input);
            tk.data->append(mono.data(), frames);
        }
        if (got < tmp.size()) break;
    }
}

void closeHeld(Take& tk, int n, i64 t) {
    if (n < 0 || n > 127 || tk.heldAt[n] < 0) return;
    Note x;
    x.start = tk.heldAt[n];
    x.len = std::max<i64>(SR / 100, t - x.start);
    x.pitch = n;
    x.vel = tk.heldVel[n];
    tk.notes.push_back(x);
    tk.heldAt[n] = -1;
}

// The live notes are timed by the audio thread when it plays them, i.e. one output latency
// after the moment they were played along with what was heard
void drainMidi() {
    i64 lat = (i64)std::llround(gOutLatMs * SR / 1000.0);
    MidiRecEv e;
    while (gMidiRing.pop(e)) {
        for (auto& tk : gTakes) {
            if (!tk.midi || tk.trackId != e.trackId) continue;
            i64 t = e.t - lat;
            switch (e.type) {
            case EV_ON:
                if (e.note < 0 || e.note > 127) break;
                closeHeld(tk, e.note, t);
                tk.heldAt[e.note] = t;
                tk.heldVel[e.note] = e.vel;
                break;
            case EV_OFF: closeHeld(tk, e.note, t); break;
            case EV_PAD: {   // one-shot pad: a short note
                Note x;
                x.start = t;
                x.len = SR / 8;
                x.pitch = e.note;
                x.vel = e.vel;
                tk.notes.push_back(x);
                break;
            }
            case EV_ALLOFF: for (int n = 0; n < 128; ++n) closeHeld(tk, n, t); break;
            }
        }
    }
}

void finalizeTakes() {
    drainRing();
    drainMidi();
    bool undoPushed = false;
    int added = 0, emptyMidi = 0;
    i64 recEnd = std::max(gRecEnd.load(), gTakeStart + 1);
    for (auto& tk : gTakes) {
        int ti = trackIndexById(tk.trackId);
        if (ti < 0) continue;
        Clip c;
        if (tk.midi) {
            for (int n = 0; n < 128; ++n) closeHeld(tk, n, recEnd);
            if (tk.notes.empty()) { ++emptyMidi; continue; }
            auto md = std::make_shared<MidiData>();
            for (Note x : tk.notes) {
                x.start -= gTakeStart;
                if (x.start < 0) { x.len = std::max<i64>(SR / 100, x.len + x.start); x.start = 0; }
                md->notes.push_back(x);
            }
            std::stable_sort(md->notes.begin(), md->notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
            c.start = gTakeStart;
            c.len = recEnd - gTakeStart;
            c.midi = md;
        } else {
            i64 lat = takeLat(tk);
            i64 size = (i64)tk.data->s.size();
            i64 start = gTakeStart - lat, off = 0;
            if (start < 0) { off = -start; start = 0; }
            if (size - off <= 0) continue;
            c.start = start;
            c.off = off;
            c.len = size - off;
            c.data = tk.data;
        }
        if (!undoPushed) { pushUndo(); undoPushed = true; }
        c.uid = gNextClipUid++;
        std::lock_guard<std::mutex> lk(gMx);
        gTracks[ti].clips.push_back(c);
        ++added;
    }
    gTakes.clear();
    gRecState = REC_IDLE;
    if (added) {
        gDirty = true;
        setMsg(L"Recorded " + std::to_wstring(added) + (added == 1 ? L" take" : L" takes"));
    } else if (emptyMidi) {
        setMsg(L"No notes were recorded - play the Sampler window keys or a MIDI keyboard while recording");
    }
}

void pollRecording() {
    if (gRecState == REC_WAIT) {
        if (gRecStarts.load() == gRecStartsSeen) return;
        gRecState = REC_ACTIVE;
        gTakeStart = gRecStart.load();
    }
    if (gRecState == REC_ACTIVE) {
        drainRing();
        drainMidi();
        if (!gRecording.load()) finalizeTakes();
    }
    int ov = gOverflows.load();
    if (ov != gOverflowsSeen) { gOverflowsSeen = ov; setMsg(L"Warning: recording buffer overflow (audio was dropped)"); }
}

void endRecordingNow() {   // the device is gone: keep what was recorded
    if (gRecState == REC_WAIT) { gTakes.clear(); gRecState = REC_IDLE; }
    else if (gRecState == REC_ACTIVE) finalizeTakes();
}

// ============================================================================
// Device management
// ============================================================================
ma_context gContext;
std::vector<ma_device_info> gPlayDevs, gCapDevs;
ma_device gDevice, gCapDevice;   // gCapDevice: the input, when it cannot share a duplex stream with the output
bool gDeviceOpen = false, gHasInput = false, gCapOpen = false;
std::wstring gAudioStatus = L"Audio: not running";

std::wstring utf8ToWide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 1) return L"";
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

void enumerateAudio() {
    gPlayDevs.clear();
    gCapDevs.clear();
    ma_device_info *pi, *ci;
    ma_uint32 pc, cc;
    if (ma_context_get_devices(&gContext, &pi, &pc, &ci, &cc) == MA_SUCCESS) {
        gPlayDevs.assign(pi, pi + pc);
        gCapDevs.assign(ci, ci + cc);
    }
}

void closeCapture() {
    if (gCapOpen) { ma_device_uninit(&gCapDevice); gCapOpen = false; }
    gSplitIn = false;
}

void closeAudio() {
    if (gDeviceOpen) { ma_device_uninit(&gDevice); gDeviceOpen = false; }
    closeCapture();
    gHasInput = false;
    gPlaying = false;
    if (gRecording.load()) gRecEnd = gPlayPos.load();
    gRecording = false;
    gPlayReq = gStopReq = gRecOnReq = gRecOffReq = false;
    i64 s = gSeekReq.exchange(-1);
    if (s >= 0) gPlayPos = s;
    endRecordingNow();
}

// inSel: -2 = no input, -1 = system default, >= 0 = gCapDevs index. outSel: -1 = default, >= 0 = gPlayDevs index
// Low latency: the output is opened in WASAPI exclusive mode with a 128-frame buffer (about 6 ms instead of
// about 25 ms, and it skips the Windows mixer and effects). Other programs cannot play through that output
// while MiniDAW has it. Virtual devices (e.g. sound enhancers) often do not support it: then the normal
// shared mode is used.
bool gLowLat = true;      // wanted (toolbar toggle)
bool gExclusive = false;  // what the open device really uses

bool audioRunsSmoothly() {   // exclusive mode can open but then starve (driver / virtual device)
    Sleep(120);
    i64 f0 = gCbFrames.load();
    Sleep(300);
    return gCbFrames.load() - f0 >= (i64)(SR * 0.3 * 0.85);
}

// Opens the output (duplex: with the input in the same stream). Checked to run at the right speed: exclusive
// mode can starve, and a duplex stream over two different devices runs too slowly.
bool openOutput(bool excl, bool duplex, int inSel, int outSel) {
    ma_device_config c = ma_device_config_init(duplex ? ma_device_type_duplex : ma_device_type_playback);
    c.sampleRate          = SR;
    c.playback.format     = ma_format_f32;
    c.playback.channels   = 2;
    c.playback.pDeviceID  = (outSel >= 0 && outSel < (int)gPlayDevs.size()) ? &gPlayDevs[outSel].id : NULL;
    c.capture.format      = ma_format_f32;
    c.capture.channels    = 2;
    c.capture.pDeviceID   = (inSel >= 0 && inSel < (int)gCapDevs.size()) ? &gCapDevs[inSel].id : NULL;
    c.periodSizeInFrames  = 256;
    c.performanceProfile  = ma_performance_profile_low_latency;
    c.dataCallback        = data_callback;
    if (excl) {   // the input stays shared (exclusive input + output starves on many devices)
        c.playback.shareMode = ma_share_mode_exclusive;
        c.periodSizeInFrames = 128;
        c.periods = 2;
    }
    if (ma_device_init(&gContext, &c, &gDevice) != MA_SUCCESS) return false;
    if (ma_device_start(&gDevice) != MA_SUCCESS) { ma_device_uninit(&gDevice); return false; }
    if (!audioRunsSmoothly()) { ma_device_uninit(&gDevice); return false; }
    gDeviceOpen = true;
    gHasInput = duplex;
    gExclusive = excl;
    return true;
}

// Opens the input as a device of its own (its audio goes through gInRing)
bool openCapture(int inSel) {
    ma_device_config c = ma_device_config_init(ma_device_type_capture);
    c.sampleRate         = SR;
    c.capture.format     = ma_format_f32;
    c.capture.channels   = 2;
    c.capture.pDeviceID  = (inSel >= 0 && inSel < (int)gCapDevs.size()) ? &gCapDevs[inSel].id : NULL;
    c.periodSizeInFrames = 256;
    c.performanceProfile = ma_performance_profile_low_latency;
    c.dataCallback       = capture_callback;
    gInRing.init((size_t)SR * IN_CH);   // 1 s
    if (ma_device_init(&gContext, &c, &gCapDevice) != MA_SUCCESS) return false;
    if (ma_device_start(&gCapDevice) != MA_SUCCESS) { ma_device_uninit(&gCapDevice); return false; }
    gCapOpen = true;
    return true;
}

bool openAudio(int inSel, int outSel) {
    closeAudio();
    gExclusive = false;
    bool wantIn = inSel != -2;
    // With an input: one duplex stream if it runs right (same device), else output and input opened apart.
    // Low latency (exclusive output) first, then the normal shared mode. Without the input only as a last resort.
    for (int mode = gLowLat ? 0 : 1; mode < 2 && wantIn && !gDeviceOpen; ++mode) {
        bool excl = mode == 0;
        if (openOutput(excl, true, inSel, outSel)) break;
        if (!openCapture(inSel)) continue;
        gSplitIn = true;
        if (openOutput(excl, false, inSel, outSel)) { gHasInput = true; break; }
        closeCapture();
    }
    for (int mode = gLowLat ? 0 : 1; mode < 2 && !gDeviceOpen; ++mode) openOutput(mode == 0, false, inSel, outSel);
    if (!gDeviceOpen) { gAudioStatus = L"Audio: failed to open the device"; return false; }

    double outMs = gDevice.playback.internalPeriodSizeInFrames * gDevice.playback.internalPeriods * 1000.0
                   / std::max<ma_uint32>(1, gDevice.playback.internalSampleRate);
    double inMs = 0.0;
    if (gHasInput) {
        ma_device& cd = gCapOpen ? gCapDevice : gDevice;
        inMs = cd.capture.internalPeriodSizeInFrames * cd.capture.internalPeriods * 1000.0
               / std::max<ma_uint32>(1, cd.capture.internalSampleRate);
        if (gCapOpen) inMs += SPLIT_PREFILL * 1000.0 / SR;   // what the ring holds
    }
    gLatCompMs = (float)std::round((outMs + inMs) * 10.0) / 10.0f;
    gOutLatMs = (float)outMs;

    wchar_t buf[200];
    if (wantIn && !gHasInput) swprintf(buf, 200, L"%.1f ms · NO INPUT", outMs);
    else swprintf(buf, 200, L"%.1f ms", outMs);
    gAudioStatus = buf;
    return true;
}

// ============================================================================
// Layout and view
// ============================================================================
const int TOOLBAR_H = 92, HEADER_W = 250, RULER_H = 26, TRACK_H = 92, STATUS_H = 24;
const int CLIENT_W = 1200, CLIENT_H = 720;
const int ID_INCOMBO = 101, ID_OUTCOMBO = 102, ID_RESCAN = 103, ID_RENAME = 104;
const UINT WM_APP_RENAME_DONE = WM_APP + 1;

float gScale = 1.0f;
float gW = CLIENT_W, gH = CLIENT_H;   // client size in logical pixels
float gScrollY = 0.0f;
double gViewStart = 0.0;               // seconds at the left edge of the lanes
double gPxPerSec = 40.0;

HWND gInCombo, gOutCombo, gRescanBtn, gRenameEdit = NULL;
int gRenameId = -1;
HFONT gUiFont;
HBRUSH gEditBrush;
Gdiplus::Font *fTitle, *fLabel, *fSmall, *fTime;

int S(float v) { return (int)std::lround(v * gScale); }

float lanesTop() { return (float)(TOOLBAR_H + RULER_H); }
float lanesBottom() { return gH - STATUS_H; }
float laneLeft() { return (float)HEADER_W; }
float trackTop(int i) { return lanesTop() + i * TRACK_H - gScrollY; }
float timeToX(double sec) { return laneLeft() + (float)((sec - gViewStart) * gPxPerSec); }
float sampleToX(i64 s) { return timeToX((double)s / SR); }
double xToTime(float x) { return gViewStart + (x - laneLeft()) / gPxPerSec; }
i64 xToSample(float x) { double t = xToTime(x); return t < 0 ? 0 : (i64)(t * SR); }

int trackAtY(float y) {
    if (y < lanesTop() || y >= lanesBottom()) return -1;
    int i = (int)std::floor((y - lanesTop() + gScrollY) / TRACK_H);
    return (i >= 0 && i < (int)gTracks.size()) ? i : -1;
}

void clampScroll() {
    float content = (float)gTracks.size() * TRACK_H + 60.0f, visible = lanesBottom() - lanesTop();
    gScrollY = clampf(gScrollY, 0.0f, std::max(0.0f, content - visible));
    if (gViewStart < 0) gViewStart = 0;
}

void ensureTrackVisible(int i) {
    float t = trackTop(i), b = t + TRACK_H;
    if (t < lanesTop()) gScrollY -= lanesTop() - t;
    else if (b > lanesBottom()) gScrollY += b - lanesBottom();
    clampScroll();
}

void ensureTimeVisible(i64 s) {
    float x = sampleToX(s);
    if (x < laneLeft() || x > gW - 20) gViewStart = std::max(0.0, (double)s / SR - 40.0 / gPxPerSec);
}

void zoomAt(float x, double factor) {
    if (x < laneLeft()) x = laneLeft();
    double t = xToTime(x);
    gPxPerSec = std::min(4000.0, std::max(2.0, gPxPerSec * factor));
    gViewStart = std::max(0.0, t - (x - laneLeft()) / gPxPerSec);
}

// Toolbar (row 1: devices, row 2: transport and tools)
const RectF btnHome(12, 48, 40, 34), btnPlay(56, 48, 40, 34), btnStop(100, 48, 40, 34), btnRec(144, 48, 40, 34);
const RectF timeBox(196, 48, 190, 34), bpmBox(396, 48, 92, 34), clickBtn(494, 48, 70, 34), latBox(570, 48, 118, 34);
const RectF btnImport(700, 48, 72, 34), btnExport(778, 48, 72, 34);   // tracks are added in the track list
RectF lowLatRect() { float x = std::max(906.0f, gW - 252); return RectF(x, 9, gW - 12 - x, 28); }

// Track header parts
enum Part { P_NONE, P_NAME, P_DEL, P_ARM, P_MUTE, P_SOLO, P_MON, P_INPUT, P_VOL, P_PAN, P_COUNT };
RectF partRect(int i, int part) {
    float t = trackTop(i);
    bool midi = i >= 0 && i < (int)gTracks.size() && gTracks[i].midi;
    bool badge = midi || (i >= 0 && i < (int)gTracks.size() && gTracks[i].vocal);   // KEYS / VOCAL badge
    switch (part) {
    case P_NAME:  return RectF(14, t + 7, badge ? 148.0f : 180.0f, 22);
    case P_INPUT: if (midi) return RectF(110, t + 36, 106, 22); break;   // instrument button
    }
    switch (part) {
    case P_DEL:   return RectF(HEADER_W - 32, t + 8, 20, 20);
    case P_ARM:   return RectF(14, t + 36, 28, 22);
    case P_MUTE:  return RectF(46, t + 36, 28, 22);
    case P_SOLO:  return RectF(78, t + 36, 28, 22);
    case P_MON:   return RectF(110, t + 36, 28, 22);
    case P_INPUT: return RectF(142, t + 36, 74, 22);
    case P_VOL:   return RectF(14, t + 66, 112, 18);
    case P_PAN:   return RectF(176, t + 66, 40, 18);
    }
    return RectF(0, t, HEADER_W, TRACK_H);
}
RectF meterRect(int i) { float t = trackTop(i); return RectF(HEADER_W - 22, t + 36, 8, TRACK_H - 44); }
RectF addTrackRect() { return RectF(14, trackTop((int)gTracks.size()) + 12, (HEADER_W - 40) / 4.0f, 30); }
RectF addVocalRect() { RectF a = addTrackRect(); return RectF(a.X + a.Width + 4, a.Y, a.Width, a.Height); }
RectF addMidiRect() { RectF a = addVocalRect(); return RectF(a.X + a.Width + 4, a.Y, a.Width, a.Height); }
RectF addSynthRect() { RectF a = addMidiRect(); return RectF(a.X + a.Width + 4, a.Y, a.Width, a.Height); }

int hitPart(int i, float x, float y) {
    for (int p = P_NAME; p < P_COUNT; ++p) {
        if (p == P_MON && gTracks[i].midi) continue;   // MIDI tracks have no input monitor
        RectF r = partRect(i, p);
        if (p == P_VOL || p == P_PAN) r.Inflate(4, 4);
        if (r.Contains(x, y)) return p;
    }
    return P_NONE;
}

// ============================================================================
// Editing actions
// ============================================================================
// Live input (Sampler window keys, mouse, MIDI keyboard) goes to one MIDI track: the "live track".
// It follows the selection: selecting / arming a MIDI track or opening its Sampler / piano roll makes it live.
int gLiveTrackId = -1;
void instOnLiveChanged();  // synth.h: the instrument windows follow the live track
void instOpenWindow();     // synth.h: Sampler or Synth window of the live track

Track* trackById(int id) { int i = trackIndexById(id); return i < 0 ? nullptr : &gTracks[i]; }

void setLiveTrack(int id) {
    Track* t = trackById(id);
    InstPtr inst = (t && t->midi) ? t->inst : nullptr;
    if (!inst) id = -1;
    {
        std::lock_guard<std::mutex> lk(gSmpMx);
        if (gLiveInst != inst) {
            if (gLiveInst) {   // let go of the notes still held on the previous instrument
                for (auto& h : gLiveInst->held) h = false;
                if ((int)gLiveInst->events.size() < SMP_MAX_EVENTS) gLiveInst->events.push_back({EV_ALLOFF, 0, 0});
            }
            gLiveInst = inst;
        }
    }
    bool changed = id != gLiveTrackId;
    gLiveTrackId = id;
    if (changed) instOnLiveChanged();
}

void validateLive() {   // after tracks were deleted / restored
    Track* t = trackById(gLiveTrackId);
    if (t && t->midi) { setLiveTrack(gLiveTrackId); return; }
    int pick = -1;
    if (gSelTrack >= 0 && gSelTrack < (int)gTracks.size() && gTracks[gSelTrack].midi) pick = gTracks[gSelTrack].id;
    else for (auto& tr : gTracks) if (tr.midi) { pick = tr.id; break; }
    setLiveTrack(pick);
}

void selectTrack(int i) {
    gSelTrack = i;
    if (i >= 0 && i < (int)gTracks.size() && gTracks[i].midi) setLiveTrack(gTracks[i].id);
}

enum { TT_AUDIO, TT_MIDI, TT_SYNTH, TT_VOCAL };   // kinds of tracks
void smpApplyDefaultPresets(Instrument& I);   // sampler.h
std::wstring smpSoundName(const Instrument& I);   // sampler.h: what a MIDI track's Sampler plays

// Default name of a new track: each kind is numbered on its own ("Audio 1", "Vocal 1", "MIDI 1", "Synth 1" ...),
// using the smallest number that no existing track has (so deleted tracks free their numbers)
std::wstring newTrackName(int type) {
    std::wstring prefix = type == TT_MIDI ? L"MIDI " : type == TT_SYNTH ? L"Synth " : type == TT_VOCAL ? L"Vocal " : L"Audio ";
    for (int n = 1;; ++n) {
        std::wstring name = prefix + std::to_wstring(n);
        bool used = false;
        for (auto& tr : gTracks) if (tr.name == name) { used = true; break; }
        if (!used) return name;
    }
}

int addTrackNoUndo(int type = TT_AUDIO) {
    Track t;
    t.id = gNextTrackId++;
    t.name = newTrackName(type);
    t.color = (t.id - 1) % 8;
    t.arm = gTracks.empty();
    t.vocal = type == TT_VOCAL;
    t.midi = type == TT_MIDI || type == TT_SYNTH;
    if (t.midi) t.inst = std::make_shared<Instrument>(type == TT_SYNTH ? INST_SYNTH : INST_SAMPLER);
    if (type == TT_MIDI) smpApplyDefaultPresets(*t.inst);   // Piano + Drum kit, so it plays right away
    {
        std::lock_guard<std::mutex> lk(gMx);
        gTracks.push_back(t);
    }
    return (int)gTracks.size() - 1;
}

void addTrack(int type = TT_AUDIO) {
    if ((int)gTracks.size() >= MAX_TRACKS) { setMsg(L"Track limit reached"); return; }
    pushUndo();
    selectTrack(addTrackNoUndo(type));
    ensureTrackVisible(gSelTrack);
    clampScroll();
    if (type == TT_MIDI) setMsg(L"MIDI track added - click its instrument button to load sounds, double-click its lane to draw notes");
    if (type == TT_SYNTH) setMsg(L"Synth track added - click its instrument button to shape the wave, double-click its lane to draw notes");
    if (type == TT_VOCAL) setMsg(L"Vocal track added - pick the mic input (In 1 / In 2), arm it (R) and record; solo it and Export for a dry vocal file");
}

void deleteTrack(int i) {
    if (gRecState != REC_IDLE) { setMsg(L"Stop recording before deleting tracks"); return; }
    if (i < 0 || i >= (int)gTracks.size()) return;
    pushUndo();
    {
        std::lock_guard<std::mutex> lk(gMx);
        gTracks.erase(gTracks.begin() + i);
    }
    for (auto& m : gMeter) m = 0.0f;
    clearClipSel();
    if (gSelTrack >= (int)gTracks.size()) gSelTrack = (int)gTracks.size() - 1;
    validateLive();
    clampScroll();
    setMsg(L"Track deleted (Ctrl+Z to undo)");
}

void undo() {
    if (gRecState != REC_IDLE) { setMsg(L"Stop recording before undo"); return; }
    if (gUndo.empty()) { setMsg(L"Nothing to undo"); return; }
    std::vector<Track> s = std::move(gUndo.back());
    gUndo.pop_back();
    // Undo restores clips and tracks; mixer settings of tracks that still exist are kept
    std::map<int, const Track*> cur;
    for (auto& t : gTracks) cur[t.id] = &t;
    for (auto& t : s) {
        auto it = cur.find(t.id);
        if (it == cur.end()) continue;
        const Track& c = *it->second;
        t.name = c.name; t.fader = c.fader; t.pan = c.pan;
        t.mute = c.mute; t.solo = c.solo; t.arm = c.arm; t.mon = c.mon; t.input = c.input;
    }
    {
        std::lock_guard<std::mutex> lk(gMx);
        gTracks.swap(s);
    }
    for (auto& m : gMeter) m = 0.0f;
    clearClipSel();
    if (gSelTrack >= (int)gTracks.size()) gSelTrack = (int)gTracks.size() - 1;
    validateLive();
    clampScroll();
    setMsg(L"Undo");
}

void seekTo(i64 s) {
    if (gRecState != REC_IDLE) return;   // keep takes continuous
    if (s < 0) s = 0;
    gPlayPos = s;
    gSeekReq = s;
}

void transportPlay() {
    if (!gDeviceOpen) { setMsg(L"No audio output device"); return; }
    gPlayReq = true;
}

void transportStop() {
    if (gPlaying.load() || gRecState != REC_IDLE || gPlayReq.load()) {
        gPlayReq = false;
        gStopReq = true;
    } else {
        seekTo(0);   // already stopped: back to the start
        gViewStart = 0;
    }
}

void transportRecord() {
    if (gRecState != REC_IDLE) { gRecOffReq = true; return; }   // punch out, keep playing
    if (!gDeviceOpen) { setMsg(L"No audio device"); return; }
    bool needInput = false;   // MIDI tracks do not need an input device
    for (auto& t : gTracks) if (t.arm && !t.midi) needInput = true;
    if (needInput && !gHasInput) {
        setMsg(L"No input device open - choose one under IN (or disarm the armed audio tracks)");
        return;
    }
    // Live MIDI goes to one track: if the live track is not armed, use the first armed MIDI track
    Track* live = trackById(gLiveTrackId);
    if (!live || !live->arm)
        for (auto& t : gTracks) if (t.midi && t.arm) { setLiveTrack(t.id); break; }
    for (auto& t : gTracks) {
        if (!t.arm) continue;
        Take tk;
        tk.trackId = t.id;
        tk.input = t.input;
        tk.midi = t.midi;
        if (!t.midi) tk.data = std::make_shared<AudioData>();
        gTakes.push_back(tk);
    }
    if (gTakes.empty()) { setMsg(L"Arm a track first (the R button on a track)"); return; }
    gRecStartsSeen = gRecStarts.load();
    gRecState = REC_WAIT;
    gPlayReq = true;
    gRecOnReq = true;
}

void deleteSelectedClip() {
    if (!selClipValid()) return;
    pushUndo();
    {
        std::lock_guard<std::mutex> lk(gMx);
        auto& cl = gTracks[gSelClipT].clips;
        cl.erase(cl.begin() + gSelClipI);
    }
    clearClipSel();
}

void splitAtPlayhead() {
    i64 p = gPlayPos.load();
    int ti = -1;
    std::vector<int> idx;
    auto inside = [p](const Clip& c) { return p > c.start && p < c.end(); };
    if (selClipValid() && inside(gTracks[gSelClipT].clips[gSelClipI])) {
        ti = gSelClipT;
        idx.push_back(gSelClipI);
    } else if (gSelTrack >= 0 && gSelTrack < (int)gTracks.size()) {
        ti = gSelTrack;
        auto& cl = gTracks[ti].clips;
        for (int i = 0; i < (int)cl.size(); ++i) if (inside(cl[i])) idx.push_back(i);
    }
    if (idx.empty()) { setMsg(L"Split: put the playhead inside a clip of the selected track"); return; }
    pushUndo();
    std::lock_guard<std::mutex> lk(gMx);
    auto& cl = gTracks[ti].clips;
    for (int k = (int)idx.size() - 1; k >= 0; --k) {
        int i = idx[k];
        Clip a = cl[i], b = cl[i];
        a.len = p - a.start;
        b.off += a.len;
        b.len -= a.len;
        b.start = p;
        b.uid = gNextClipUid++;
        cl[i] = a;
        cl.insert(cl.begin() + i + 1, b);
        if (k == (int)idx.size() - 1) { gSelClipT = ti; gSelClipI = i + 1; }
    }
}

double samplesPerBeat() { return SR * 60.0 / gBpm.load(); }

// A new, empty MIDI clip (4 bars, starting at the bar at timeline position at). Returns its uid.
int createMidiClip(int ti, i64 at) {
    if (ti < 0 || ti >= (int)gTracks.size() || !gTracks[ti].midi) return 0;
    double bar = 4 * samplesPerBeat();
    pushUndo();
    Clip c;
    c.start = (i64)(std::floor(at / bar) * bar);
    c.len = (i64)std::llround(4 * bar);
    c.midi = std::make_shared<MidiData>();
    c.uid = gNextClipUid++;
    {
        std::lock_guard<std::mutex> lk(gMx);
        gTracks[ti].clips.push_back(c);
    }
    gSelClipT = ti;
    gSelClipI = (int)gTracks[ti].clips.size() - 1;
    gDirty = true;
    return c.uid;
}

std::wstring fileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::shared_ptr<AudioData> loadAudioFile(const std::wstring& path) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, SR);   // mono, resampled to SR
    ma_decoder dec;
    if (ma_decoder_init_file_w(path.c_str(), &cfg, &dec) != MA_SUCCESS) return nullptr;
    auto d = std::make_shared<AudioData>();
    std::vector<float> buf(16384);
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, buf.data(), buf.size(), &got);
        if (got) d->append(buf.data(), (size_t)got);
        if (r != MA_SUCCESS || got < buf.size()) break;
    }
    ma_decoder_uninit(&dec);
    if (d->s.empty()) return nullptr;
    return d;
}

// The first file goes to targetTrack (or a new track if -1), further files to new tracks
void importFiles(const std::vector<std::wstring>& files, int targetTrack, i64 pos) {
    bool undoPushed = false;
    int ok = 0;
    std::wstring failed;
    for (size_t i = 0; i < files.size(); ++i) {
        auto d = loadAudioFile(files[i]);
        if (!d) { failed = fileNameOf(files[i]); continue; }
        if (!undoPushed) { pushUndo(); undoPushed = true; }
        int ti = (ok == 0 && targetTrack >= 0 && targetTrack < (int)gTracks.size() && !gTracks[targetTrack].midi)
                     ? targetTrack : -1;   // audio never goes onto a MIDI track
        if (ti < 0) {
            if ((int)gTracks.size() >= MAX_TRACKS) { setMsg(L"Track limit reached"); break; }
            ti = addTrackNoUndo(TT_AUDIO);
            std::wstring nm = fileNameOf(files[i]);
            size_t dot = nm.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) nm = nm.substr(0, dot);
            std::lock_guard<std::mutex> lk(gMx);
            gTracks[ti].name = nm;
            gTracks[ti].arm = false;
        }
        Clip c;
        c.start = pos;
        c.len = (i64)d->s.size();
        c.data = d;
        c.uid = gNextClipUid++;
        {
            std::lock_guard<std::mutex> lk(gMx);
            gTracks[ti].clips.push_back(c);
        }
        gSelTrack = ti;
        gSelClipT = ti;
        gSelClipI = (int)gTracks[ti].clips.size() - 1;
        ++ok;
    }
    if (ok) {
        gDirty = true;
        ensureTrackVisible(gSelTrack);
    }
    if (!failed.empty()) setMsg(L"Could not read " + failed);
    else if (ok) setMsg(L"Imported " + std::to_wstring(ok) + (ok == 1 ? L" file" : L" files"));
}

void importDialog() {
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gWnd;
    ofn.lpstrFilter = L"Audio files (*.wav;*.mp3;*.flac)\0*.wav;*.mp3;*.flac\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> files;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) files.push_back(first);
    else while (*p) { files.push_back(first + L"\\" + p); p += wcslen(p) + 1; }
    importFiles(files, gSelTrack, gPlayPos.load());
}

void exportMix() {
    i64 end = 0;
    for (auto& t : gTracks) for (auto& c : t.clips) end = std::max(end, c.end());
    if (end <= 0) { setMsg(L"Nothing to export yet"); return; }

    wchar_t path[MAX_PATH] = L"mixdown.wav";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gWnd;
    ofn.lpstrFilter = L"WAV file (*.wav)\0*.wav\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    FILE* f = _wfopen(path, L"wb");
    if (!f) { setMsg(L"Could not write the file"); return; }
    std::vector<Track> snap = gTracks;   // mix a copy, the live tracks can keep playing
    auto writeHeader = [&](uint32_t dataBytes) {
        uint32_t riff = 36 + dataBytes, fmtLen = 16, rate = SR, byteRate = SR * 4;
        uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
        fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f); fwrite(&fmtLen, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
        fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    };
    writeHeader(0);   // patched at the end, when the length (with the instruments' release tail) is known

    // MIDI tracks are played by fresh copies of their instruments' voices (the live ones keep running)
    bool anySolo = false, anyMidi = false;
    for (auto& t : snap) anySolo |= t.solo;
    std::vector<std::unique_ptr<Voices>> engines(snap.size());
    for (size_t i = 0; i < snap.size(); ++i)
        if (snap[i].midi && snap[i].inst && trackAudible(snap[i], anySolo)) { engines[i].reset(new Voices()); anyMidi = true; }

    const int CH = (int)CB_BLOCK;
    const i64 maxTail = anyMidi ? 10 * (i64)SR : 0, quietNeeded = SR / 2;
    std::vector<float> mix(CH * 2);
    std::vector<int16_t> pcmBuf(CH * 2);
    float peak = 0.0f;
    i64 total = 0, quiet = 0;
    for (i64 pos = 0; pos < end + maxTail; pos += CH) {
        int n = (int)std::min<i64>(CH, end + maxTail - pos);
        std::fill(mix.begin(), mix.end(), 0.0f);
        mixTracks(snap, pos, n, mix.data(), nullptr);
        for (size_t i = 0; i < snap.size(); ++i) {
            if (!engines[i]) continue;
            midiTrigger(snap[i], *engines[i], pos, pos + n, pos, nullptr);
            renderMidiTrack(snap[i], *engines[i], pos, n, mix.data());
        }
        float blockPeak = 0.0f;
        for (int i = 0; i < n * 2; ++i) {
            blockPeak = std::max(blockPeak, std::fabs(mix[i]));
            pcmBuf[i] = (int16_t)std::lround(clampf(mix[i], -1.0f, 1.0f) * 32767.0f);
        }
        peak = std::max(peak, blockPeak);
        fwrite(pcmBuf.data(), sizeof(int16_t), n * 2, f);
        total += n;
        if (pos + n >= end) {   // after the last clip: stop once the instruments have faded out
            quiet = blockPeak < 0.0001f ? quiet + n : 0;
            if (quiet >= quietNeeded || pos + n >= end + maxTail) break;
        }
    }
    fseek(f, 0, SEEK_SET);
    writeHeader((uint32_t)(total * 4));
    fclose(f);
    gDirty = false;
    wchar_t m[300];
    swprintf(m, 300, L"Exported %ls  (%.1f s, peak %.1f dB%ls)", fileNameOf(path).c_str(), (double)total / SR,
             peak > 0 ? 20.0 * std::log10(peak) : -99.0, peak > 1.0f ? L" - CLIPPED, lower the faders" : L"");
    setMsg(m);
}

// ============================================================================
// Drawing
// ============================================================================
const Color C_BG(255, 22, 24, 29), C_PANEL(255, 34, 37, 44), C_PANEL_HI(255, 44, 48, 57), C_BORDER(255, 52, 56, 64);
const Color C_ACCENT(255, 255, 138, 61), C_TEXT(255, 230, 230, 230), C_DIM(255, 138, 143, 152);
const Color C_BTN(255, 46, 50, 58), C_CANVAS(255, 18, 20, 24), C_LANE2(255, 22, 24, 29);
const Color C_GRID(255, 40, 44, 52), C_GRID_MINOR(255, 30, 33, 39), C_REC(255, 232, 72, 72);
const Color C_PLAY(255, 90, 210, 120), C_MUTE(255, 255, 138, 61), C_SOLO(255, 240, 205, 70), C_MON(255, 90, 170, 255);
const Color TRACK_COLORS[8] = {
    Color(255, 79, 209, 197), Color(255, 130, 170, 255), Color(255, 240, 110, 150), Color(255, 170, 220, 90),
    Color(255, 200, 140, 255), Color(255, 250, 200, 80), Color(255, 255, 138, 61), Color(255, 100, 200, 255)};

Color mixColor(const Color& a, const Color& b, float t) {
    return Color(255, (BYTE)(a.GetR() + (b.GetR() - a.GetR()) * t), (BYTE)(a.GetG() + (b.GetG() - a.GetG()) * t),
                 (BYTE)(a.GetB() + (b.GetB() - a.GetB()) * t));
}

void drawText(Graphics& g, const wchar_t* s, Gdiplus::Font* f, const Color& c, const RectF& r,
              StringAlignment h = StringAlignmentCenter) {
    StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(s, -1, f, r, &fmt, &b);
}

void roundRect(GraphicsPath& p, const RectF& r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void fillRound(Graphics& g, const RectF& r, float rad, const Color& fill, const Color* border = nullptr) {
    GraphicsPath p;
    roundRect(p, r, rad);
    SolidBrush b(fill);
    g.FillPath(&b, &p);
    if (border) { Pen pen(*border, 1.0f); g.DrawPath(&pen, &p); }
}

void drawButton(Graphics& g, const RectF& r, const wchar_t* text, bool enabled = true) {
    fillRound(g, r, 5, C_BTN, &C_BORDER);
    drawText(g, text, fLabel, enabled ? C_TEXT : C_DIM, r);
}

void drawToggle(Graphics& g, const RectF& r, const wchar_t* text, bool on, const Color& onColor) {
    if (on) {
        fillRound(g, r, 4, onColor);
        drawText(g, text, fLabel, Color(255, 20, 20, 24), r);
    } else {
        fillRound(g, r, 4, C_BTN, &C_BORDER);
        drawText(g, text, fLabel, C_DIM, r);
    }
}

Color meterColor(float db) { return db > -0.1f ? C_REC : db > -9.0f ? C_SOLO : C_PLAY; }
float meterFrac(float lin) {
    if (lin <= 0.000001f) return 0.0f;
    return clampf((20.0f * std::log10(lin) + 60.0f) / 60.0f, 0.0f, 1.0f);
}

float gMeterDisp[MAX_TRACKS], gMasterDisp[2];

void drawWave(Graphics& g, const AudioData& d, i64 off, i64 len, float x0, float x1, float yTop, float h,
              const Color& col) {
    float visL = std::max(x0, laneLeft()), visR = std::min(x1, gW);
    if (visR - visL < 1.0f || len <= 0) return;
    i64 lim = std::min<i64>(off + len, (i64)d.s.size());
    if (lim <= off) return;
    double spp = SR / gPxPerSec;
    float mid = yTop + h * 0.5f, amp = h * 0.5f;
    int nPx = (int)std::ceil(visR - visL);
    std::vector<PointF> pts(2 * (nPx + 1));
    for (int i = 0; i <= nPx; ++i) {
        float x = std::min(visL + i, visR);
        i64 s0 = off + (i64)((x - x0) * spp), s1 = off + (i64)((x + 1.0f - x0) * spp);
        s0 = std::max(off, std::min(s0, lim - 1));
        s1 = std::max(s0 + 1, std::min(s1, lim));
        float mn, mx;
        if (s1 - s0 >= 2 * PEAK_BLOCK) {
            size_t b0 = (size_t)(s0 / PEAK_BLOCK), b1 = (size_t)((s1 - 1) / PEAK_BLOCK);
            mn = d.pkMin[b0];
            mx = d.pkMax[b0];
            for (size_t b = b0 + 1; b <= b1; ++b) { mn = std::min(mn, d.pkMin[b]); mx = std::max(mx, d.pkMax[b]); }
        } else {
            mn = mx = d.s[(size_t)s0];
            for (i64 s = s0 + 1; s < s1; ++s) { mn = std::min(mn, d.s[(size_t)s]); mx = std::max(mx, d.s[(size_t)s]); }
        }
        mn = clampf(mn, -1, 1);
        mx = clampf(mx, -1, 1);
        if (mx - mn < 1.0f / amp) { mx += 0.5f / amp; mn -= 0.5f / amp; }   // at least a hairline
        pts[i] = PointF(x, mid - mx * amp);
        pts[2 * (nPx + 1) - 1 - i] = PointF(x, mid - mn * amp);
    }
    SolidBrush b(col);
    g.FillPolygon(&b, pts.data(), (INT)pts.size());
}

// Notes of a MIDI clip as small bars. base = timeline position of note time 0; only [from, to) is drawn.
void drawMidiNotes(Graphics& g, const std::vector<Note>& notes, i64 base, i64 from, i64 to, float yTop, float h,
                   const Color& col) {
    int lo = 127, hi = 0;
    for (auto& n : notes) {
        if (base + n.start >= to || base + n.start + n.len <= from) continue;
        lo = std::min(lo, n.pitch);
        hi = std::max(hi, n.pitch);
    }
    if (lo > hi) return;
    if (hi - lo < 11) { int mid = (lo + hi) / 2; lo = std::max(0, mid - 6); hi = lo + 11; }   // at least an octave
    float row = h / (hi - lo + 1), nh = std::max(2.0f, std::min(row - 1.0f, 6.0f));
    float clipL = std::max(sampleToX(from), laneLeft()), clipR = std::min(sampleToX(to), gW);
    SolidBrush b(col);
    for (auto& n : notes) {
        i64 a = std::max(base + n.start, from), e = std::min(base + n.start + n.len, to);
        if (a >= e) continue;
        float x0 = std::max(sampleToX(a), clipL), x1 = std::min(sampleToX(e), clipR);
        if (x1 < clipL || x0 > clipR) continue;
        float y = yTop + (hi - n.pitch) * row + (row - nh) / 2;
        g.FillRectangle(&b, x0, y, std::max(1.5f, x1 - x0 - 1), nh);
    }
}

void drawGrid(Graphics& g, bool ruler) {
    double spBar = 240.0 / gBpm.load();
    double pxBar = spBar * gPxPerSec;
    int step = 1;
    while (pxBar * step < 56.0) step *= 2;
    bool beats = step == 1 && pxBar / 4.0 >= 14.0;
    int first = std::max(0, (int)std::floor(gViewStart / spBar));
    first -= first % step;
    Pen major(C_GRID, 1.0f), minor(C_GRID_MINOR, 1.0f), tick(C_DIM, 1.0f);
    float top = ruler ? (float)TOOLBAR_H : lanesTop(), bottom = ruler ? lanesTop() : lanesBottom();
    for (int b = first;; b += step) {
        float x = timeToX(b * spBar);
        if (x > gW) break;
        if (x >= laneLeft()) {
            if (ruler) {
                g.DrawLine(&tick, x, bottom - 10, x, bottom);
                wchar_t s[16];
                swprintf(s, 16, L"%d", b + 1);
                drawText(g, s, fSmall, C_DIM, RectF(x + 3, top + 1, 40, 16), StringAlignmentNear);
            } else {
                g.DrawLine(&major, x, top, x, bottom);
            }
        }
        if (beats)
            for (int k = 1; k < 4; ++k) {
                float xb = timeToX(b * spBar + k * spBar / 4.0);
                if (xb < laneLeft()) continue;
                if (ruler) g.DrawLine(&minor, xb, bottom - 5, xb, bottom);
                else g.DrawLine(&minor, xb, top, xb, bottom);
            }
    }
}

void drawLanes(Graphics& g) {
    RectF area(laneLeft(), lanesTop(), gW - laneLeft(), lanesBottom() - lanesTop());
    g.SetClip(area);
    SolidBrush bg(C_CANVAS);
    g.FillRectangle(&bg, area);
    for (int i = 0; i < (int)gTracks.size(); ++i) {
        float t = trackTop(i);
        if (t > lanesBottom() || t + TRACK_H < lanesTop()) continue;
        Color c = i == gSelTrack ? Color(255, 28, 31, 38) : (i % 2 ? C_LANE2 : C_CANVAS);
        SolidBrush lb(c);
        g.FillRectangle(&lb, laneLeft(), t, gW - laneLeft(), (float)TRACK_H);
    }
    drawGrid(g, false);
    Pen sep(C_BORDER, 1.0f);
    for (int i = 0; i <= (int)gTracks.size(); ++i) {
        float y = trackTop(i) - 0.5f;
        g.DrawLine(&sep, laneLeft(), y, gW, y);
    }

    g.SetSmoothingMode(SmoothingModeNone);
    for (int i = 0; i < (int)gTracks.size(); ++i) {
        float t = trackTop(i);
        if (t > lanesBottom() || t + TRACK_H < lanesTop()) continue;
        const Track& tr = gTracks[i];
        const Color& col = TRACK_COLORS[tr.color % 8];
        Color fill = mixColor(C_CANVAS, col, tr.mute ? 0.10f : 0.22f), wave = tr.mute ? C_DIM : col;
        for (int ci = 0; ci < (int)tr.clips.size(); ++ci) {
            const Clip& c = tr.clips[ci];
            float x0 = sampleToX(c.start), x1 = sampleToX(c.end());
            if (x1 < laneLeft() || x0 > gW) continue;
            RectF r(x0, t + 3, std::max(1.0f, x1 - x0), TRACK_H - 7.0f);
            SolidBrush fb(fill);
            g.FillRectangle(&fb, r);
            if (c.data) drawWave(g, *c.data, c.off, c.len, x0, x1, t + 7, TRACK_H - 15.0f, wave);
            if (c.midi) drawMidiNotes(g, c.midi->notes, c.start - c.off, c.start, c.end(), t + 8, TRACK_H - 17.0f, wave);
            bool sel = i == gSelClipT && ci == gSelClipI;
            Pen border(sel ? Color(255, 255, 255, 255) : mixColor(C_CANVAS, col, 0.7f), sel ? 2.0f : 1.0f);
            g.DrawRectangle(&border, r);
        }
    }
    // Takes being recorded
    if (gRecState == REC_ACTIVE) {
        for (auto& tk : gTakes) {
            int i = trackIndexById(tk.trackId);
            if (i < 0) continue;
            float t = trackTop(i);
            if (tk.midi) {
                i64 now = std::max(gPlayPos.load(), gTakeStart);
                float x0 = sampleToX(gTakeStart), x1 = sampleToX(now);
                RectF r(x0, t + 3, std::max(1.0f, x1 - x0), TRACK_H - 7.0f);
                SolidBrush fb(Color(255, 70, 28, 30));
                g.FillRectangle(&fb, r);
                std::vector<Note> ns = tk.notes;
                for (int n = 0; n < 128; ++n)
                    if (tk.heldAt[n] >= 0) {
                        Note x;
                        x.start = tk.heldAt[n];
                        x.len = std::max<i64>(1, now - x.start);
                        x.pitch = n;
                        ns.push_back(x);
                    }
                drawMidiNotes(g, ns, 0, gTakeStart, now, t + 8, TRACK_H - 17.0f, Color(255, 255, 120, 120));
                Pen border(C_REC, 1.0f);
                g.DrawRectangle(&border, r);
                continue;
            }
            i64 start = gTakeStart - takeLat(tk);
            i64 off = start < 0 ? -start : 0, len = (i64)tk.data->s.size() - off;
            if (len <= 0) continue;
            float x0 = sampleToX(start + off), x1 = sampleToX(start + off + len);
            RectF r(x0, t + 3, std::max(1.0f, x1 - x0), TRACK_H - 7.0f);
            SolidBrush fb(Color(255, 70, 28, 30));
            g.FillRectangle(&fb, r);
            drawWave(g, *tk.data, off, len, x0, x1, t + 7, TRACK_H - 15.0f, Color(255, 255, 120, 120));
            Pen border(C_REC, 1.0f);
            g.DrawRectangle(&border, r);
        }
    }
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    float px = sampleToX(gPlayPos.load());
    Pen ph(gRecording.load() ? C_REC : C_TEXT, 1.0f);
    g.DrawLine(&ph, px, lanesTop(), px, lanesBottom());
    g.ResetClip();
}

void drawRuler(Graphics& g) {
    RectF r(laneLeft(), (float)TOOLBAR_H, gW - laneLeft(), (float)RULER_H);
    SolidBrush bg(C_PANEL);
    g.FillRectangle(&bg, r);
    g.SetClip(r);
    drawGrid(g, true);
    float px = sampleToX(gPlayPos.load());
    PointF tri[3] = {PointF(px - 6, (float)TOOLBAR_H + 12), PointF(px + 6, (float)TOOLBAR_H + 12), PointF(px, lanesTop())};
    SolidBrush pb(gRecording.load() ? C_REC : C_TEXT);
    g.FillPolygon(&pb, tri, 3);
    g.ResetClip();
    Pen line(C_BORDER, 1.0f);
    g.DrawLine(&line, laneLeft(), lanesTop() - 0.5f, gW, lanesTop() - 0.5f);
    // corner above the headers
    SolidBrush cb(C_PANEL);
    g.FillRectangle(&cb, 0.0f, (float)TOOLBAR_H, laneLeft(), (float)RULER_H);
    drawText(g, L"TRACKS", fLabel, C_DIM, RectF(14, (float)TOOLBAR_H, 100, (float)RULER_H), StringAlignmentNear);
    g.DrawLine(&line, 0.0f, lanesTop() - 0.5f, laneLeft(), lanesTop() - 0.5f);
}

void drawSlider(Graphics& g, const RectF& r, float v, bool bipolar, const Color& col) {
    float cy = r.Y + r.Height / 2;
    fillRound(g, RectF(r.X, cy - 2, r.Width, 4), 2, C_BTN);
    float kx = r.X + v * r.Width;
    float a = bipolar ? r.X + r.Width / 2 : r.X;
    if (std::fabs(kx - a) > 0.5f) fillRound(g, RectF(std::min(a, kx), cy - 2, std::fabs(kx - a), 4), 2, col);
    SolidBrush kb(C_TEXT);
    g.FillEllipse(&kb, kx - 5, cy - 5, 10.0f, 10.0f);
}

void drawHeaders(Graphics& g) {
    RectF col(0, lanesTop(), laneLeft(), lanesBottom() - lanesTop());
    g.SetClip(col);
    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, col);
    for (int i = 0; i < (int)gTracks.size(); ++i) {
        float t = trackTop(i);
        if (t > lanesBottom() || t + TRACK_H < lanesTop()) continue;
        const Track& tr = gTracks[i];
        const Color& tc = TRACK_COLORS[tr.color % 8];
        SolidBrush hb(i == gSelTrack ? C_PANEL_HI : C_PANEL);
        g.FillRectangle(&hb, 0.0f, t, laneLeft(), (float)TRACK_H);
        SolidBrush sb(tc);
        g.FillRectangle(&sb, 0.0f, t, 5.0f, (float)TRACK_H);
        Pen sep(C_BORDER, 1.0f);
        g.DrawLine(&sep, 0.0f, t + TRACK_H - 0.5f, laneLeft(), t + TRACK_H - 0.5f);
        g.DrawLine(&sep, laneLeft() - 0.5f, t, laneLeft() - 0.5f, t + TRACK_H);

        if (!(gRenameEdit && gRenameId == tr.id))
            drawText(g, tr.name.c_str(), fTitle, C_TEXT, partRect(i, P_NAME), StringAlignmentNear);
        RectF d = partRect(i, P_DEL);
        Pen xp(C_DIM, 1.5f);
        g.DrawLine(&xp, d.X + 6, d.Y + 6, d.X + 14, d.Y + 14);
        g.DrawLine(&xp, d.X + 14, d.Y + 6, d.X + 6, d.Y + 14);

        drawToggle(g, partRect(i, P_ARM), L"R", tr.arm, C_REC);
        drawToggle(g, partRect(i, P_MUTE), L"M", tr.mute, C_MUTE);
        drawToggle(g, partRect(i, P_SOLO), L"S", tr.solo, C_SOLO);
        if (tr.midi) {
            if (tr.id == gLiveTrackId) {   // the PC keys / MIDI keyboard play this track
                RectF bd(166, t + 10, 40, 16);
                fillRound(g, bd, 3, tc);
                drawText(g, L"KEYS", fSmall, Color(255, 20, 20, 24), bd);
            }
            RectF ib = partRect(i, P_INPUT);
            fillRound(g, ib, 5, C_BTN, &tc);
            // what the track plays (its type is already on the badge): the sound / kit, or "Edit sound"
            std::wstring sound = tr.synth() ? std::wstring(L"∿  Edit sound") : L"♪  " + smpSoundName(*tr.inst);
            drawText(g, sound.c_str(), fLabel, C_TEXT, ib);
        } else {
            if (tr.vocal) {   // marks the track as a vocal track
                RectF bd(164, t + 10, 46, 16);
                fillRound(g, bd, 3, tc);
                drawText(g, L"VOCAL", fSmall, Color(255, 20, 20, 24), bd);
            }
            drawToggle(g, partRect(i, P_MON), L"I", tr.mon, C_MON);
            static const wchar_t* inNames[3] = {L"In 1", L"In 2", L"In 1+2"};
            drawButton(g, partRect(i, P_INPUT), inNames[tr.input % 3]);
        }

        drawSlider(g, partRect(i, P_VOL), tr.fader, false, tc);
        wchar_t db[24];
        if (tr.fader <= 0.001f) wcscpy(db, L"-inf");
        else swprintf(db, 24, L"%+.1f", faderDb(tr.fader));
        drawText(g, db, fSmall, C_DIM, RectF(128, t + 64, 44, 22), StringAlignmentCenter);
        drawSlider(g, partRect(i, P_PAN), (tr.pan + 1) / 2, true, tc);

        RectF m = meterRect(i);
        SolidBrush mb(C_CANVAS);
        g.FillRectangle(&mb, m);
        float lv = i < MAX_TRACKS ? gMeterDisp[i] : 0.0f, fr = meterFrac(lv);
        if (fr > 0) {
            SolidBrush lb(meterColor(20.0f * std::log10(lv)));
            g.FillRectangle(&lb, m.X, m.Y + m.Height * (1 - fr), m.Width, m.Height * fr);
        }
    }
    RectF ar = addTrackRect(), vr = addVocalRect(), mr = addMidiRect(), sr = addSynthRect();
    if (ar.Y < lanesBottom()) {
        Color dash(255, 70, 75, 85);
        fillRound(g, ar, 5, C_BG, &dash);
        drawText(g, L"+ Audio", fLabel, C_DIM, ar);
        fillRound(g, vr, 5, C_BG, &dash);
        drawText(g, L"+ Vocal", fLabel, C_DIM, vr);
        fillRound(g, mr, 5, C_BG, &dash);
        drawText(g, L"+ MIDI", fLabel, C_DIM, mr);
        fillRound(g, sr, 5, C_BG, &dash);
        drawText(g, L"+ Synth", fLabel, C_DIM, sr);
    }
    g.ResetClip();
}

void drawTransportIcon(Graphics& g, const RectF& r, int kind, bool active) {
    Color bgc = C_BTN;
    if (active) bgc = kind == 3 ? Color(255, 110, 30, 32) : Color(255, 30, 80, 46);
    fillRound(g, r, 5, bgc, &C_BORDER);
    float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    SolidBrush b(C_TEXT);
    if (kind == 0) {   // go to start
        g.FillRectangle(&b, cx - 8, cy - 7, 3.0f, 14.0f);
        PointF p[3] = {PointF(cx + 7, cy - 7), PointF(cx + 7, cy + 7), PointF(cx - 4, cy)};
        g.FillPolygon(&b, p, 3);
    } else if (kind == 1) {   // play
        SolidBrush pb(active ? C_PLAY : C_TEXT);
        PointF p[3] = {PointF(cx - 6, cy - 8), PointF(cx - 6, cy + 8), PointF(cx + 8, cy)};
        g.FillPolygon(&pb, p, 3);
    } else if (kind == 2) {   // stop
        g.FillRectangle(&b, cx - 6, cy - 6, 12.0f, 12.0f);
    } else {   // record
        bool blink = active && (GetTickCount() / 400) % 2;
        SolidBrush rb(blink ? Color(255, 255, 140, 140) : C_REC);
        g.FillEllipse(&rb, cx - 7, cy - 7, 14.0f, 14.0f);
    }
}

void drawToolbar(Graphics& g) {
    SolidBrush bg(C_PANEL);
    g.FillRectangle(&bg, 0.0f, 0.0f, gW, (float)TOOLBAR_H);
    Pen line(C_BORDER, 1.0f);
    g.DrawLine(&line, 0.0f, TOOLBAR_H - 0.5f, gW, TOOLBAR_H - 0.5f);

    drawText(g, L"IN", fLabel, C_DIM, RectF(12, 10, 28, 26), StringAlignmentNear);
    drawText(g, L"OUT", fLabel, C_DIM, RectF(302, 10, 40, 26), StringAlignmentNear);
    drawText(g, L"MIDI", fLabel, C_DIM, RectF(688, 10, 36, 26), StringAlignmentNear);
    {   // latency: click to switch low latency (exclusive output) on / off
        std::wstring t = gExclusive ? L"⚡ Low latency · " : gLowLat ? L"Low latency n/a · " : L"Low latency off · ";
        drawToggle(g, lowLatRect(), (t + gAudioStatus).c_str(), gExclusive, C_ACCENT);
    }

    bool playing = gPlaying.load();
    drawTransportIcon(g, btnHome, 0, false);
    drawTransportIcon(g, btnPlay, 1, playing);
    drawTransportIcon(g, btnStop, 2, false);
    drawTransportIcon(g, btnRec, 3, gRecState != REC_IDLE);

    // Time
    fillRound(g, timeBox, 5, C_CANVAS, &C_BORDER);
    double sec = (double)gPlayPos.load() / SR;
    int mins = (int)(sec / 60);
    double rest = sec - mins * 60;
    wchar_t ts[32];
    swprintf(ts, 32, L"%02d:%06.3f", mins, rest);
    drawText(g, ts, fTime, gRecording.load() ? C_REC : C_ACCENT, RectF(timeBox.X + 6, timeBox.Y, 116, timeBox.Height),
             StringAlignmentNear);
    double beats = sec * gBpm.load() / 60.0;
    swprintf(ts, 32, L"%d . %d", (int)(beats / 4) + 1, (int)beats % 4 + 1);
    drawText(g, L"BAR", fSmall, C_DIM, RectF(timeBox.X + 124, timeBox.Y + 1, 60, 14));
    drawText(g, ts, fLabel, C_TEXT, RectF(timeBox.X + 124, timeBox.Y + 14, 60, 18));

    // Tempo / metronome / latency
    wchar_t s[32];
    fillRound(g, bpmBox, 5, C_BTN, &C_BORDER);
    drawText(g, L"TEMPO", fSmall, C_DIM, RectF(bpmBox.X, bpmBox.Y + 1, bpmBox.Width, 14));
    swprintf(s, 32, L"%.0f BPM", gBpm.load());
    drawText(g, s, fLabel, C_TEXT, RectF(bpmBox.X, bpmBox.Y + 14, bpmBox.Width, 18));
    drawToggle(g, clickBtn, L"Click", gMetronome.load(), C_ACCENT);
    fillRound(g, latBox, 5, C_BTN, &C_BORDER);
    drawText(g, L"REC OFFSET", fSmall, C_DIM, RectF(latBox.X, latBox.Y + 1, latBox.Width, 14));
    swprintf(s, 32, L"%.1f ms", gLatCompMs);
    drawText(g, s, fLabel, C_TEXT, RectF(latBox.X, latBox.Y + 14, latBox.Width, 18));

    drawButton(g, btnImport, L"Import");
    drawButton(g, btnExport, L"Export");

    // Master meter
    float mx = std::max(btnExport.X + btnExport.Width + 24, gW - 260);
    drawText(g, L"MASTER", fSmall, C_DIM, RectF(mx, 48, 60, 14), StringAlignmentNear);
    for (int c = 0; c < 2; ++c) {
        RectF r(mx, 64.0f + c * 9, gW - 12 - mx, 6);
        SolidBrush mb(C_CANVAS);
        g.FillRectangle(&mb, r);
        float lv = gMasterDisp[c], fr = meterFrac(lv);
        if (fr > 0) {
            SolidBrush lb(meterColor(20.0f * std::log10(lv)));
            g.FillRectangle(&lb, r.X, r.Y, r.Width * fr, r.Height);
        }
    }
}

void drawStatus(Graphics& g) {
    RectF r(0, lanesBottom(), gW, (float)STATUS_H);
    SolidBrush bg(C_PANEL);
    g.FillRectangle(&bg, r);
    Pen line(C_BORDER, 1.0f);
    g.DrawLine(&line, 0.0f, r.Y + 0.5f, gW, r.Y + 0.5f);
    bool recent = !gMsg.empty() && GetTickCount() - gMsgTime < 5000;
    const wchar_t* hint = L"Space play/stop  ·  R record  ·  S split  ·  Del delete  ·  Ctrl+Z undo  ·  drag clips  ·  "
                          L"double-click a MIDI clip: piano roll  ·  double-click an empty MIDI lane: new clip  ·  "
                          L"Ctrl+wheel zoom  ·  Shift+wheel scroll  ·  double-click name to rename";
    drawText(g, recent ? gMsg.c_str() : hint, fSmall, recent ? C_TEXT : C_DIM, RectF(12, r.Y, gW - 24, r.Height),
             StringAlignmentNear);
}

// Sampler window (edits the live MIDI track's instrument) and piano roll window. Use the helpers above.
#include "sampler.h"
#include "pianoroll.h"
#include "synth.h"

void paint(HDC hdc) {
    RECT rc;
    GetClientRect(gWnd, &rc);
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
        drawLanes(g);
        drawHeaders(g);
        drawRuler(g);
        drawToolbar(g);
        drawStatus(g);
    }
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ============================================================================
// Device combo boxes
// ============================================================================
std::wstring comboText(HWND combo) {
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return L"";
    int len = (int)SendMessageW(combo, CB_GETLBTEXTLEN, sel, 0);
    std::wstring s(len, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, sel, (LPARAM)&s[0]);
    return s;
}

void fillCombos() {
    SendMessageW(gInCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(gInCombo, CB_ADDSTRING, 0, (LPARAM)L"(No input)");
    SendMessageW(gInCombo, CB_ADDSTRING, 0, (LPARAM)L"System default");
    for (auto& d : gCapDevs) SendMessageW(gInCombo, CB_ADDSTRING, 0, (LPARAM)utf8ToWide(d.name).c_str());
    SendMessageW(gOutCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(gOutCombo, CB_ADDSTRING, 0, (LPARAM)L"System default");
    for (auto& d : gPlayDevs) SendMessageW(gOutCombo, CB_ADDSTRING, 0, (LPARAM)utf8ToWide(d.name).c_str());
}

void openFromCombos() {
    int in = (int)SendMessageW(gInCombo, CB_GETCURSEL, 0, 0);
    int out = (int)SendMessageW(gOutCombo, CB_GETCURSEL, 0, 0);
    openAudio(in <= 0 ? -2 : in - 2, out - 1);
}

std::wstring ansiRoundTrip(const std::wstring& s) {   // what an ANSI ini file makes of a name (® -> R ...)
    int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, NULL, 0, NULL, NULL);
    if (n <= 1) return L"";
    std::string a(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, &a[0], n, NULL, NULL);
    int m = MultiByteToWideChar(CP_ACP, 0, a.c_str(), -1, NULL, 0);
    if (m <= 1) return L"";
    std::wstring w(m - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, a.c_str(), -1, &w[0], m);
    return w;
}

void selectByText(HWND combo, const std::wstring& text, int fallback) {
    int i = text.empty() ? -1 : (int)SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)text.c_str());
    if (i < 0 && !text.empty()) {   // saved by an older version in an ANSI file: compare the same way
        int n = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
        for (int k = 0; k < n && i < 0; ++k) {
            int len = (int)SendMessageW(combo, CB_GETLBTEXTLEN, k, 0);
            std::wstring s(len, L'\0');
            SendMessageW(combo, CB_GETLBTEXT, k, (LPARAM)&s[0]);
            if (ansiRoundTrip(s) == text) i = k;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, i >= 0 ? i : fallback, 0);
}

// The chosen devices and the low latency switch are remembered in minidaw.ini next to the program
std::wstring settingsPath() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return L"";
    std::wstring p = exe;
    return p.substr(0, p.find_last_of(L"\\/")) + L"\\minidaw.ini";
}

void saveAudioSettings() {
    std::wstring f = settingsPath();
    if (f.empty()) return;
    // A UTF-16 file keeps the device names exactly (in an ANSI file characters such as ® are lost and the
    // device is not found again). All the keys are written below, so an old ANSI file is simply replaced.
    unsigned char bom[2] = {0, 0};
    size_t got = 0;
    if (FILE* fp = _wfopen(f.c_str(), L"rb")) { got = fread(bom, 1, 2, fp); fclose(fp); }
    if (got < 2 || bom[0] != 0xFF || bom[1] != 0xFE)
        if (FILE* fp = _wfopen(f.c_str(), L"wb")) { fputc(0xFF, fp); fputc(0xFE, fp); fclose(fp); }
    WritePrivateProfileStringW(L"audio", L"in", comboText(gInCombo).c_str(), f.c_str());
    WritePrivateProfileStringW(L"audio", L"out", comboText(gOutCombo).c_str(), f.c_str());
    WritePrivateProfileStringW(L"audio", L"lowLatency", gLowLat ? L"1" : L"0", f.c_str());
}

void initDevices() {
    enumerateAudio();
    fillCombos();
    std::wstring f = settingsPath();
    wchar_t in[256] = L"", out[256] = L"";
    if (!f.empty()) {
        GetPrivateProfileStringW(L"audio", L"in", L"", in, 256, f.c_str());
        GetPrivateProfileStringW(L"audio", L"out", L"", out, 256, f.c_str());
        gLowLat = GetPrivateProfileIntW(L"audio", L"lowLatency", 1, f.c_str()) != 0;
    }
    selectByText(gInCombo, in, 1);     // default: system default input
    selectByText(gOutCombo, out, 0);   // default: system default output
    openFromCombos();
}

void rescanDevices() {
    std::wstring in = comboText(gInCombo), out = comboText(gOutCombo);
    closeAudio();   // device infos are about to be replaced
    enumerateAudio();
    fillCombos();
    selectByText(gInCombo, in, 1);
    selectByText(gOutCombo, out, 0);
    openFromCombos();
}

// ============================================================================
// Rename
// ============================================================================
void beginRename(int i) {
    if (gRenameEdit) return;
    gRenameId = gTracks[i].id;
    RectF r = partRect(i, P_NAME);
    gRenameEdit = CreateWindowExW(0, L"EDIT", gTracks[i].name.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                  S(r.X), S(r.Y), S(r.Width), S(r.Height), gWnd, (HMENU)(INT_PTR)ID_RENAME, NULL, NULL);
    SendMessageW(gRenameEdit, WM_SETFONT, (WPARAM)gUiFont, TRUE);
    SendMessageW(gRenameEdit, EM_SETSEL, 0, -1);
    SetFocus(gRenameEdit);
}

void endRename(bool apply) {
    if (!gRenameEdit) return;
    HWND h = gRenameEdit;
    gRenameEdit = NULL;
    if (apply) {
        wchar_t buf[128];
        GetWindowTextW(h, buf, 128);
        int i = trackIndexById(gRenameId);
        if (i >= 0 && buf[0]) {
            std::lock_guard<std::mutex> lk(gMx);
            gTracks[i].name = buf;
        }
    }
    DestroyWindow(h);
    SetFocus(gWnd);
}

// ============================================================================
// Keyboard
// ============================================================================
bool handleKey(const MSG& m) {
    if (samplerHandleKey(m)) return true;
    if (prHandleKey(m)) return true;
    if (synHandleKey(m)) return true;
    if (gRenameEdit && m.hwnd == gRenameEdit && (m.wParam == VK_RETURN || m.wParam == VK_ESCAPE)) {
        if (m.message == WM_KEYDOWN) { endRename(m.wParam == VK_RETURN); return true; }
        if (m.message == WM_CHAR) return true;   // no beep
    }
    if (m.message != WM_KEYDOWN) return false;
    if (GetAncestor(m.hwnd, GA_ROOT) != gWnd) return false;
    if (gRenameEdit && GetFocus() == gRenameEdit) return false;
    if (SendMessageW(gInCombo, CB_GETDROPPEDSTATE, 0, 0) || SendMessageW(gOutCombo, CB_GETDROPPEDSTATE, 0, 0) ||
        SendMessageW(gSmpMidiCombo, CB_GETDROPPEDSTATE, 0, 0))
        return false;
    WPARAM vk = m.wParam;
    if (GetKeyState(VK_CONTROL) < 0) {
        if (vk == 'Z') { undo(); return true; }
        if (vk == 'T') { addTrack(GetKeyState(VK_SHIFT) < 0 ? TT_MIDI : TT_AUDIO); return true; }
        if (vk == 'Y') { addTrack(TT_SYNTH); return true; }
        if (vk == 'V') { addTrack(TT_VOCAL); return true; }
        return false;
    }
    switch (vk) {
    case VK_SPACE:
        if (gPlaying.load() || gRecState != REC_IDLE) transportStop(); else transportPlay();
        return true;
    case 'R': transportRecord(); return true;
    case VK_HOME: seekTo(0); gViewStart = 0; return true;
    case VK_END: {
        i64 end = 0;
        for (auto& t : gTracks) for (auto& c : t.clips) end = std::max(end, c.end());
        seekTo(end);
        ensureTimeVisible(end);
        return true;
    }
    case VK_DELETE: case VK_BACK: deleteSelectedClip(); return true;
    case 'S': splitAtPlayhead(); return true;
    case VK_OEM_PLUS: case VK_ADD: zoomAt(sampleToX(gPlayPos.load()), 1.5); return true;
    case VK_OEM_MINUS: case VK_SUBTRACT: zoomAt(sampleToX(gPlayPos.load()), 1 / 1.5); return true;
    case VK_UP: if (gSelTrack > 0) { selectTrack(gSelTrack - 1); ensureTrackVisible(gSelTrack); } return true;
    case VK_DOWN: if (gSelTrack + 1 < (int)gTracks.size()) { selectTrack(gSelTrack + 1); ensureTrackVisible(gSelTrack); } return true;
    }
    return false;
}

// ============================================================================
// Mouse
// ============================================================================
enum Drag { D_NONE, D_SEEK, D_VOL, D_PAN, D_BPM, D_LAT, D_CLIP } gDrag = D_NONE;
int gDragTrack = -1;
float gDragStartY = 0, gDragStartVal = 0;
i64 gDragGrab = 0;
bool gDragMoved = false;
std::vector<Track> gDragUndo;

void setFader(int i, float x) {
    RectF r = partRect(i, P_VOL);
    float v = clampf((x - r.X) / r.Width, 0, 1);
    if (std::fabs(v - FADER_0DB) < 0.012f) v = FADER_0DB;   // sticky 0 dB
    std::lock_guard<std::mutex> lk(gMx);
    gTracks[i].fader = v;
}

void setPan(int i, float x) {
    RectF r = partRect(i, P_PAN);
    float v = clampf((x - r.X) / r.Width, 0, 1) * 2 - 1;
    if (std::fabs(v) < 0.08f) v = 0;
    std::lock_guard<std::mutex> lk(gMx);
    gTracks[i].pan = v;
}

void showFaderMsg(int i) {
    wchar_t m[128];
    const Track& t = gTracks[i];
    if (gDrag == D_PAN) {
        int p = (int)std::lround(t.pan * 100);
        if (p == 0) swprintf(m, 128, L"%ls  pan: centre", t.name.c_str());
        else swprintf(m, 128, L"%ls  pan: %ls %d", t.name.c_str(), p < 0 ? L"L" : L"R", std::abs(p));
    } else {
        if (t.fader <= 0.001f) swprintf(m, 128, L"%ls  volume: -inf dB", t.name.c_str());
        else swprintf(m, 128, L"%ls  volume: %+.1f dB", t.name.c_str(), faderDb(t.fader));
    }
    setMsg(m);
}

int clipAt(int ti, float x) {   // topmost clip under x on track ti
    auto& cl = gTracks[ti].clips;
    for (int ci = (int)cl.size() - 1; ci >= 0; --ci)
        if (x >= sampleToX(cl[ci].start) && x <= sampleToX(cl[ci].end())) return ci;
    return -1;
}

void onMouseDown(float x, float y, bool dbl) {
    if (y < TOOLBAR_H) {
        if (btnHome.Contains(x, y)) { seekTo(0); gViewStart = 0; }
        else if (btnPlay.Contains(x, y)) transportPlay();
        else if (btnStop.Contains(x, y)) transportStop();
        else if (btnRec.Contains(x, y)) transportRecord();
        else if (bpmBox.Contains(x, y)) {
            if (dbl) gBpm = 120.0f;
            else { gDrag = D_BPM; gDragStartY = y; gDragStartVal = gBpm.load(); }
        } else if (clickBtn.Contains(x, y)) gMetronome = !gMetronome.load();
        else if (latBox.Contains(x, y)) { gDrag = D_LAT; gDragStartY = y; gDragStartVal = gLatCompMs; }
        else if (btnImport.Contains(x, y)) importDialog();
        else if (btnExport.Contains(x, y)) exportMix();
        else if (lowLatRect().Contains(x, y)) {
            gLowLat = !gLowLat;
            openFromCombos();
            saveAudioSettings();
            if (gLowLat && !gExclusive)
                setMsg(L"Low latency is not available on this output (virtual devices such as sound enhancers do "
                       L"not support it) - pick your audio interface or speakers in OUT");
            else if (gExclusive)
                setMsg(L"Low latency on: MiniDAW uses this output alone, other programs are silent on it until you turn it off");
            else setMsg(L"Low latency off: normal Windows output, shared with other programs");
        }
        return;
    }
    if (y >= lanesBottom()) return;
    if (y < lanesTop()) {   // ruler
        if (x >= laneLeft()) { seekTo(xToSample(x)); gDrag = D_SEEK; }
        return;
    }
    int ti = trackAtY(y);
    if (x < laneLeft()) {   // track headers
        if (ti < 0) {
            if (addTrackRect().Contains(x, y)) addTrack(TT_AUDIO);
            else if (addVocalRect().Contains(x, y)) addTrack(TT_VOCAL);
            else if (addMidiRect().Contains(x, y)) addTrack(TT_MIDI);
            else if (addSynthRect().Contains(x, y)) addTrack(TT_SYNTH);
            return;
        }
        selectTrack(ti);
        Track& t = gTracks[ti];
        switch (hitPart(ti, x, y)) {
        case P_DEL: deleteTrack(ti); break;
        case P_ARM:
            { std::lock_guard<std::mutex> lk(gMx); t.arm = !t.arm; }
            if (t.midi && t.arm) setMsg(L"Armed: notes you play in the Sampler window or on a MIDI keyboard are recorded on this track");
            break;
        case P_MUTE: { std::lock_guard<std::mutex> lk(gMx); t.mute = !t.mute; } break;
        case P_SOLO: { std::lock_guard<std::mutex> lk(gMx); t.solo = !t.solo; } break;
        case P_MON:
            { std::lock_guard<std::mutex> lk(gMx); t.mon = !t.mon; }
            if (t.mon) setMsg(L"Input monitoring on (only heard while the track is armed) - use headphones to avoid feedback");
            break;
        case P_INPUT: {
            if (t.midi) { instOpenWindow(); break; }   // the track's instrument (it is live now)
            std::lock_guard<std::mutex> lk(gMx);
            t.input = (t.input + 1) % 3;
            break;
        }
        case P_VOL:
            if (dbl) { std::lock_guard<std::mutex> lk(gMx); t.fader = FADER_0DB; }
            else { gDrag = D_VOL; gDragTrack = ti; setFader(ti, x); }
            showFaderMsg(ti);
            break;
        case P_PAN:
            gDrag = D_PAN;
            if (dbl) { std::lock_guard<std::mutex> lk(gMx); t.pan = 0; }
            else { gDragTrack = ti; setPan(ti, x); }
            showFaderMsg(ti);
            if (dbl) gDrag = D_NONE;
            break;
        case P_NAME: if (dbl) beginRename(ti); break;
        }
        return;
    }
    // lanes
    if (ti < 0) { clearClipSel(); return; }
    selectTrack(ti);
    int ci = clipAt(ti, x);
    if (dbl && gTracks[ti].midi) {   // MIDI: open the clip in the piano roll, or make a new clip there
        int uid = ci >= 0 ? gTracks[ti].clips[ci].uid : createMidiClip(ti, xToSample(x));
        if (uid) prOpen(uid);
        return;
    }
    if (ci >= 0) {
        gSelClipT = ti;
        gSelClipI = ci;
        gDrag = D_CLIP;
        gDragGrab = xToSample(x) - gTracks[ti].clips[ci].start;
        gDragMoved = false;
        gDragUndo = gTracks;
    } else {
        clearClipSel();
        seekTo(xToSample(x));
    }
}

void onMouseMove(float x, float y, bool shift) {
    switch (gDrag) {
    case D_SEEK: seekTo(xToSample(std::max(x, laneLeft()))); break;
    case D_VOL:  if (gDragTrack < (int)gTracks.size()) { setFader(gDragTrack, x); showFaderMsg(gDragTrack); } break;
    case D_PAN:  if (gDragTrack < (int)gTracks.size()) { setPan(gDragTrack, x); showFaderMsg(gDragTrack); } break;
    case D_BPM:  gBpm = clampf(std::round(gDragStartVal + (gDragStartY - y) * (shift ? 0.1f : 0.5f)), 30, 300); break;
    case D_LAT:  gLatCompMs = clampf(std::round((gDragStartVal + (gDragStartY - y) * (shift ? 0.05f : 0.5f)) * 10) / 10, 0, 500); break;
    case D_CLIP: {
        if (!selClipValid()) { gDrag = D_NONE; break; }
        i64 ns = std::max<i64>(0, xToSample(x) - gDragGrab);
        int nt = trackAtY(y);
        if (nt < 0 || gTracks[nt].midi != gTracks[gSelClipT].midi || gTracks[nt].vocal != gTracks[gSelClipT].vocal)
            nt = gSelClipT;   // same kind of track only (vocal clips stay on vocal tracks)
        Clip& c = gTracks[gSelClipT].clips[gSelClipI];
        if (ns == c.start && nt == gSelClipT) break;
        if (!gDragMoved) { pushUndoState(std::move(gDragUndo)); gDragMoved = true; }
        std::lock_guard<std::mutex> lk(gMx);
        if (nt == gSelClipT) {
            c.start = ns;
        } else {   // move to another track (it lands on top there)
            Clip moved = c;
            moved.start = ns;
            auto& from = gTracks[gSelClipT].clips;
            from.erase(from.begin() + gSelClipI);
            gTracks[nt].clips.push_back(moved);
            gSelClipT = nt;
            gSelClipI = (int)gTracks[nt].clips.size() - 1;
            gSelTrack = nt;
        }
        break;
    }
    default: break;
    }
}

void endDrag() {
    if (gDrag == D_CLIP && gDragMoved) gDirty = true;
    gDrag = D_NONE;
    gDragUndo.clear();
}

// ============================================================================
// Window
// ============================================================================
void onTimer() {
    pollRecording();
    for (int i = 0; i < MAX_TRACKS; ++i) gMeterDisp[i] = std::max(gMeter[i].exchange(0.0f), gMeterDisp[i] * 0.82f);
    for (int c = 0; c < 2; ++c) gMasterDisp[c] = std::max(gMasterPeak[c].exchange(0.0f), gMasterDisp[c] * 0.82f);
    if (gPlaying.load() && gDrag == D_NONE) {   // follow the playhead, page by page
        float x = sampleToX(gPlayPos.load());
        if (x > gW - 30 && x < gW + 80) gViewStart += (gW - laneLeft() - 80) / gPxPerSec;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        gWnd = hwnd;
        gUiFont = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        gEditBrush = CreateSolidBrush(RGB(18, 20, 24));
        gInCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 S(40), S(10), S(252), S(300), hwnd, (HMENU)(INT_PTR)ID_INCOMBO, NULL, NULL);
        gOutCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  S(344), S(10), S(252), S(300), hwnd, (HMENU)(INT_PTR)ID_OUTCOMBO, NULL, NULL);
        gRescanBtn = CreateWindowW(L"BUTTON", L"Rescan", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   S(606), S(9), S(70), S(26), hwnd, (HMENU)(INT_PTR)ID_RESCAN, NULL, NULL);
        gSmpMidiCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      S(724), S(10), S(170), S(300), hwnd, (HMENU)(INT_PTR)ID_SMP_MIDI, NULL, NULL);
        for (HWND h : {gInCombo, gOutCombo, gRescanBtn, gSmpMidiCombo}) SendMessageW(h, WM_SETFONT, (WPARAM)gUiFont, TRUE);
        DragAcceptFiles(hwnd, TRUE);
        initDevices();
        samplerStartup();   // opens the first MIDI keyboard, fills the MIDI list
        SetTimer(hwnd, 1, 30, NULL);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        if ((HWND)lParam == gRenameEdit) {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(230, 230, 230));
            SetBkColor(dc, RGB(18, 20, 24));
            return (LRESULT)gEditBrush;
        }
        break;
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if ((id == ID_INCOMBO || id == ID_OUTCOMBO) && code == CBN_SELCHANGE) { openFromCombos(); saveAudioSettings(); SetFocus(hwnd); }
        else if (id == ID_RESCAN && code == BN_CLICKED) { rescanDevices(); smpFillMidi(); SetFocus(hwnd); }
        else if (id == ID_SMP_MIDI && code == CBN_SELCHANGE) {
            smpOpenMidi((int)SendMessageW(gSmpMidiCombo, CB_GETCURSEL, 0, 0) - 1);
            SetFocus(hwnd);
        }
        else if (id == ID_RENAME && code == EN_KILLFOCUS) PostMessageW(hwnd, WM_APP_RENAME_DONE, 0, 0);
        return 0;
    }
    case WM_APP_RENAME_DONE:
        endRename(true);
        return 0;
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
        int ti = trackAtY(y);
        i64 pos = (ti >= 0 && x >= laneLeft()) ? xToSample(x) : gPlayPos.load();
        importFiles(files, ti, pos);
        return 0;
    }
    case WM_TIMER:
        onTimer();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SIZE:
        gW = LOWORD(lParam) / gScale;
        gH = HIWORD(lParam) / gScale;
        clampScroll();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = S(1100);
        mm->ptMinTrackSize.y = S(460);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(hwnd);
        onMouseDown(GET_X_LPARAM(lParam) / gScale, GET_Y_LPARAM(lParam) / gScale, msg == WM_LBUTTONDBLCLK);
        if (gDrag != D_NONE) SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (gDrag != D_NONE) onMouseMove(GET_X_LPARAM(lParam) / gScale, GET_Y_LPARAM(lParam) / gScale, (wParam & MK_SHIFT) != 0);
        return 0;
    case WM_LBUTTONUP:
        endDrag();
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        endDrag();
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        if (gRenameEdit) endRename(true);
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        float x = pt.x / gScale, y = pt.y / gScale;
        float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        WORD keys = GET_KEYSTATE_WPARAM(wParam);
        if (msg == WM_MOUSEHWHEEL) { gViewStart += notches * 80 / gPxPerSec; clampScroll(); return 0; }
        if (bpmBox.Contains(x, y)) { gBpm = clampf(gBpm.load() + notches, 30, 300); return 0; }
        if (latBox.Contains(x, y)) { gLatCompMs = clampf(gLatCompMs + notches * ((keys & MK_SHIFT) ? 0.1f : 1.0f), 0, 500); return 0; }
        if (keys & MK_CONTROL) zoomAt(x, std::pow(1.25, notches));
        else if ((keys & MK_SHIFT) || (y >= TOOLBAR_H && y < lanesTop())) gViewStart -= notches * 80 / gPxPerSec;
        else gScrollY -= notches * TRACK_H / 2;
        clampScroll();
        return 0;
    }
    case WM_ACTIVATE:
        return 0;
    case WM_CLOSE:
        if (gDirty && MessageBoxW(hwnd, L"There are recordings that have not been exported.\nClose anyway?", L"MiniDAW",
                                  MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        samplerShutdown();
        closeAudio();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    gScale = GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    ReleaseDC(NULL, screen);

    gRing.init((size_t)SR * IN_CH * 20);   // 20 s of input (In 1, In 2)
    gMidiRing.init(65536);
    addTrackNoUndo(TT_AUDIO);            // start with an audio track (armed), a vocal track, a MIDI track and a Synth track
    addTrackNoUndo(TT_VOCAL);
    addTrackNoUndo(TT_MIDI);
    addTrackNoUndo(TT_SYNTH);
    gSelTrack = 0;
    validateLive();

    if (ma_context_init(NULL, 0, NULL, &gContext) != MA_SUCCESS) {
        MessageBoxW(NULL, L"Failed to initialize audio context.", L"MiniDAW", MB_ICONERROR);
        return -1;
    }

    GdiplusStartupInput gsi;
    ULONG_PTR gdipToken;
    GdiplusStartup(&gdipToken, &gsi, NULL);
    {
        FontFamily family(L"Segoe UI"), mono(L"Consolas");   // scoped: destroyed before GdiplusShutdown
        fTitle = new Gdiplus::Font(&family, 12.5f, FontStyleBold, UnitPixel);
        fLabel = new Gdiplus::Font(&family, 11.5f, FontStyleBold, UnitPixel);
        fSmall = new Gdiplus::Font(&family, 10.5f, FontStyleRegular, UnitPixel);
        fTime  = new Gdiplus::Font(&mono, 19.0f, FontStyleBold, UnitPixel);
    }

    WNDCLASSW wc = {};
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"MiniDAWWindow";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT r = {0, 0, S(CLIENT_W), S(CLIENT_H)};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MiniDAW", style, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetFocus(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (handleKey(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete fTitle; delete fLabel; delete fSmall; delete fTime;
    GdiplusShutdown(gdipToken);
    DeleteObject(gUiFont);
    DeleteObject(gEditBrush);
    ma_context_uninit(&gContext);
    return 0;
}
