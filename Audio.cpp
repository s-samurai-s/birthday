#include "Audio.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio/miniaudio.h>

namespace bd {

// ============================================================ synth helpers
// These helpers build sounds directly into a small floating-point buffer. Keeping the
// synthesis here means the game can create its own effects and music without shipping a
// separate audio file for every little sound.
namespace {
const int SR = 22050;

struct Buf {
    std::vector<float> d;
    bool loop;
    Buf(float secs, bool lp = false) : d((size_t)(secs * SR), 0.f), loop(lp) {}
    void add(size_t i, float v) {
        if (loop)
            i %= d.size();
        else if (i >= d.size())
            return;
        d[i] += v;
    }
    size_t at(float t) const {
        return (size_t)std::max(0.f, t * SR);
    }
};

float mf(float m) {
    return 440.f * std::pow(2.f, (m - 69.f) / 12.f);
}

struct Timbre {
    int n;
    float ratio[4], amp[4], decay[4];
    float attack, release, vibHz, vibDepth;
};
const Timbre BELL = {
    4, {1, 2.01f, 2.76f, 5.4f}, {1, 0.45f, 0.22f, 0.1f}, {2.2f, 3.5f, 5.f, 8.f}, 0.003f, 0.05f, 0,
    0};
const Timbre CELESTA = {3, {1, 2.0f, 4.0f}, {1, 0.35f, 0.12f}, {3.2f, 5.f, 8.f}, 0.003f, 0.05f, 0,
                        0};
const Timbre PLUCK = {3, {1, 2, 3}, {1, 0.4f, 0.18f}, {4.5f, 7.f, 10.f}, 0.004f, 0.05f, 0, 0};
const Timbre HARP = {
    4, {1, 2, 3, 4}, {1, 0.5f, 0.25f, 0.1f}, {2.8f, 4.f, 6.f, 9.f}, 0.004f, 0.08f, 0, 0};
const Timbre PAD = {
    3, {1, 1.005f, 2.0f}, {0.6f, 0.6f, 0.15f}, {0.15f, 0.15f, 0.3f}, 0.5f, 0.8f, 0.2f, 0.002f};
const Timbre FLUTE = {2, {1, 2.0f}, {1, 0.12f}, {0.5f, 1.f}, 0.07f, 0.12f, 5.2f, 0.006f};
const Timbre SOFTBASS = {2, {1, 2}, {1, 0.2f}, {1.2f, 3.f}, 0.01f, 0.15f, 0, 0};

void note(Buf &b, float t, float f, float dur, float vol, const Timbre &T) {
    float total = dur + T.release;
    size_t n0 = b.at(t), n = (size_t)(total * SR);
    for (size_t k = 0; k < n; k++) {
        float tt = (float)k / SR;
        float env = std::min(1.f, tt / T.attack);
        if (tt > dur)
            env *= std::max(0.f, 1.f - (tt - dur) / T.release);
        if (env <= 0)
            continue;
        float vib = T.vibHz > 0 ? std::sin(2 * PI * T.vibHz * tt) * T.vibDepth : 0.f;
        float s = 0;
        for (int j = 0; j < T.n; j++)
            s += T.amp[j] * std::exp(-T.decay[j] * tt) *
                 std::sin(2 * PI * f * T.ratio[j] * (tt + vib * tt * 0 + vib));
        b.add(n0 + k, s * env * vol);
    }
}
// simple one-pole lowpassed noise burst with exponential decay
void noiseBurst(Buf &b, float t, float dur, float vol, float lp, Rng &rng, float decay = 6.f) {
    size_t n0 = b.at(t), n = (size_t)(dur * SR);
    float y = 0;
    for (size_t k = 0; k < n; k++) {
        float tt = (float)k / SR;
        y += (rng.range(-1, 1) - y) * lp;
        b.add(n0 + k,
              y * vol * std::exp(-decay * tt / std::max(dur, 0.01f)) * std::min(1.f, tt * 400.f));
    }
}
void sweep(Buf &b, float t, float f0, float f1, float dur, float vol, bool saw = false,
           float decay = 3.f) {
    size_t n0 = b.at(t), n = (size_t)(dur * SR);
    double ph = 0;
    for (size_t k = 0; k < n; k++) {
        float u = (float)k / n, f = f0 + (f1 - f0) * u;
        ph += 2 * PI * f / SR;
        float s = saw ? (float)(std::fmod(ph / (2 * PI), 1.0) * 2 - 1) * 0.5f : std::sin((float)ph);
        b.add(n0 + k, s * vol * std::sin(PI * u) * std::exp(-decay * 0.2f * u));
    }
}
void echo(Buf &b, float d1, float fb1, float d2, float fb2) {
    size_t N = b.d.size();
    std::vector<float> base = b.d;
    size_t a = (size_t)(d1 * SR), c = (size_t)(d2 * SR);
    for (int pass = 0; pass < 3; pass++)
        for (size_t i = 0; i < N; i++)
            b.d[i] = base[i] + fb1 * b.d[(i + N - a % N) % N] + fb2 * b.d[(i + N - c % N) % N];
}
void lowpass(Buf &b, float k) {
    float y = 0;
    for (float &s : b.d) {
        y += (s - y) * k;
        s = y;
    }
}
Sample finish(Buf &b, float peakTarget = 0.8f) {
    float pk = 0.0001f;
    for (float s : b.d)
        pk = std::max(pk, std::abs(s));
    float g = peakTarget / pk;
    Sample s;
    s.channels = 1;
    s.rate = SR;
    s.data = std::move(b.d);
    for (float &v : s.data)
        v *= g;
    return s;
}
Sample finishFixed(Buf &b, float gain) {
    Sample s;
    s.channels = 1;
    s.rate = SR;
    s.data = std::move(b.d);
    for (float &v : s.data)
        v = std::max(-1.f, std::min(1.f, v * gain));
    return s;
}

// ------------------------------------------------------------ music
// The music is assembled from notes, chords and a few instrument-like sound profiles.
// A TrackSpec describes the musical choices; makeTrack turns those choices into samples.
// composition
struct ChordDef {
    int root;
    int type;
}; // type 0 maj, 1 min, 2 maj7, 3 min7, 4 dom7
void chordTones(const ChordDef &c, int out[4]) {
    static const int iv[5][4] = {
        {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 4, 7, 10}};
    for (int i = 0; i < 4; i++)
        out[i] = c.root + iv[c.type][i];
}

struct TrackSpec {
    float bpm;
    int beats;
    std::vector<ChordDef> prog;
    int reps;
    const Timbre *arp;
    const Timbre *mel;
    int arpStyle; // 0 eighth up/down, 1 waltz, 2 slow quarters
    float arpVol, melVol, padVol, bassVol, melDensity;
    int arpOct;
    std::vector<int> scale;
    uint32_t seed;
    float echo1, echo2;
};

Sample makeTrack(const TrackSpec &S) {
    float beat = 60.f / S.bpm;
    int bars = (int)S.prog.size() * S.reps;
    float total = bars * S.beats * beat;
    Buf b(total, true);
    Rng rng(S.seed);
    int last = (int)S.scale.size() / 2;
    for (int bar = 0; bar < bars; bar++) {
        const ChordDef &c = S.prog[bar % S.prog.size()];
        int ct[4];
        chordTones(c, ct);
        float t0 = bar * S.beats * beat;
        float barDur = S.beats * beat;
        for (int i = 0; i < 3; i++)
            note(b, t0, mf((float)ct[i] + 12), barDur, S.padVol * 0.5f, PAD);
        if (S.bassVol > 0) {
            note(b, t0, mf((float)ct[0] - 12), beat * 1.6f, S.bassVol, SOFTBASS);
            if (S.beats == 4)
                note(b, t0 + beat * 2, mf((float)ct[2] - 12), beat * 1.2f, S.bassVol * 0.7f,
                     SOFTBASS);
        }
        if (S.arpStyle == 0) {
            static const int pat[8] = {0, 1, 2, 3, 2, 1, 2, 1};
            for (int i = 0; i < S.beats * 2; i++) {
                int k = pat[i % 8];
                note(b, t0 + i * beat * 0.5f, mf((float)(ct[k] + 12 * S.arpOct)), beat * 1.2f,
                     S.arpVol * (i % 2 ? 0.75f : 1.f), *S.arp);
            }
        } else if (S.arpStyle == 1) { // waltz: bass then two chord stabs
            note(b, t0, mf((float)ct[0] - 12 + 12 * S.arpOct), beat * 0.9f, S.arpVol, *S.arp);
            for (int bt = 1; bt < 3; bt++)
                for (int i = 0; i < 3; i++)
                    note(b, t0 + bt * beat + i * 0.012f, mf((float)(ct[i] + 12 * S.arpOct)),
                         beat * 0.6f, S.arpVol * 0.55f, *S.arp);
        } else { // slow quarters
            static const int pat[4] = {0, 2, 1, 3};
            for (int i = 0; i < S.beats; i++)
                note(b, t0 + i * beat, mf((float)(ct[pat[i % 4]] + 12 * S.arpOct)), beat * 2.f,
                     S.arpVol, *S.arp);
        }
        if (S.mel && rng.f() < S.melDensity) {
            int notes = rng.i(1, 3);
            float tt = t0 + beat * (float)rng.i(0, 1);
            for (int i = 0; i < notes; i++) {
                last = std::max(0, std::min((int)S.scale.size() - 1, last + rng.i(-2, 2)));
                float dur = beat * (rng.chance(0.5f) ? 1.f : 2.f);
                note(b, tt, mf((float)S.scale[last]), dur, S.melVol, *S.mel);
                tt += dur;
                if (tt > t0 + barDur)
                    break;
            }
        }
    }
    echo(b, 0.31f, 0.28f, 0.47f, 0.2f);
    return finish(b, 0.6f);
}

Sample makeBirthday() {
    const float bpm = 108.f, beat = 60.f / bpm;
    struct N {
        float m;
        float d;
    };
    const std::vector<std::vector<N>> bars = {
        {{67, .75f}, {67, .25f}, {69, 1}, {67, 1}}, {{72, 1}, {71, 2}},
        {{67, .75f}, {67, .25f}, {69, 1}, {67, 1}}, {{74, 1}, {72, 2}},
        {{67, .75f}, {67, .25f}, {79, 1}, {76, 1}}, {{72, 1}, {71, 1}, {69, 1}},
        {{77, .75f}, {77, .25f}, {76, 1}, {72, 1}}, {{74, 1}, {72, 2}}};
    const ChordDef ch[8] = {{48, 0}, {43, 4}, {43, 4}, {48, 0}, {48, 0}, {41, 0}, {41, 0}, {48, 0}};
    const int reps = 3;
    Buf b(8 * 3 * beat * reps, true);
    for (int rep = 0; rep < reps; rep++)
        for (int bar = 0; bar < 8; bar++) {
            float t0 = (rep * 8 + bar) * 3 * beat;
            int ct[4];
            chordTones(ch[bar], ct);
            for (int i = 0; i < 3; i++)
                note(b, t0, mf((float)ct[i] + 12), 3 * beat, 0.07f + 0.02f * rep, PAD);
            note(b, t0, mf((float)ct[0] - 12), 1.5f * beat, 0.16f, SOFTBASS);
            // arpeggio (harp), richer on later repeats
            for (int i = 0; i < 6; i++)
                note(b, t0 + i * beat * 0.5f, mf((float)(ct[(i % 3)] + 12)), beat * 1.2f, 0.07f,
                     HARP);
            if (rep >= 1)
                for (int bt = 1; bt < 3; bt++)
                    note(b, t0 + bt * beat, mf((float)(ct[0] + 24)), beat * 0.6f, 0.05f, BELL);
            float t = t0;
            for (const N &n : bars[bar]) {
                float m = n.m + (rep == 2 ? 0.f : 0.f);
                const Timbre &T = rep == 0 ? CELESTA : (rep == 1 ? BELL : FLUTE);
                note(b, t, mf(m), n.d * beat * 0.98f, rep == 2 ? 0.22f : 0.26f, T);
                if (rep == 2)
                    note(b, t, mf(m + 12), n.d * beat * 0.9f, 0.06f, CELESTA);
                t += n.d * beat;
            }
        }
    echo(b, 0.28f, 0.25f, 0.43f, 0.18f);
    return finish(b, 0.65f);
}

// ------------------------------------------------------------ ambience loops
// Ambient tracks are deliberately generated as loops, so the transition from the end
// back to the beginning stays seamless while the player explores an area.
// (10 s)
Sample makeAmbience(int kind) {
    Buf b(10.f, true);
    Rng rng(500 + kind);
    size_t N = b.d.size();
    float y1 = 0, y2 = 0;
    for (size_t i = 0; i < N; i++) {
        float t = (float)i / SR;
        float n = rng.range(-1, 1);
        float k = 0;
        switch (kind) {
        case 0: {
            y1 += (n - y1) * 0.04f;
            float gust = 0.5f + 0.5f * std::sin(2 * PI * t / 10.f * 1.f + 1.f);
            k = y1 * (0.35f + 0.65f * gust) * 0.9f;
            break;
        } // forest wind
        case 1: {
            y1 += (n - y1) * 0.06f;
            y2 += (y1 - y2) * 0.2f;
            float sw = 0.5f + 0.5f * std::sin(2 * PI * t / 5.f);
            k = y2 * (0.4f + 0.6f * sw) * 1.2f;
            break;
        } // lake lapping
        case 2: {
            y1 += (n - y1) * 0.03f;
            k = y1 * 0.5f * (0.6f + 0.4f * std::sin(2 * PI * t / 10.f * 2));
            break;
        } // garden breeze
        case 3: {
            k = (std::sin(2 * PI * 55.f * t) * 0.3f + std::sin(2 * PI * 82.4f * t) * 0.18f) *
                (0.7f + 0.3f * std::sin(2 * PI * t / 10.f * 3));
            y1 += (n - y1) * 0.02f;
            k += y1 * 0.15f;
            break;
        } // castle hum
        default: {
            y1 += (n - y1) * 0.035f;
            k = y1 * 0.45f;
            break;
        }
        }
        b.d[i] = k * 0.5f;
    }
    // layered details
    if (kind == 0 || kind == 2 || kind == 4) { // crickets / tiny chirps
        for (int c = 0; c < (kind == 0 ? 28 : 12); c++) {
            float t = rng.range(0.f, 9.5f),
                  f = kind == 2 ? rng.range(3000, 4200) : rng.range(4000, 5200);
            for (int p = 0; p < 3; p++)
                note(b, t + p * 0.09f, f, 0.04f, kind == 0 ? 0.03f : 0.02f, CELESTA);
        }
    }
    if (kind == 1)
        for (int c = 0; c < 9; c++) {
            float t = rng.range(0.f, 9.5f), f = rng.range(700, 1300);
            sweep(b, t, f, f * 1.7f, 0.12f, 0.05f);
        }
    if (kind == 2 || kind == 4)
        for (int c = 0; c < 6; c++)
            note(b, rng.range(0.f, 9.f), mf((float)rng.i(84, 96)), 0.6f, 0.03f, BELL);
    return finish(b, 0.5f);
}
} // namespace

// ============================================================ sample bank
// The sample bank is the place the rest of the game asks for sounds by name. Built-in
// samples are always available, but a matching file in assets/audio can replace one.
void AudioManager::put(const std::string &name, Sample &&s) {
    // user override from assets/audio
    for (const char *ext : {".wav", ".mp3", ".flac", ".WAV", ".MP3", ".FLAC"}) {
        std::string path = Paths::file("assets/audio/" + name + ext);
        if (!util::fileExists(path))
            continue;
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100);
        ma_uint64 frames = 0;
        void *pData = nullptr;
        if (ma_decode_file(path.c_str(), &cfg, &frames, &pData) == MA_SUCCESS && pData &&
            frames > 0) {
            Sample o;
            o.channels = 2;
            o.rate = 44100;
            o.data.assign((float *)pData, (float *)pData + frames * 2);
            ma_free(pData, nullptr);
            s = std::move(o);
            std::lock_guard<std::mutex> lk(mu_);
            overrides_.push_back(name);
            Log::info("audio override: %s", path.c_str());
        } else
            Log::warn("Could not decode audio file %s - using built-in sound", path.c_str());
        break;
    }
    std::lock_guard<std::mutex> lk(mu_);
    bank_[name] = std::make_shared<Sample>(std::move(s));
}
bool AudioManager::overrideLoaded(const std::string &name) const {
    for (auto &o : overrides_)
        if (o == name)
            return true;
    return false;
}
std::shared_ptr<Sample> AudioManager::get(const std::string &name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = bank_.find(name);
    return it == bank_.end() ? nullptr : it->second;
}
bool AudioManager::hasSample(const std::string &name) {
    return get(name) != nullptr;
}

