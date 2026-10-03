#include "dsp.hpp"

#include <algorithm>
#include <cmath>

namespace utagoe {

namespace {

const double PI = 3.14159265358979323846;
const double OLA_GAIN = (double)5.33333f / OVERLAP;

std::vector<double> hann(size_t n) {
    std::vector<double> w(n);
    for (size_t i = 0; i < n; i++) w[i] = (float)(0.5 - 0.5 * std::cos(2.0 * PI * (double)i / (double)n));
    return w;
}

// pi * (k + 1) / (N / 2), stored as float like the original
float bin_ramp(size_t k, size_t n) {
    double half = (double)(n / 2);
    return (float)(PI / (half / ((double)k + 1.0)));
}

double phase_diff(double a, double b) {
    double d = std::fabs(a - b);
    return d > PI ? 2.0 * PI - d : d;
}

// Sample of the zero-prefixed stream: frame k covers xp[k*hop, k*hop + N) where
// xp = (N - hop zeros) followed by x.
inline double at(const double* x, size_t len, size_t i) {
    return (i < LATENCY || i - LATENCY >= len) ? 0.0 : x[i - LATENCY];
}

// Splits Z = FFT(a + i b) into the spectra of the two real signals (k < N/2).
inline void unpack(const cplx* z, size_t n, size_t k, cplx* a, cplx* b) {
    cplx zk = z[k], zc = std::conj(z[(n - k) % n]);
    *a = (zk + zc) * 0.5;
    *b = (zk - zc) * cplx(0.0, -0.5);
}

// Hermitian spectrum whose real inverse equals Re(ifft(one-sided v)).
inline void hermitian(const cplx* v, size_t n, std::vector<cplx>* h) {
    size_t half = n / 2;
    (*h)[0] = cplx(v[0].real(), 0.0);
    for (size_t k = 1; k < half; k++) {
        cplx y = v[k] * 0.5;
        (*h)[k] = y;
        (*h)[n - k] = std::conj(y);
    }
    (*h)[half] = 0.0;
}

// Adds a synthesised frame (minus its first hop samples) into out.
inline void overlap_add(const double* frame, const std::vector<double>& win, size_t k, std::vector<double>* out) {
    size_t n = win.size();
    size_t len = out->size();
    for (size_t j = HOP; j < n; j++) {
        size_t t = k * HOP + j;
        if (t >= len) break;
        (*out)[t] += frame[j] * win[j] * OLA_GAIN;
    }
}

// Runs the inverse transforms of a list of one-sided spectra two at a time
// (packing two real outputs into one complex IFFT) and overlap-adds them.
class Synth {
public:
    Synth(const FFT& fft, const std::vector<double>& win, std::vector<double>* out)
        : fft_(fft), win_(win), out_(out), h1_(win.size()), h2_(win.size()), z_(win.size()), r_(win.size()) {}
    void push(const cplx* v, size_t frame) {
        if (!pending_) {
            hermitian(v, win_.size(), &h1_);
            k1_ = frame;
            pending_ = true;
        } else {
            hermitian(v, win_.size(), &h2_);
            flush2(frame);
        }
    }
    void finish() {
        if (pending_) {
            size_t n = win_.size();
            for (size_t i = 0; i < n; i++) z_[i] = h1_[i];
            fft_.inverse(z_.data());
            for (size_t i = 0; i < n; i++) r_[i] = z_[i].real() / (double)n;
            overlap_add(r_.data(), win_, k1_, out_);
            pending_ = false;
        }
    }
private:
    void flush2(size_t k2) {
        size_t n = win_.size();
        for (size_t i = 0; i < n; i++) z_[i] = h1_[i] + cplx(0.0, 1.0) * h2_[i];
        fft_.inverse(z_.data());
        for (size_t i = 0; i < n; i++) r_[i] = z_[i].real() / (double)n;
        overlap_add(r_.data(), win_, k1_, out_);
        for (size_t i = 0; i < n; i++) r_[i] = z_[i].imag() / (double)n;
        overlap_add(r_.data(), win_, k2, out_);
        pending_ = false;
    }
    const FFT& fft_;
    const std::vector<double>& win_;
    std::vector<double>* out_;
    std::vector<cplx> h1_, h2_, z_;
    std::vector<double> r_;
    size_t k1_ = 0;
    bool pending_ = false;
};

}  // namespace

// ---------------------------------------------------------------- FFT

FFT::FFT(size_t n) : n_(n), rev_(n), tw_(n / 2) {
    size_t bits = 0;
    while (((size_t)1 << bits) < n) bits++;
    for (size_t i = 0; i < n; i++) {
        size_t r = 0;
        for (size_t b = 0; b < bits; b++) if (i & ((size_t)1 << b)) r |= (size_t)1 << (bits - 1 - b);
        rev_[i] = r;
    }
    for (size_t i = 0; i < n / 2; i++) tw_[i] = std::polar(1.0, -2.0 * PI * (double)i / (double)n);
}

void FFT::run(cplx* x, bool inv) const {
    size_t n = n_;
    for (size_t i = 0; i < n; i++) if (i < rev_[i]) std::swap(x[i], x[rev_[i]]);
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len / 2, step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t j = 0; j < half; j++) {
                cplx w = tw_[j * step];
                if (inv) w = std::conj(w);
                cplx u = x[i + j], v = x[i + j + half] * w;
                x[i + j] = u + v;
                x[i + j + half] = u - v;
            }
        }
    }
}

