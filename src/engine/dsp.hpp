// Frequency-domain stages (ThVocalFFT, CenterFocus) and the Kaiser FIR filters.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace utagoe {

using cplx = std::complex<double>;

// In-place iterative radix-2 FFT of a fixed power-of-two size.
class FFT {
public:
    explicit FFT(size_t n);
    size_t size() const { return n_; }
    void forward(cplx* x) const { run(x, false); }
    void inverse(cplx* x) const { run(x, true); }  // unnormalised
private:
    void run(cplx* x, bool inv) const;
    size_t n_;
    std::vector<size_t> rev_;
    std::vector<cplx> tw_;
};

// STFT framework shared by both stages (matches the original's streaming):
// N = 8192, hop = N/8, Hann analysis and synthesis windows, one-sided
// spectrum, the first hop samples of every synthesised frame discarded.
// Output sample t corresponds to input sample t - LATENCY.
constexpr size_t FFT_SIZE = 0x2000;
constexpr size_t OVERLAP = 8;
constexpr size_t HOP = FFT_SIZE / OVERLAP;
constexpr size_t LATENCY = FFT_SIZE - HOP;

// ThVocalFFT: per-bin V = O - g*I, zeroing bins the instrumental explains.
// orig/inst/gain are per-sample streams of length len (one channel).
std::vector<double> vocal_fft(const double* orig, const double* inst, const double* gain, size_t len,
                              float kvol, bool quality_priority);

// CenterFocus: keep what sits in the centre of the stereo image.
void center_focus(const double* left, const double* right, size_t len, float strength,
                  std::vector<double>* out_l, std::vector<double>* out_r);

// Kaiser-window FIR (CFIR).  Output is truncated to integers like the
// original; the high-pass output is negated like the original.
class FIRFilter {
public:
    enum Kind { LOWPASS, HIGHPASS };
    FIRFilter(int rate, Kind kind, double freq);
    size_t delay() const { return (h_.size() - 1) / 2; }
    // Filters two channels at once (zero initial state).
    void apply(std::vector<double>* a, std::vector<double>* b) const;
    const std::vector<double>& taps() const { return h_; }
private:
    std::vector<double> h_;
    Kind kind_;
};

}  // namespace utagoe