// Most short effects are made from the same few building blocks: a little noise,
// a pitched note, or a frequency sweep. Keeping them together also makes it easy
// to adjust the overall sound style without hunting through gameplay code.
void AudioManager::buildSfx() {
    Rng rng(4242);
    auto mk = [&](const std::string &name, float len, auto fn, float peak = 0.7f) {
        Buf b(len);
        fn(b);
        put(name, finish(b, peak));
    };
    mk(
        "step_grass", 0.12f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.09f, 0.8f, 0.22f, rng, 5);
            note(b, 0, 95, 0.05f, 0.25f, SOFTBASS);
        },
        0.35f);
    mk(
        "step_stone", 0.12f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.05f, 0.9f, 0.5f, rng, 7);
            note(b, 0, 180, 0.04f, 0.3f, PLUCK);
        },
        0.35f);
    mk(
        "step_wood", 0.14f,
        [&](Buf &b) {
            note(b, 0, 150, 0.08f, 0.8f, PLUCK);
            noiseBurst(b, 0, 0.04f, 0.3f, 0.4f, rng, 7);
        },
        0.35f);
    mk("step_sand", 0.14f, [&](Buf &b) { noiseBurst(b, 0, 0.12f, 0.8f, 0.12f, rng, 4); }, 0.3f);
    mk(
        "step_water", 0.25f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.2f, 0.8f, 0.3f, rng, 4);
            sweep(b, 0, 500, 900, 0.1f, 0.15f);
        },
        0.3f);
    mk("ui_move", 0.1f, [&](Buf &b) { note(b, 0, 880, 0.04f, 0.6f, PLUCK); }, 0.3f);
    mk(
        "ui_click", 0.3f,
        [&](Buf &b) {
            note(b, 0, 1320, 0.1f, 0.6f, BELL);
            note(b, 0.05f, 1760, 0.12f, 0.4f, BELL);
        },
        0.45f);
    mk(
        "ui_back", 0.2f,
        [&](Buf &b) {
            note(b, 0, 660, 0.06f, 0.6f, PLUCK);
            note(b, 0.06f, 440, 0.08f, 0.6f, PLUCK);
        },
        0.4f);
    mk(
        "chest_open", 2.6f,
        [&](Buf &b) {
            sweep(b, 0.0f, 90, 160, 0.4f, 0.35f, true);
            noiseBurst(b, 0.0f, 0.4f, 0.15f, 0.15f, rng, 3);
            noiseBurst(b, 0.38f, 0.12f, 0.5f, 0.3f, rng, 6);
            const float ms[] = {72, 76, 79, 84, 88, 91};
            for (int i = 0; i < 6; i++)
                note(b, 0.45f + i * 0.11f, mf(ms[i]), 1.2f, 0.4f, BELL);
            for (int i = 0; i < 14; i++)
                note(b, 0.6f + rng.range(0, 1.4f), mf((float)rng.i(90, 100)), 0.3f, 0.08f, CELESTA);
        },
        0.75f);
    mk(
        "chest_locked", 0.5f,
        [&](Buf &b) {
            note(b, 0, 110, 0.12f, 0.9f, SOFTBASS);
            note(b, 0.18f, 100, 0.14f, 0.9f, SOFTBASS);
            noiseBurst(b, 0, 0.05f, 0.4f, 0.3f, rng, 7);
            noiseBurst(b, 0.18f, 0.05f, 0.4f, 0.3f, rng, 7);
        },
        0.6f);
    mk(
        "collect", 0.7f,
        [&](Buf &b) {
            note(b, 0, mf(88), 0.3f, 0.6f, BELL);
            note(b, 0.09f, mf(93), 0.5f, 0.5f, BELL);
        },
        0.5f);
    mk(
        "key", 1.2f,
        [&](Buf &b) {
            const float m[] = {79, 84, 88, 91};
            for (int i = 0; i < 4; i++)
                note(b, i * 0.1f, mf(m[i]), 0.6f, 0.5f, BELL);
            for (int i = 0; i < 10; i++)
                noiseBurst(b, 0.05f * i, 0.03f, 0.08f, 0.8f, rng, 5);
        },
        0.6f);
    mk(
        "shard", 1.4f,
        [&](Buf &b) {
            const float m[] = {84, 88, 91, 96, 100};
            for (int i = 0; i < 5; i++)
                note(b, i * 0.08f, mf(m[i]), 0.9f, 0.45f, CELESTA);
        },
        0.6f);
    mk(
        "door_unlock", 0.8f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.03f, 0.8f, 0.7f, rng, 6);
            noiseBurst(b, 0.12f, 0.04f, 0.8f, 0.6f, rng, 6);
            note(b, 0.2f, mf(84), 0.4f, 0.4f, BELL);
            note(b, 0.3f, mf(91), 0.5f, 0.4f, BELL);
        },
        0.6f);
    mk(
        "door_open", 0.9f,
        [&](Buf &b) {
            sweep(b, 0, 200, 90, 0.7f, 0.4f, true);
            noiseBurst(b, 0.55f, 0.15f, 0.5f, 0.15f, rng, 4);
        },
        0.55f);
    mk(
        "lever", 0.5f,
        [&](Buf &b) {
            note(b, 0, 70, 0.2f, 1.f, SOFTBASS);
            noiseBurst(b, 0, 0.04f, 0.6f, 0.4f, rng, 6);
            noiseBurst(b, 0.2f, 0.05f, 0.4f, 0.3f, rng, 6);
        },
        0.65f);
    mk(
        "bridge", 2.0f,
        [&](Buf &b) {
            for (int i = 0; i < 9; i++) {
                note(b, i * 0.18f, 110 + rng.range(0, 40), 0.08f, 0.7f, PLUCK);
                noiseBurst(b, i * 0.18f, 0.06f, 0.3f, 0.3f, rng, 6);
            }
            for (int i = 0; i < 4; i++)
                note(b, 1.2f + i * 0.15f, mf(76 + i * 4.f), 0.7f, 0.3f, BELL);
        },
        0.6f);
    mk(
        "secret", 1.4f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.4f, 0.4f, 0.1f, rng, 2);
            const float m[] = {88, 91, 95, 100};
            for (int i = 0; i < 4; i++)
                note(b, 0.15f + i * 0.09f, mf(m[i]), 0.9f, 0.35f, CELESTA);
        },
        0.5f);
    const float fl[4] = {72, 76, 79, 81};
    for (int i = 0; i < 4; i++)
        mk(
            "flower_" + std::to_string(i), 0.9f,
            [&](Buf &b) {
                note(b, 0, mf(fl[i]), 0.5f, 0.7f, BELL);
                note(b, 0, mf(fl[i] + 12), 0.5f, 0.25f, CELESTA);
            },
            0.5f);
    mk(
        "flower_good", 2.0f,
        [&](Buf &b) {
            const float m[] = {72, 76, 79, 84, 88, 91, 96};
            for (int i = 0; i < 7; i++)
                note(b, i * 0.09f, mf(m[i]), 1.2f, 0.4f, BELL);
        },
        0.65f);
    mk(
        "flower_bad", 0.6f,
        [&](Buf &b) {
            sweep(b, 0, 260, 130, 0.45f, 0.5f, false);
            note(b, 0, 130, 0.3f, 0.3f, SOFTBASS);
        },
        0.45f);
    mk(
        "paper", 0.6f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.5f, 0.6f, 0.3f, rng, 2.5f);
            noiseBurst(b, 0.15f, 0.2f, 0.3f, 0.5f, rng, 4);
        },
        0.3f);
    mk(
        "type", 0.05f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.015f, 0.8f, 0.8f, rng, 4);
            note(b, 0, 1800 + rng.range(0, 400), 0.01f, 0.3f, PLUCK);
        },
        0.18f);
    mk(
        "talk", 0.08f, [&](Buf &b) { note(b, 0, 380 + rng.range(0, 180), 0.04f, 0.8f, PLUCK); },
        0.2f);
    mk(
        "fw_launch", 1.0f,
        [&](Buf &b) {
            sweep(b, 0, 380, 1700, 0.9f, 0.4f);
            noiseBurst(b, 0, 0.9f, 0.1f, 0.4f, rng, 1.5f);
        },
        0.3f);
    mk(
        "fw_boom", 1.8f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.7f, 1.f, 0.12f, rng, 3);
            note(b, 0, 55, 0.5f, 0.9f, SOFTBASS);
            for (int i = 0; i < 40; i++)
                noiseBurst(b, 0.2f + rng.range(0, 1.3f), 0.012f, 0.4f, 0.9f, rng, 4);
        },
        0.6f);
    mk(
        "achievement", 1.2f,
        [&](Buf &b) {
            const float m[] = {84, 88, 91, 96};
            for (int i = 0; i < 4; i++)
                note(b, i * 0.1f, mf(m[i]), 0.8f, 0.5f, PLUCK);
            note(b, 0.4f, mf(103), 0.8f, 0.3f, BELL);
        },
        0.55f);
    mk(
        "magic", 1.5f,
        [&](Buf &b) {
            for (int i = 0; i < 8; i++)
                note(b, i * 0.08f, mf(100.f - i * 2.f), 0.6f, 0.3f, CELESTA);
        },
        0.45f);
    mk(
        "sit", 0.4f,
        [&](Buf &b) {
            noiseBurst(b, 0, 0.2f, 0.7f, 0.1f, rng, 4);
            note(b, 0, 85, 0.15f, 0.6f, SOFTBASS);
        },
        0.45f);
    mk("pop", 0.15f, [&](Buf &b) { sweep(b, 0, 300, 900, 0.08f, 0.8f); }, 0.35f);
}

