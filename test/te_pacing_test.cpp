/* te_pacing_test.cpp - host check for ST7305Tft's double-buffer state machine.
 *
 * The machine has three slots - drawing_, ready_, sending_ - written from two
 * threads with no lock. That is only safe because of an ordering rule that is
 * invisible at the call site, so it is asserted here rather than trusted:
 *
 *   THE BUFFER BEING SENT IS NEVER HANDED TO THE PRODUCER.
 *
 * On hardware a violation means the emulator dithers into the 15000 bytes that
 * are mid-flight on the SPI bus - the exact MCU-side tear that double buffering
 * exists to prevent, and one that would look like a panel fault rather than a
 * software one. So every sequence below ends in checkInvariants().
 *
 * startTePacing() deliberately still sets up the state machine when built off
 * an ESP32: only the ISR and the task are compiled out. That is what lets this
 * file drive takeReady()/sendDone() by hand, in the interleavings a test cannot
 * otherwise reach.
 *
 * Arduino IDE ignores sketch subfolders other than src/ and data/, so this
 * folder is not part of the sketch build.
 *
 *   cd waveshare_rlcd_nes
 *   cc -x c++ -std=c++17 -O2 -Wall -Wextra -lc++ -I. -Itest/arduino_stubs \
 *      -include Arduino.h test/te_pacing_test.cpp test/stub_impl.cpp st7305_tft.cpp \
 *      -o /tmp/te_pacing_test && /tmp/te_pacing_test
 */
#include <stdio.h>
#include <string.h>

#include "st7305_tft.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* The one rule the whole design rests on, plus the bookkeeping that implies it.
 * Called after every state transition in every sequence below. */
static void checkInvariants(const ST7305Tft &t, const char *where)
{
    const int d = t.drawingIndex();
    const int r = t.readyIndex();
    const int s = t.sendingIndex();

    char msg[160];

    snprintf(msg, sizeof msg, "%s: indices in range", where);
    check(d >= -1 && d < 2 && r >= -1 && r < 2 && s >= -1 && s < 2, msg);

    /* Pairwise distinct when set. Two slots naming one buffer is precisely the
     * "producer draws into the buffer on the wire" bug. */
    snprintf(msg, sizeof msg, "%s: drawing != sending", where);
    check(d < 0 || s < 0 || d != s, msg);
    snprintf(msg, sizeof msg, "%s: drawing != ready", where);
    check(d < 0 || r < 0 || d != r, msg);
    snprintf(msg, sizeof msg, "%s: ready != sending", where);
    check(r < 0 || s < 0 || r != s, msg);
}

/* --- 1. unpaced: the menu path must behave exactly as it always did -------- */

static void test_unpaced_passthrough()
{
    ST7305Tft t;
    t.init();

    check(t.beginFrame(), "unpaced: beginFrame always succeeds");
    check(t.beginFrame(), "unpaced: beginFrame is idempotent");

    t.fillScreen(TFT_BLACK);
    check(t.dirty(), "unpaced: drawing marks dirty");

    /* present() collapses to flush() with no panel behind it - the stub SPI
     * swallows the bytes. What matters is that it clears dirty like flush does
     * and leaves no half-set state behind. */
    t.present();
    check(!t.dirty(), "unpaced: present clears dirty");
    check(t.readyIndex() < 0, "unpaced: nothing is left queued");
    check(t.sendingIndex() < 0, "unpaced: nothing is left in flight");
}

/* --- 2. the ordinary cycle ------------------------------------------------ */

static void test_single_cycle()
{
    ST7305Tft t;
    t.init();
    t.startTePacing(-1, 100);
    checkInvariants(t, "paced/cold");

    check(t.takeReady() < 0, "cold: nothing to send before the first frame");

    check(t.beginFrame(), "cycle: producer gets a buffer");
    const int drawn = t.drawingIndex();
    check(drawn >= 0, "cycle: drawing index is set");
    checkInvariants(t, "cycle/drawing");

    t.present();
    check(t.readyIndex() == drawn, "cycle: present queues the drawn buffer");
    check(t.drawingIndex() < 0, "cycle: producer gives up ownership");
    checkInvariants(t, "cycle/ready");

    const int sent = t.takeReady();
    check(sent == drawn, "cycle: the sent buffer is the drawn one");
    check(t.sendingIndex() == drawn, "cycle: sending index is set");
    check(t.readyIndex() < 0, "cycle: the queue is emptied by the take");
    checkInvariants(t, "cycle/sending");

    check(t.takeReady() < 0, "cycle: a frame is never sent twice");

    t.sendDone();
    check(t.sendingIndex() < 0, "cycle: sendDone frees the buffer");
    checkInvariants(t, "cycle/done");
}

/* --- 3. production is paced to the panel, not run flat out ---------------- */

