/*
Live power from a cumulative energy counter (1/10 Wh) that the meter reports
once per second of its own uptime.

Why not a plain "delta over the last N frames": the counter is quantised to
0.1 Wh, which is 360 W over one second. A one-frame delta at 5 kW can only show
5040/5400/5760 W; at 100 W the counter moves once every 3.6 s. Accuracy and
latency are traded by the window length, so the window adapts:

 - Only frames where the counter changed ("edges") bound a window. At an edge
   the true energy is known to within min(1 unit, rate * 1 s), so the relative
   error of an edge-to-edge window is about 1 / max(delta units, delta seconds).
   Growing the window until max(units, seconds) >= QUALITY bounds the error by
   ~1/QUALITY at any load: a few seconds at high load, ~QUALITY s at low load.
 - Step detection: a short window (FAST_QUALITY) that disagrees with the long
   one by more than both error bounds means the load really changed. A barrier
   at the start of the short window stops the long window from averaging
   across the step, so the estimate follows within seconds and then smooths
   again as the window regrows from the barrier.
 - Decay: with no edge for g seconds the rate must be below 1 unit / g. The
   estimate is clamped to that bound, which falls quickly when a load switches
   off and never undershoots the true value.

Header-only and free of Arduino dependencies so it runs in the native unit tests.
*/

#pragma once

#include <stdint.h>

class PowerEstimator {
public:
    static const uint8_t  RING = 40;          // edges kept (>= QUALITY s at any load)
    static const uint32_t MAX_WIN_S = 300;    // edges older than this are ignored
    static const uint32_t QUALITY = 30;       // long window: error <~ 1/30
    static const uint32_t FAST_QUALITY = 6;   // short window for step detection

    void reset() {
        _n = 0;
        _have = false;
        _p = 0;
        _win = 0;
        _barrier = 0;
        _steps = 0;
    }

    // Feed one accepted frame: meter uptime in s, counter in 1/10 Wh.
    // Returns false (and changes nothing) if the frame goes backwards;
    // the caller decides whether that is a meter reset and calls reset().
    bool update(uint32_t t, uint64_t c) {
        if (!_have) {
            _have = true;
            _last_t = t;
            _last_c = c;
            _barrier = t;
            _p = 0;
            return true;
        }
        if (t < _last_t || c < _last_c) return false;
        if (t == _last_t) return true;   // duplicate frame
        if (c > _last_c) push(t, (uint32_t)c);
        _last_t = t;
        _last_c = c;
        compute();
        return true;
    }

    uint32_t power_dW() const { return _p; }       // 1/10 W
    uint32_t window_s() const { return _win; }     // length of the window used
    uint32_t steps() const { return _steps; }      // step detections so far

private:
    struct Edge {
        uint32_t t;   // meter uptime (s)
        uint32_t c;   // low 32 bit of the counter; only differences are used
    };

    Edge _e[RING];
    uint8_t _head = 0;   // index of the newest edge
    uint8_t _n = 0;      // edges stored
    bool _have = false;
    uint32_t _last_t = 0;
    uint64_t _last_c = 0;
    uint32_t _barrier = 0;   // windows never start before this time
    uint32_t _p = 0;
    uint32_t _win = 0;
    uint32_t _steps = 0;

    void push(uint32_t t, uint32_t c) {
        _head = (uint8_t)((_head + 1) % RING);
        _e[_head] = {t, c};
        if (_n < RING) _n++;
    }

    const Edge &edge(uint8_t k) const {   // k = 0 is the newest
        return _e[(_head + RING - k) % RING];
    }

    struct Window {
        bool ok;
        uint32_t p;      // 1/10 W
        uint32_t err;    // 1/10 W, quantisation bound
        uint32_t start;  // time of the older edge
        uint32_t len;    // s
    };

    // Shortest window ending at the newest edge with max(units, s) >= q, or
    // the longest one available if none reaches q.
    Window window(uint32_t q, uint32_t lower) const {
        Window w = {false, 0, 0, 0, 0};
        if (_n < 2) return w;
        const Edge &e0 = edge(0);
        for (uint8_t k = 1; k < _n; k++) {
            const Edge &ek = edge(k);
            if (ek.t < lower) break;
            uint32_t d = e0.c - ek.c;
            uint32_t dt = e0.t - ek.t;
            if (dt == 0) continue;
            w.ok = true;
            w.p = (uint32_t)((uint64_t)d * 3600 / dt);
            uint32_t m = d > dt ? d : dt;
            w.err = w.p / m + 1;
            w.start = ek.t;
            w.len = dt;
            if (m >= q) break;
        }
        return w;
    }

    void compute() {
        uint32_t now = _last_t;
        uint32_t lower = now > MAX_WIN_S ? now - MAX_WIN_S : 0;
        if (_barrier > lower) lower = _barrier;

        Window slow = window(QUALITY, lower);
        // The newest segment alone catches a step at once (it cannot straddle
        // it); the short window catches smaller steps its noise would hide.
        const uint32_t probes[] = {1, FAST_QUALITY};
        for (uint32_t q : probes) {
            Window fast = window(q, lower);
            if (!slow.ok || !fast.ok || fast.start <= slow.start) continue;
            uint32_t diff = slow.p > fast.p ? slow.p - fast.p : fast.p - slow.p;
            if (diff > 2 * (slow.err + fast.err)) {
                _barrier = fast.start;
                _steps++;
                slow = fast;
                break;
            }
        }

        uint32_t p = slow.ok ? slow.p : 0;
        _win = slow.ok ? slow.len : 0;
        if (_n > 0) {
            uint32_t g = now - edge(0).t;    // time since the counter last moved
            if (g >= MAX_WIN_S) {
                p = 0;
            } else if (g > 0 && p > 3600 / g) {
                p = 3600 / g;                // no edge in g s: rate < 1 unit / g
            }
        }
        _p = p;
    }
};