// ---------------------------------------------------------------- ThVocalFFT

std::vector<double> vocal_fft(const double* orig, const double* inst, const double* gain, size_t len,
                              float kvol, bool quality_priority) {
    const size_t n = FFT_SIZE, half = n / 2;
    std::vector<double> out(len, 0.0);
    FFT fft(n);
    std::vector<double> win = hann(n);
    double cap;
    std::vector<double> thr;
    if (quality_priority) {
        cap = (double)std::min(kvol, 1.5f);
        thr.resize(half);
        for (size_t k = 0; k < half; k++) thr[k] = (double)(std::max(bin_ramp(k, n), 0.15f) * kvol);
    } else {
        cap = (double)kvol;
    }

    size_t nframes = len / HOP;
    std::vector<cplx> z(n), v(half);
    Synth synth(fft, win, &out);
    for (size_t k = 0; k < nframes; k++) {
        size_t base = k * HOP;
        for (size_t i = 0; i < n; i++)
            z[i] = cplx(at(orig, len, base + i) * win[i], at(inst, len, base + i) * win[i]);
        fft.forward(z.data());
        double g = std::min(gain[(k + 1) * HOP - 1], cap);
        for (size_t b = 0; b < half; b++) {
            cplx A, B;
            unpack(z.data(), n, b, &A, &B);
            cplx V = A - g * B;
            bool kill = std::abs(B) * cap > std::abs(A);
            if (kill && quality_priority)
                kill = phase_diff(std::arg(A), std::arg(B)) < thr[b];
            v[b] = kill ? cplx(0.0, 0.0) : V;
        }
        synth.push(v.data(), k);
    }
    synth.finish();
    return out;
}

// ---------------------------------------------------------------- CenterFocus

void center_focus(const double* left, const double* right, size_t len, float strength,
                  std::vector<double>* out_l, std::vector<double>* out_r) {
    const size_t n = FFT_SIZE, half = n / 2;
    out_l->assign(len, 0.0);
    out_r->assign(len, 0.0);
    FFT fft(n);
    std::vector<double> win = hann(n);
    const float s = strength;
    const double pull = (double)std::max(0.4f - 0.1f * s, 0.0f);
    std::vector<double> thr(half);
    for (size_t k = 0; k < half; k++) thr[k] = (double)((bin_ramp(k, n) * 0.5f + 1.0f) / s);

    size_t nframes = len / HOP;
    std::vector<cplx> z(n), vl(half), vr(half);
    Synth synth_l(fft, win, out_l), synth_r(fft, win, out_r);
    for (size_t k = 0; k < nframes; k++) {
        size_t base = k * HOP;
        for (size_t i = 0; i < n; i++)
            z[i] = cplx(at(left, len, base + i) * win[i], at(right, len, base + i) * win[i]);
        fft.forward(z.data());
        for (size_t b = 0; b < half; b++) {
            cplx L, R;
            unpack(z.data(), n, b, &L, &R);
            double ml = std::abs(L), mr = std::abs(R);
            if (!(ml > 0.0 || mr > 0.0)) {
                vl[b] = vr[b] = 0.0;
                continue;
            }
            double pd = phase_diff(std::arg(R), std::arg(L));
            double d = std::fabs(ml - mr);
            if (mr > ml) R *= (d * pull + ml) / mr;   // pull the louder channel toward the quieter one
            else L *= (d * pull + mr) / ml;
            if (pd > thr[b]) {                       // fade out off-centre bins
                double x = (pd - thr[b]) * 2.0 * (double)s;
                double fade = x < PI ? 0.5 * std::cos(x) + 0.5 : 0.0;
                L *= fade;
                R *= fade;
            }
            vl[b] = L;
            vr[b] = R;
        }
        synth_l.push(vl.data(), k);
        synth_r.push(vr.data(), k);
    }
    synth_l.finish();
    synth_r.finish();
}