// Music generation can take a moment because the tracks are synthesized sample by
// sample. It runs on a worker thread so the game does not freeze during startup.
void AudioManager::buildMusicAsync() {
    worker_ = std::thread([this]() {
        auto done = [&](const char *n, Sample &&s) {
            if (!quit_)
                put(n, std::move(s));
        };
        {
            TrackSpec s{72,
                        4,
                        {{48, 2}, {45, 3}, {41, 2}, {43, 0}},
                        2,
                        &BELL,
                        &FLUTE,
                        0,
                        0.09f,
                        0.16f,
                        0.1f,
                        0.12f,
                        0.6f,
                        1,
                        {60, 62, 64, 67, 69, 72, 74, 76},
                        1,
                        0,
                        0};
            done("music_menu", makeTrack(s));
        }
        {
            TrackSpec s{80,
                        4,
                        {{45, 1}, {41, 0}, {48, 0}, {43, 0}},
                        2,
                        &HARP,
                        &FLUTE,
                        0,
                        0.1f,
                        0.2f,
                        0.09f,
                        0.14f,
                        0.55f,
                        1,
                        {57, 60, 62, 64, 67, 69, 72, 74},
                        2,
                        0,
                        0};
            done("music_forest", makeTrack(s));
        }
        {
            TrackSpec s{64,
                        4,
                        {{41, 2}, {40, 3}, {38, 3}, {48, 2}},
                        2,
                        &BELL,
                        &CELESTA,
                        2,
                        0.11f,
                        0.14f,
                        0.12f,
                        0.1f,
                        0.35f,
                        1,
                        {65, 67, 69, 72, 74, 77, 79},
                        3,
                        0,
                        0};
            done("music_lake", makeTrack(s));
        }
        {
            TrackSpec s{70,
                        4,
                        {{40, 1}, {48, 0}, {43, 0}, {38, 0}},
                        2,
                        &CELESTA,
                        &CELESTA,
                        0,
                        0.1f,
                        0.15f,
                        0.1f,
                        0.1f,
                        0.5f,
                        1,
                        {64, 67, 69, 71, 74, 76, 79},
                        4,
                        0,
                        0};
            done("music_garden", makeTrack(s));
        }
        {
            TrackSpec s{96,
                        3,
                        {{50, 1}, {46, 0}, {48, 0}, {45, 4}},
                        3,
                        &PLUCK,
                        &FLUTE,
                        1,
                        0.12f,
                        0.16f,
                        0.07f,
                        0.0f,
                        0.75f,
                        1,
                        {62, 65, 67, 69, 72, 74, 77},
                        5,
                        0,
                        0};
            done("music_castle", makeTrack(s));
        }
        {
            TrackSpec s{84,
                        4,
                        {{48, 0}, {43, 0}, {45, 1}, {41, 0}, {48, 0}, {43, 0}, {41, 0}, {48, 0}},
                        1,
                        &BELL,
                        &CELESTA,
                        0,
                        0.09f,
                        0.15f,
                        0.1f,
                        0.12f,
                        0.6f,
                        1,
                        {67, 69, 72, 74, 76, 79, 81},
                        6,
                        0,
                        0};
            done("music_final", makeTrack(s));
        }
        done("music_birthday", makeBirthday());
        for (int k = 0; k < 5; k++) {
            const char *n[] = {"amb_forest", "amb_lake", "amb_garden", "amb_castle", "amb_final"};
            done(n[k], makeAmbience(k));
        }
        bankReady_ = true;
        Log::info("audio: music bank ready");
    });
}