static void test_production_is_paced()
{
    ST7305Tft t;
    t.init();
    t.startTePacing(-1, 100);

    /* The emulator offers a frame every 16.7 ms against a panel that takes one
     * every ~43. A queued frame that has not been sent yet would be thrown away
     * by drawing over it, so the second and third offers must be refused rather
     * than dithered - that refusal is where the CPU saving lives. */
    check(t.beginFrame(), "paced: the first offer is taken");
    const int queued = t.drawingIndex();
    t.present();
    checkInvariants(t, "paced/queued");

    for (int i = 0; i < 4; i++) {
        char msg[80];
        snprintf(msg, sizeof msg, "paced: offer %d refused while one is queued", i);
        check(!t.beginFrame(), msg);
        checkInvariants(t, "paced/refused");
    }

    check(t.drawingIndex() < 0, "paced: a refused beginFrame claims nothing");
    check(t.readyIndex() == queued, "paced: the queued frame is untouched");

    /* The panel taking it is what lets the next one be drawn. */
    check(t.takeReady() == queued, "paced: the take drains the queue");
    check(t.beginFrame(), "paced: and releases the producer");
    check(t.drawingIndex() != queued, "paced: onto the other buffer");
    checkInvariants(t, "paced/next");
}

/* --- 4. what the refusal is actually gated on ----------------------------- */

static void test_refusal_is_gated_on_the_queue()
{
    ST7305Tft t;
    t.init();
    t.startTePacing(-1, 100);

    t.beginFrame();
    t.present();
    const int sending = t.takeReady();     /* buffer A is on the wire */
    check(sending >= 0, "refuse: a send is in flight");

    check(t.beginFrame(), "refuse: the second buffer is still free");
    check(t.drawingIndex() != sending, "refuse: and it is not the one in flight");
    t.present();                           /* buffer B is now queued */
    checkInvariants(t, "refuse/both-busy");

    check(!t.beginFrame(), "refuse: a queued frame refuses the producer");
    check(t.drawingIndex() < 0, "refuse: a refused beginFrame claims nothing");
    checkInvariants(t, "refuse/refused");

    /* The send finishing frees a buffer but does NOT release the producer: the
     * gate is the queue, not the bus. Getting this backwards would restore
     * 60 Hz dithering the moment a send happened to be short. */
    t.sendDone();
    check(!t.beginFrame(), "refuse: sendDone alone does not release it");
    checkInvariants(t, "refuse/still-queued");

    const int sent2 = t.takeReady();
    check(sent2 >= 0 && sent2 != sending, "refuse: the queued frame goes next");
    check(t.beginFrame(), "refuse: draining the queue releases the producer");
    check(t.drawingIndex() == sending, "refuse: it reuses the buffer just freed");
    checkInvariants(t, "refuse/released");
}

/* --- 5. long interleaved run ---------------------------------------------- */

static void test_interleaved_sequence()
{
    ST7305Tft t;
    t.init();
    t.startTePacing(-1, 100);

    /* A deterministic stand-in for 60 Hz of production against ~23 Hz of TE:
     * three produce attempts per send, with the send held open across one of
     * them so the "both spoken for" path is exercised repeatedly. */
    int pushed = 0, refused = 0;
    for (int cycle = 0; cycle < 40; cycle++) {
        for (int k = 0; k < 3; k++) {
            if (t.beginFrame()) {
                /* A byte written into whatever we were handed. If the machine
                 * ever hands over the in-flight buffer, this is the write that
                 * would corrupt it - the invariant check below is what catches
                 * that, since the pointer itself is what beginFrame chose. */
                t.fillScreen(cycle & 1 ? TFT_BLACK : TFT_WHITE);
                t.present();
            } else {
                refused++;
            }
            checkInvariants(t, "interleaved/produce");
        }

        const int i = t.takeReady();
        checkInvariants(t, "interleaved/take");
        if (i >= 0) {
            pushed++;
            t.sendDone();
            checkInvariants(t, "interleaved/done");
        }
    }

    check(pushed == 40, "interleaved: every cycle pushed exactly one frame");
    /* Three offers per panel period, one taken: the other two are the frames
     * the panel was never going to show, refused before they cost any
     * dithering. That ratio is the whole point of pacing production. */
    check(refused == 80, "interleaved: the two surplus offers per cycle are refused");
}

/* --- 6. the send is held open across the producer, the real hardware case -- */

static void test_send_held_open()
{
    ST7305Tft t;
    t.init();
    t.startTePacing(-1, 100);

    int refused = 0;
    for (int cycle = 0; cycle < 20; cycle++) {
        /* Take first, then produce while the take is still open: this is the
         * 6.5 ms of SPI during which the emulator keeps running. */
        const int in_flight = t.takeReady();

        for (int k = 0; k < 3; k++) {
            if (t.beginFrame()) {
                check(t.drawingIndex() != in_flight,
                      "held: producer never gets the in-flight buffer");
                t.present();
            } else {
                refused++;
            }
            checkInvariants(t, "held/produce");
        }

        if (in_flight >= 0) t.sendDone();
        checkInvariants(t, "held/done");
    }

    /* With one buffer in flight and one queued there is genuinely nowhere to
     * draw, so refusals are expected here - they are dropped frames, not a
     * fault. The point is that they are refusals and not corruption. */
    check(refused > 0, "held: the producer is refused while both are spoken for");
}

int main(void)
{
    test_unpaced_passthrough();
    test_single_cycle();
    test_production_is_paced();
    test_refusal_is_gated_on_the_queue();
    test_interleaved_sequence();
    test_send_held_open();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
