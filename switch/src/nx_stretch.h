// MPH-nx: time-stretching for the sound output (WSOLA), from SA-nx's dream::audio::Stretcher
// (same authors). See the comment below; the DS mixer makes 32728 Hz here.
// Time-stretching for the sound output (WSOLA: waveform-similarity overlap-add).
//
// The game makes its sound in step with its own clock. When the guest falls behind
// real time, fewer samples arrive than the device plays, the device's queue runs dry and the gaps
// between blocks are heard as crackles and pops. Stretching makes the audio that did arrive last
// longer instead, at the same pitch: windows of the input are laid one after the other with half
// overlaps, each taken from wherever around its nominal position it best continues the previous
// one, and the input advances by less than the output does. Sound stays continuous; the music's
// tempo follows the game's for the length of a dip.
//
// At a ratio of 1 every window is taken at its nominal position and the overlapping Hann windows
// add back to the input exactly (after the first hop, which fades in), so full speed passes
// through unchanged.
// Header-only, with no SDL, so it is tested without a sound device.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mphnx {

class Stretcher {
public:
    static constexpr std::size_t kHop = 512;         // output frames per step (11.6 ms)
    static constexpr std::size_t kWindow = 2 * kHop;  // window length
    static constexpr std::size_t kSearch = 256;      // how far around nominal a window may move

    Stretcher() {
        for (std::size_t i = 0; i < kWindow; ++i)
            window_[i] = 0.5f - 0.5f * std::cos(6.283185307179586f * static_cast<float>(i) /
                                                static_cast<float>(kWindow));
        tail_.assign(kHop * 2, 0.0f);
    }

    // One stereo frame of input.
    void push(std::int16_t l, std::int16_t r) {
        in_.push_back(static_cast<float>(l));
        in_.push_back(static_cast<float>(r));
    }

    // Emits every output hop the buffered input allows, `ratio` output frames per input frame
    // (1 = unchanged, 2 = twice as long). emit(const std::int16_t* stereo, std::size_t frames).
    template <class Emit>
    void process(double ratio, Emit&& emit) {
        ratio = std::clamp(ratio, 1.0, 4.0);
        for (;;) {
            const std::size_t frames = in_.size() / 2;
            const std::size_t nominal = static_cast<std::size_t>(pos_);
            if (nominal + kSearch + kWindow > frames)
                break;
            std::size_t start = nominal;
            if (have_prev_) {
                const std::size_t target = prev_ + kHop;  // where the last window would continue
                if (ratio > 1.0005 || target != nominal)
                    start = best_match(nominal, target);
            }
            for (std::size_t i = 0; i < kHop; ++i)
                for (std::size_t c = 0; c < 2; ++c) {
                    const float v = tail_[i * 2 + c] + in_[(start + i) * 2 + c] * window_[i];
                    out_[i * 2 + c] = static_cast<std::int16_t>(std::clamp(v, -32768.0f, 32767.0f));
                    tail_[i * 2 + c] = in_[(start + kHop + i) * 2 + c] * window_[kHop + i];
                }
            emit(out_, kHop);
            have_prev_ = true;
            prev_ = start;
            pos_ += static_cast<double>(kHop) / ratio;
            // Drop input nothing can reach any more.
            const std::size_t keep_from = std::min<std::size_t>(
                prev_, static_cast<std::size_t>(pos_) > kSearch ? static_cast<std::size_t>(pos_) - kSearch : 0);
            if (keep_from >= 4096) {
                in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(keep_from * 2));
                prev_ -= keep_from;
                pos_ -= static_cast<double>(keep_from);
            }
        }
    }

    // Frames waiting that no output covers yet (for a caller that wants to flush at the end).
    std::size_t buffered() const { return in_.size() / 2; }
    std::uint64_t moved = 0;  // windows taken away from their nominal position (diagnostics)

private:
    // The start within kSearch of `nominal` whose first hop best matches the hop at `target` (the
    // natural continuation of the last window). Coarse every other frame on a mono mix, then
    // refined one frame either side.
    std::size_t best_match(std::size_t nominal, std::size_t target) {
        const std::size_t lo = nominal > kSearch ? nominal - kSearch : 0;
        const std::size_t hi = nominal + kSearch;
        auto score = [this, target](std::size_t s, std::size_t step) {
            double sum = 0.0;
            for (std::size_t i = 0; i < kHop; i += step) {
                const float a = in_[(s + i) * 2] + in_[(s + i) * 2 + 1];
                const float b = in_[(target + i) * 2] + in_[(target + i) * 2 + 1];
                sum += static_cast<double>(a) * b;
            }
            return sum;
        };
        std::size_t best = nominal;
        double best_score = -1e300;
        for (std::size_t s = lo; s <= hi; s += 2) {
            const double v = score(s, 2);
            if (v > best_score) {
                best_score = v;
                best = s;
            }
        }
        const std::size_t c = best;
        best_score = -1e300;
        for (std::size_t s = c > lo ? c - 1 : c; s <= std::min(c + 1, hi); ++s) {
            const double v = score(s, 1);
            if (v > best_score) {
                best_score = v;
                best = s;
            }
        }
        if (best != nominal)
            ++moved;
        return best;
    }

    std::vector<float> in_;  // stereo, interleaved
    std::vector<float> tail_;
    float window_[kWindow];
    std::int16_t out_[kHop * 2];
    double pos_ = 0.0;
    std::size_t prev_ = 0;
    bool have_prev_ = false;
};

}  // namespace mphnx