// ============================================================ device + mixer
// This section connects the sample bank to the actual audio device. The callback mixes
// active voices into the output buffer, while the game thread only requests sounds or
// changes which music and ambience tracks should be playing.
static void dataCallback(ma_device *dev, void *out, const void *, ma_uint32 frames) {
    static_cast<AudioManager *>(dev->pUserData)->mix((float *)out, (int)frames);
}

bool AudioManager::init() {
    voices_.reserve(64);
    buildSfx();
    buildMusicAsync();
    ma_device *d = new ma_device;
    ma_device_config c = ma_device_config_init(ma_device_type_playback);
    c.playback.format = ma_format_f32;
    c.playback.channels = 2;
    c.sampleRate = 44100;
    c.dataCallback = dataCallback;
    c.pUserData = this;
    if (ma_device_init(nullptr, &c, d) != MA_SUCCESS) {
        delete d;
        Log::warn("No audio output device available - the game will run silently");
        return false;
    }
    deviceRate_ = (int)d->sampleRate;
    if (ma_device_start(d) != MA_SUCCESS) {
        ma_device_uninit(d);
        delete d;
        Log::warn("Audio device failed to start");
        return false;
    }
    device_ = d;
    return true;
}
void AudioManager::shutdown() {
    quit_ = true;
    if (worker_.joinable())
        worker_.join();
    if (device_) {
        ma_device *d = (ma_device *)device_;
        ma_device_uninit(d);
        delete d;
        device_ = nullptr;
    }
}