// ---------------------------------------------------------------- FIR

namespace {

double bessel_i0(double x) {  // 20-term series, as in the original
    double total = 0.0, half = x * 0.5, fact = 1.0;
    for (int k = 1; k <= 20; k++) {
        fact *= k;
        double t = std::pow(half, k) / fact;
        total += t * t;
    }
    return 1.0 + total;
}

}  // namespace

FIRFilter::FIRFilter(int rate, Kind kind, double freq) : kind_(kind) {
    double atten, f_pass, f_stop;
    if (kind == LOWPASS) {
        atten = 80.0, f_pass = freq, f_stop = (float)(freq * 1.2);
    } else {
        atten = 60.0, f_pass = (float)(freq * 0.75), f_stop = freq;
    }
    double nyq = rate * 0.5;
    double trans = (f_stop - f_pass) / nyq * PI;
    int order = (int)((atten - 8.0) / (2.285 * trans) + 1.0);
    if (order % 2) order++;
    order = std::min(order, 1000);
    int half = order / 2;
    double w1 = f_pass / nyq * PI, w2 = f_stop / nyq * PI;
    if (kind == HIGHPASS) w1 = PI - w1, w2 = PI - w2;
    double wc = (w1 + w2) * 0.5;
    double beta = atten > 50.0 ? 0.1102 * (atten - 8.7)
                : atten >= 21.0 ? 0.5842 * std::pow(atten - 21.0, 0.4) + 0.07886 * (atten - 21.0) : 0.0;
    double i0b = bessel_i0(beta);
    h_.resize(order + 1);
    for (int i = 0, n = -half; n <= half; i++, n++) {
        double t = half ? (double)n / half : 0.0;
        double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - t * t))) / i0b;
        h_[i] = (n ? std::sin(n * wc) / (n * PI) : wc / PI) * w;
    }
    if (kind == HIGHPASS)  // modulate to Nyquist; the output is negated again in apply()
        for (size_t i = 0; i < h_.size(); i += 2) h_[i] = -h_[i];
}

void FIRFilter::apply(std::vector<double>* a, std::vector<double>* b) const {
    // Overlap-add FFT convolution; both channels ride in one complex transform.
    const size_t n = FFT_SIZE;
    const size_t taps = h_.size();
    const size_t block = n - taps + 1;
    const size_t len = a->size();
    FFT fft(n);
    std::vector<cplx> hf(n, 0.0);
    for (size_t i = 0; i < taps; i++) hf[i] = h_[i];
    fft.forward(hf.data());
    std::vector<double> ya(len + taps, 0.0), yb(len + taps, 0.0);
    std::vector<cplx> z(n);
    for (size_t start = 0; start < len; start += block) {
        size_t m = std::min(block, len - start);
        for (size_t i = 0; i < n; i++) z[i] = i < m ? cplx((*a)[start + i], (*b)[start + i]) : cplx(0.0, 0.0);
        fft.forward(z.data());
        for (size_t i = 0; i < n; i++) z[i] *= hf[i];
        fft.inverse(z.data());
        for (size_t i = 0; i < m + taps - 1 && start + i < ya.size(); i++) {
            ya[start + i] += z[i].real() / (double)n;
            yb[start + i] += z[i].imag() / (double)n;
        }
    }
    double sign = kind_ == HIGHPASS ? -1.0 : 1.0;
    for (size_t i = 0; i < len; i++) {
        (*a)[i] = sign * std::trunc(ya[i]);
        (*b)[i] = sign * std::trunc(yb[i]);
    }
}

}  // namespace utagoe
