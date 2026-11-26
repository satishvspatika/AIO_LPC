/* Host test (CTRL v1.18): the CTRL's rain debounce (main.c debounce_rfpin, taken from the source by
 * bench/host_tests.sh into rf_debounce.inc), called every 5 ms as the SysTick does, against rain
 * gauge pin waveforms. One count per bucket tip; the count is on the rise (the reed opening). */
#include <stdint.h>
#include <stdio.h>
#define LPC845_PORT 0
#define RF_PIN 0
#define GPIO 0
static int s_pin = 1;
#define GPIO_PinRead(g, port, pin) (s_pin)
volatile int rfpin_oldstate, rfpin_prevstate, rfpin_newstate, rfpin_debouncecnt, rf_val, calib_rf;
volatile int calib_mode = 0;
#define MAX_RF_DEBCNT 2
#include "rf_debounce.inc"

static int s_fails = 0;
static void check(const char *what, int got, int want)
{
    int ok = got == want; if (!ok) s_fails++;
    printf("%s %-58s got %3d want %3d\n", ok ? "PASS" : "FAIL", what, got, want);
}
typedef int (*wave_t)(int ms);
static int run(int rest, wave_t w, int ms)
{
    rfpin_oldstate = 1; rfpin_prevstate = 1; rfpin_newstate = 1; rfpin_debouncecnt = 0; rf_val = 0;   /* as main.c sets them at start-up */
    s_pin = rest;
    for (int t = 0; t < ms; t += 5) { s_pin = w(t); debounce_rfpin(); }
    return rf_val;
}
static int W_clean(int t) { return !(t >= 1000 && t < 1100); }
static int W_bounce(int t) { if (t >= 1000 && t < 1010) return (t / 5) & 1; if (t >= 1090 && t < 1100) return !((t / 5) & 1); return !(t >= 1000 && t < 1100); }
static int W_rebound(int t) { return !((t >= 1000 && t < 1080) || (t >= 1105 && t < 1135)); }
static int W_rebound_late(int t) { return !((t >= 1000 && t < 1080) || (t >= 1250 && t < 1280)); }
static int W_spike(int t) { return !(t >= 1000 && t < 1005); }
static int W_fast(int t) { int k = t % 1500; return !(k >= 500 && k < 560); }
static int W_08(int t) { int k = t % 800; return !(k >= 300 && k < 360); }
static int W_closed(int t) { (void)t; return 0; }
static int W_closed_tip(int t) { return !(t < 3000 || (t >= 5000 && t < 5080)); }
int main(void)
{
    check("clean tip", run(1, W_clean, 3000), 1);
    check("contact bounce at close and open", run(1, W_bounce, 3000), 1);
    check("bucket rebound 25 ms after release", run(1, W_rebound, 3000), 1);
    check("bucket rebound 170 ms after release", run(1, W_rebound_late, 3000), 1);
    check("5 ms spike", run(1, W_spike, 3000), 0);
    check("a tip every 1.5 s for 60 s", run(1, W_fast, 60000), 40);
    check("tips 0.8 s apart for 60 s", run(1, W_08, 60000), 75);
    check("power-on resting on the reed", run(0, W_closed, 5000), 0);
    check("power-on resting on the reed, then a tip", run(0, W_closed_tip, 8000), 2);
    printf("%s (%d failed)\n", s_fails ? "FAILED" : "ALL PASSED", s_fails);
    return s_fails != 0;
}