void AudioManager::sfx(const std::string &name, float vol, float pitch) {
    auto s = get(name);
    if (!s)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    Voice *v = nullptr;
    for (auto &x : voices_)
        if (!x.active) {
            v = &x;
            break;
        }
    if (!v) {
        if (voices_.size() >= 48)
            return;
        voices_.emplace_back();
        v = &voices_.back();
    }
    *v = Voice();
    v->s = s;
    v->vol = v->target = vol;
    v->pitch = pitch;
    v->active = true;
    v->cat = Cat::Sfx;
}
void AudioManager::music(const std::string &name, float fade) {
    wantMusic_ = name;
    wantMusicFade_ = fade;
}
void AudioManager::ambience(const std::string &name, float fade) {
    wantAmb_ = name;
    wantAmbFade_ = fade;
}

void AudioManager::startLoop(Cat cat, const std::string &name, float fade) {
    std::shared_ptr<Sample> s = name.empty() ? nullptr : get(name);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto &v : voices_)
        if (v.active && v.cat == cat && v.target > 0) {
            v.target = 0;
            v.fadeRate = 1.f / std::max(0.05f, fade);
            v.stopWhenSilent = true;
        }
    if (!s)
        return;
    Voice *v = nullptr;
    for (auto &x : voices_)
        if (!x.active) {
            v = &x;
            break;
        }
    if (!v) {
        voices_.emplace_back();
        v = &voices_.back();
    }
    *v = Voice();
    v->s = s;
    v->vol = 0;
    v->target = 1;
    v->fadeRate = 1.f / std::max(0.05f, fade);
    v->loop = true;
    v->active = true;
    v->cat = cat;
}
void AudioManager::update() {
    if (wantMusic_ != playingMusic_) {
        if (wantMusic_.empty() || hasSample(wantMusic_)) {
            startLoop(Cat::Music, wantMusic_, wantMusicFade_);
            playingMusic_ = wantMusic_;
        }
    }
    if (wantAmb_ != playingAmb_) {
        if (wantAmb_.empty() || hasSample(wantAmb_)) {
            startLoop(Cat::Amb, wantAmb_, wantAmbFade_);
            playingAmb_ = wantAmb_;
        }
    }
}

