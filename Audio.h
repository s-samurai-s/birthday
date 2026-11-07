// Audio.h - procedural audio: synthesised sound effects, music and ambience.
// Every sound is generated in code at start-up, so the game ships without audio
// files. A file named assets/audio/<name>.wav/.mp3/.flac replaces the built-in
// sound with the same name.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Common.h"

namespace bd {

// A block of PCM audio. Mono samples store one float per frame; stereo samples
// store interleaved left/right floats.
struct Sample {
    std::vector<float> data;
    int channels = 1;
    int rate = 22050;

    size_t frames() const {
        return channels > 0 ? data.size() / (size_t)channels : 0;
    }
};

// Which volume group a playing voice belongs to.
enum class Cat { Sfx, Music, Amb };

// One currently playing sound. Voices are mixed on the audio thread.
struct Voice {
    std::shared_ptr<Sample> s;
    double pos = 0;       // playback position in source frames
    float vol = 1.f;      // current volume (moves towards target)
    float target = 1.f;   // volume the fade is heading for
    float fadeRate = 0.f; // volume change per second (0 = jump straight to target)
    float pitch = 1.f;    // playback speed multiplier
    bool loop = false;
    bool active = false;
    bool stopWhenSilent = false; // free the voice once a fade-out reaches zero
    Cat cat = Cat::Sfx;
};

// Owns the audio device, the sample bank and the mixer. The game thread asks for
// sounds by name; the device callback mixes whatever is currently playing.
class AudioManager {
  public:
    // Build the effects, start music generation in the background and open the
    // output device. Returns false if no device is available. The game can still
    // run silently in that case.
    bool init();
    void shutdown();

    // Call once per frame. Applies pending music/ambience changes.
    void update();

    // Play a one-shot effect by name.
    void sfx(const std::string &name, float vol = 1.f, float pitch = 1.f);
    // Crossfade to a looping track. An empty name fades the track out.
    void music(const std::string &name, float fadeSeconds = 1.5f);
    void ambience(const std::string &name, float fadeSeconds = 1.5f);

    // ---- settings
    void setEnabled(bool on) {
        enabled_ = on;
    }
    bool enabled() const {
        return enabled_;
    }
    void setSfxVolume(float v) {
        sfxVol_ = clamp01(v);
    }
    void setMusicVolume(float v) {
        musicVol_ = clamp01(v);
    }
    float sfxVolume() const {
        return sfxVol_;
    }
    float musicVolume() const {
        return musicVol_;
    }
    // Lower the music while something important plays (1 = normal, 0 = silent).
    void setDuck(float target) {
        duckTarget_ = clamp01(target);
    }

    // ---- queries
    bool musicReady() const {
        return bankReady_;
    }
    bool hasSample(const std::string &name);
    bool overrideLoaded(const std::string &name) const;
    // Recent output peak (0..1), handy for a simple level meter or visual pulse.
    float peak() const {
        return peak_;
    }

    // Called by the device callback. Public only so the C callback can reach it.
    void mix(float *out, int frames);

  private:
    void put(const std::string &name, Sample &&s);
    std::shared_ptr<Sample> get(const std::string &name);
    void buildSfx();
    void buildMusicAsync();
    void startLoop(Cat cat, const std::string &name, float fade);

    std::mutex mu_; // guards bank_, voices_ and overrides_
    std::unordered_map<std::string, std::shared_ptr<Sample>> bank_;
    std::vector<std::string> overrides_;
    std::vector<Voice> voices_;

    void *device_ = nullptr; // ma_device*, kept opaque so miniaudio stays out of this header
    int deviceRate_ = 44100;

    std::thread worker_; // generates the long music tracks off the main thread
    std::atomic<bool> quit_{false};
    std::atomic<bool> bankReady_{false};

    std::string wantMusic_, playingMusic_;
    std::string wantAmb_, playingAmb_;
    float wantMusicFade_ = 1.5f, wantAmbFade_ = 1.5f;

    // Plain values: the game thread writes, the audio thread reads. A momentarily
    // stale value is harmless for volume settings.
    bool enabled_ = true;
    float sfxVol_ = 0.8f;
    float musicVol_ = 0.7f;
    float duckTarget_ = 1.f;
    float duck_ = 1.f;
    float peak_ = 0.f;
};

} // namespace bd
