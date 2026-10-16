// Native unit tests for PowerEstimator: pio test -e native
//
// A simulated meter integrates a power profile and reports floor(energy) in
// 1/10 Wh once per second, exactly like the Itron frames.

#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <PowerEstimator.h>

struct Meter {
    double units = 12345.37;   // energy in 1/10 Wh, arbitrary start phase
    uint32_t t = 1000;
    void step(double watt) {
        t++;
        units += watt / 360.0;   // 1 s at P W = P/3600 Wh = P/360 units
    }
    uint64_t counter() const { return (uint64_t)floor(units); }
};

static double watt(const PowerEstimator &e) { return e.power_dW() / 10.0; }

// Run `secs` frames at `w`, return worst relative error over the last `check` frames.
static double run(PowerEstimator &e, Meter &m, double w, int secs, int check) {
    double worst = 0;
    for (int i = 0; i < secs; i++) {
        m.step(w);
        TEST_ASSERT_TRUE(e.update(m.t, m.counter()));
        if (i >= secs - check) {
            double rel = fabs(watt(e) - w) / w;
            if (rel > worst) worst = rel;
        }
    }
    return worst;
}

void setUp() {}
void tearDown() {}

void test_constant_high_load_is_accurate_and_smooth() {
    PowerEstimator e; e.reset();
    Meter m;
    e.update(m.t, m.counter());
    double worst = run(e, m, 5400.0, 120, 100);
    // A one-frame delta would be off by up to 1/15 (360 W); the window keeps it ~1/30
    TEST_ASSERT_LESS_THAN_DOUBLE(0.04, worst);
}

void test_constant_medium_and_low_loads() {
    const double loads[] = {3000, 1200, 500, 230, 100, 40};
    for (double w : loads) {
        PowerEstimator e; e.reset();
        Meter m;
        e.update(m.t, m.counter());
        double worst = run(e, m, w, 400, 100);
        char msg[48];
        snprintf(msg, sizeof(msg), "load %.0f W, worst %.3f", w, worst);
        TEST_ASSERT_LESS_THAN_DOUBLE_MESSAGE(0.05, worst, msg);
    }
}

void test_zero_load_reports_zero() {
    PowerEstimator e; e.reset();
    Meter m;
    e.update(m.t, m.counter());
    run(e, m, 0.0001, 30, 1);
    TEST_ASSERT_EQUAL_UINT32(0, e.power_dW());
}

void test_step_up_followed_within_seconds() {
    PowerEstimator e; e.reset();
    Meter m;
    e.update(m.t, m.counter());
    run(e, m, 300, 200, 1);
    int secs = 0;
    for (; secs < 60; secs++) {
        m.step(4000);
        e.update(m.t, m.counter());
        if (fabs(watt(e) - 4000) / 4000 < 0.10) break;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(4, secs);
    // ... and it settles to the accurate long window afterwards
    double worst = run(e, m, 4000, 60, 30);
    TEST_ASSERT_LESS_THAN_DOUBLE(0.04, worst);
}

void test_step_down_decays_without_undershoot() {
    PowerEstimator e; e.reset();
    Meter m;
    e.update(m.t, m.counter());
    run(e, m, 5000, 60, 1);
    int secs = 0;
    for (; secs < 60; secs++) {
        m.step(200);
        e.update(m.t, m.counter());
        TEST_ASSERT_TRUE(watt(e) >= 200 * 0.95);   // the bound never undershoots
        if (watt(e) < 200 * 1.10) break;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(12, secs);
}

void test_switch_off_decays_to_zero() {
    PowerEstimator e; e.reset();
    Meter m;
    e.update(m.t, m.counter());
    run(e, m, 2000, 60, 1);
    for (int i = 0; i < 10; i++) {
        m.step(0);
        e.update(m.t, m.counter());
    }
    TEST_ASSERT_LESS_THAN_DOUBLE(40.0, watt(e));   // 1 unit / 10 s = 36 W bound
    for (uint32_t i = 0; i < PowerEstimator::MAX_WIN_S; i++) {
        m.step(0);
        e.update(m.t, m.counter());
    }
    TEST_ASSERT_EQUAL_UINT32(0, e.power_dW());
}

void test_no_false_steps_at_constant_load() {
    const double loads[] = {5400, 800, 150};
    for (double w : loads) {
        PowerEstimator e; e.reset();
        Meter m;
        e.update(m.t, m.counter());
        run(e, m, w, 120, 1);
        uint32_t before = e.steps();
        run(e, m, w, 600, 1);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, e.steps(), "step detected at constant load");
    }
}

void test_backwards_frame_is_rejected() {
    PowerEstimator e; e.reset();
    TEST_ASSERT_TRUE(e.update(100, 5000));
    TEST_ASSERT_TRUE(e.update(101, 5010));
    TEST_ASSERT_FALSE(e.update(102, 4000));   // counter backwards
    TEST_ASSERT_FALSE(e.update(99, 5020));    // uptime backwards
    TEST_ASSERT_TRUE(e.update(102, 5020));
}

void test_starts_from_zero_after_reset() {
    PowerEstimator e; e.reset();
    TEST_ASSERT_TRUE(e.update(100, 5000));
    TEST_ASSERT_EQUAL_UINT32(0, e.power_dW());
    TEST_ASSERT_TRUE(e.update(101, 5015));
    TEST_ASSERT_EQUAL_UINT32(0, e.power_dW());   // one edge: no window yet
    TEST_ASSERT_TRUE(e.update(102, 5030));
    TEST_ASSERT_EQUAL_UINT32(54000, e.power_dW());   // 15 units/s = 5400.0 W
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_constant_high_load_is_accurate_and_smooth);
    RUN_TEST(test_constant_medium_and_low_loads);
    RUN_TEST(test_zero_load_reports_zero);
    RUN_TEST(test_step_up_followed_within_seconds);
    RUN_TEST(test_step_down_decays_without_undershoot);
    RUN_TEST(test_switch_off_decays_to_zero);
    RUN_TEST(test_no_false_steps_at_constant_load);
    RUN_TEST(test_backwards_frame_is_rejected);
    RUN_TEST(test_starts_from_zero_after_reset);
    return UNITY_END();
}