// Mix every active voice into the buffer requested by miniaudio. This function runs
// on the audio callback thread, so it keeps its work predictable and avoids loading
// files or creating samples while the device is asking for the next batch of audio.
void AudioManager::mix(float *out, int frames) {
    std::fill(out, out + (size_t)frames * 2, 0.f);
    std::lock_guard<std::mutex> lk(mu_);
    duck_ += (duckTarget_ - duck_) * std::min(1.f, (float)frames / deviceRate_ * 3.f);
    float en = enabled_ ? 1.f : 0.f;
    for (Voice &v : voices_) {
        if (!v.active || !v.s)
            continue;
        const Sample &s = *v.s;
        size_t n = s.frames();
        if (n < 2) {
            v.active = false;
            continue;
        }
        float catVol = v.cat == Cat::Sfx
                           ? sfxVol_
                           : (v.cat == Cat::Music ? musicVol_ * duck_ * 0.8f : sfxVol_ * 0.45f);
        double step = (double)s.rate / deviceRate_ * v.pitch;
        float fadeStep = v.fadeRate / deviceRate_;
        for (int i = 0; i < frames; i++) {
            if (v.vol != v.target) {
                if (v.fadeRate <= 0)
                    v.vol = v.target;
                else if (v.vol < v.target)
                    v.vol = std::min(v.target, v.vol + fadeStep);
                else
                    v.vol = std::max(v.target, v.vol - fadeStep);
            }
            size_t i0 = (size_t)v.pos;
            if (i0 >= n - 1) {
                if (v.loop) {
                    v.pos = std::fmod(v.pos, (double)n);
                    i0 = (size_t)v.pos;
                } else {
                    v.active = false;
                    break;
                }
            }
            float fr = (float)(v.pos - (double)i0);
            size_t i1 = i0 + 1 < n ? i0 + 1 : (v.loop ? 0 : i0);
            float l, r;
            if (s.channels == 1) {
                l = r = s.data[i0] + (s.data[i1] - s.data[i0]) * fr;
            } else {
                l = s.data[i0 * 2] + (s.data[i1 * 2] - s.data[i0 * 2]) * fr;
                r = s.data[i0 * 2 + 1] + (s.data[i1 * 2 + 1] - s.data[i0 * 2 + 1]) * fr;
            }
            float g = v.vol * catVol * en;
            out[i * 2] += l * g;
            out[i * 2 + 1] += r * g;
            v.pos += step;
        }
        if (v.stopWhenSilent && v.vol <= 0.0001f && v.target <= 0)
            v.active = false;
    }
    float pk = 0;
    for (int i = 0; i < frames * 2; i++) {
        out[i] = std::tanh(out[i]);
        pk = std::max(pk, std::abs(out[i]));
    }
    peak_ = std::max(pk, peak_ * 0.98f);
}

} // namespace bd
