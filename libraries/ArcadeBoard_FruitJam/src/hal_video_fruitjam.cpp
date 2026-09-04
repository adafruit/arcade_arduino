// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// hal_video.h implementation for Adafruit Fruit Jam.
//
// Ported from invaders_pico's pico_video.c initialize_video()/
// video_core1_main()/draw_frame()/draw_error_frame() -- same underlying
// low-level "libdvi" scanline-queue API (dvi_init/dvi_register_irqs_this_core/
// dvi_start/queue_*_blocking_u32), just reached via the vendored copy inside
// the "PicoDVI - Adafruit Fork" Arduino library instead of a CMake
// FetchContent checkout. <PicoDVI.h> is the only way to reach that
// library's adafruit_fruitjam_cfg pin config from outside its own sources
// (it lives outside that library's src/, only reachable via PicoDVI.h's own
// relative include) -- we only use the low-level dvi_inst/queue API it
// pulls in, not its higher-level DVIGFX* framebuffer classes.
//
// Output: 640x480 @ 60 Hz RGB565 physically, presented to the machine layer
// as a 320x240 canvas of square logical pixels (both axes are doubled --
// see DVI_VERTICAL_REPEAT_USED / DVI_HORIZONTAL_REPEAT_USED below).
// See ArcadeMachine_Invaders's
// invaders_video.cpp for the rotation/mirror math this geometry was
// calibrated against.
#include "pico/sync.h"     // next_striped_spin_lock_num()
#include "pico/platform.h" // __not_in_flash()
#include "hardware/dma.h"  // DMA_IRQ_0
#include <PicoDVI.h>        // libdvi (dvi_init, dvi_inst, queue_*_blocking_u32) + adafruit_fruitjam_cfg
#include "pico/time.h"    // time_us_32() -- profiling counter below
#include "arcade_hal_video.h"

#define DVI_WIDTH  640u
#define DVI_HEIGHT 480u

// This fork's dvi_vertical_repeat defaults to 2 (each submitted scanline
// physically displayed twice). The reference clone's CMake build overrode
// the equivalent upstream setting to 1 (one submission per physical row),
// but forcing repeat=1 here through this fork's low-level dvi_inst API
// produced a solid red screen outright (confirmed on real hardware) --
// this fork's internals evidently don't support that combination cleanly.
// Rather than fight it, we work with the default: submit half as many
// scanlines per frame (which is why HAL_VIDEO_HEIGHT below is 240, not
// 480), each naturally shown twice to fill all 480 physical rows.
// Left as a variable
// (not a #define) since dvi_vertical_repeat itself is one upstream --
// change this if a future fork update makes repeat=1 viable and you want
// back the full 480-unique-row resolution.
#define DVI_VERTICAL_REPEAT_USED 2

// HORIZONTAL doubling, the half that used to be missing from this contract.
// libdvi's 16bpp path encodes `pixwidth / 2` source pixels across the full
// line (_dvi_prepare_scanline_16bpp() in src/libdvi/dvi.c passes
// `pixwidth / 2` to tmds_encode_data_channel_16bpp()), so it reads only
// scanbuf[0..319] of a 640-pixel line and doubles each one -- exactly as
// dvi_vertical_repeat = 2 doubles vertically. It is not configurable here:
// DVI_SYMBOLS_PER_WORD defaults to 2 in dvi_config_defs.h.
#define DVI_HORIZONTAL_REPEAT_USED 2

// The canvas a renderer actually draws into: 320x240 logical pixels, each
// shown as a 2x2 block of physical pixels. See arcade_hal_video.h for why
// these are the canvas and not the 640x480 physical resolution -- reporting
// the physical resolution here is what let DEVNOTES #23's landscape
// resampling bug exist.
const uint32_t HAL_VIDEO_WIDTH  = DVI_WIDTH  / DVI_HORIZONTAL_REPEAT_USED;
const uint32_t HAL_VIDEO_HEIGHT = DVI_HEIGHT / DVI_VERTICAL_REPEAT_USED;

static struct dvi_inst dvi;

// Scanline pixel buffers cycled between the render side (acquire/submit)
// and the DVI encode side (hal_video_run).
//
// THIS IS THE PIPELINE'S RUNWAY, and it was long believed to be a hard
// ceiling of 8. It is not. The vendored libdvi's dvi_init() does hardcode
// q_colour_free/q_colour_valid to 8 elements each
// (queue_init_with_spinlock(..., 8, ...) in src/libdvi/dvi.c), and a prior
// attempt to raise this number alone hung setup() completely: the 9th
// queue_add_blocking_u32() below blocks forever once the queue's real
// capacity is full, because nothing drains it until Core 1's pump starts,
// which never happens because setup() itself is stuck. Full black screen,
// not a glitch.
//
// The fix is to raise the QUEUES too, which hal_video_init() now does by
// re-initialising the two colour queues after dvi_init(). The vendored
// library is left untouched -- they are a plain producer/consumer pair
// used only by dvi_scanbuf_main_16bpp() and by acquire/submit here.
//
// Why 16: a scanline is 63.49us of DVI time (800x525 at 25.2MHz with
// dvi_vertical_repeat 2), so 8 buffers is 508us of slack and 16 is 1016us.
// Galaga has ~2ms of average headroom per frame and still starved, because
// its shortfall is a BURST -- measured peak drawdown of 4-6 buffers against
// the 8 available, before counting the ~300us stall of DEVNOTES #84, which
// on its own eats 5. See DEVNOTES #85.
//
// Cost is 10,240 bytes of SRAM. Shrinking the buffers to HAL_VIDEO_WIDTH
// (DISPLAY_GEOMETRY.md phase 5) would make 16 buffers cost exactly what 8
// cost at 640 wide, i.e. free.
#define N_SCANBUF 32
// Still DVI_WIDTH (640) entries although libdvi reads only the first
// HAL_VIDEO_WIDTH (320) of each -- 5,120 bytes of deliberate slack. Kept
// for now so that a renderer which has not yet been converted to canvas
// coordinates writes into unused space rather than over the next buffer.
// Shrinking this to HAL_VIDEO_WIDTH is a separate step (DISPLAY_GEOMETRY.md
// phase 5), to be taken once every renderer is known to stop at 320.
static uint16_t scanbuf[N_SCANBUF][DVI_WIDTH];

bool hal_video_init(void) {
    dvi_vertical_repeat = DVI_VERTICAL_REPEAT_USED; // explicit, even though it matches the library default

    dvi.timing  = &dvi_timing_640x480p_60hz;
    dvi.ser_cfg = adafruit_fruitjam_cfg;
    dvi_init(&dvi, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Deepen the two colour queues to match N_SCANBUF. dvi_init() sized them
    // at 8; re-initialising here is safe because they are a plain
    // producer/consumer pair (see the N_SCANBUF comment), nothing has been
    // pushed into them yet, and Core 1's pump has not started -- both cores
    // pick up the new spinlock through the queue struct itself. This must
    // stay AFTER dvi_init() and BEFORE the fill loop below.
    //
    // queue_init_with_spinlock() allocates, so the original 8-entry
    // allocations are leaked: 64 bytes, once, at boot. Deliberate -- there
    // is no queue_deinit() in this SDK version.
    {
        const uint spin_colour = next_striped_spin_lock_num();
        queue_init_with_spinlock(&dvi.q_colour_valid, sizeof(void *), N_SCANBUF, spin_colour);
        queue_init_with_spinlock(&dvi.q_colour_free,  sizeof(void *), N_SCANBUF, spin_colour);
    }

    // Populate q_colour_free with all scanline buffers. hal_video_acquire_
    // scanline()/hal_video_submit_scanline() recycle them one at a time
    // through q_colour_valid.
    for (int i = 0; i < N_SCANBUF; i++) {
        void *p = scanbuf[i];
        queue_add_blocking_u32(&dvi.q_colour_free, &p);
    }
    return true;
}

// Microseconds spent BLOCKED in hal_video_acquire_scanline() since the
// last hal_video_take_blocked_us() call.
//
// Why this exists: acquire blocks until Core 1's DVI pump frees a buffer,
// so a caller timing its own frame loop measures max(its own work, the DVI
// frame period) -- once the work fits, the number pins at ~16.7ms and tells
// you NOTHING about the remaining headroom. Subtracting this counter
// recovers the real work time, which is what you need before adding
// anything (audio, a new video layer) to a frame that already fits.
static volatile uint32_t s_blocked_us = 0;

// Starvation-risk counter; see arcade_hal_video.h. Incremented when a
// submit leaves the valid-scanline queue at or below one entry, meaning
// Core 1 is about to run dry. The unlocked read is deliberate: this is a
// diagnostic, a race costs at most a miscount, and the locked variant
// would add a critical section to a path that runs 240 times a frame.
static volatile uint32_t s_starve_events = 0;
// Lowest level the VALID queue has been seen at, i.e. how much runway was
// left at the worst moment. This is the honest measure of pipeline margin:
// `starve` only says whether a threshold was crossed, and the noblock-run
// detector it replaced was valid ONLY while N_SCANBUF was 8. At depth 8,
// free and valid are complementary and tight, so "acquire did not block"
// implied "valid is nearly empty". At 16 the two decouple -- valid can sit
// at a healthy 8 while 7 buffers are free and acquire never blocks -- and
// the run counter reads high with no starvation at all. See DEVNOTES #85.
static volatile uint32_t s_min_valid = 0xFFFFFFFFu;

uint16_t *hal_video_acquire_scanline(void) {
    uint16_t *buf;
    uint32_t t0 = time_us_32();
    queue_remove_blocking_u32(&dvi.q_colour_free, &buf);
    s_blocked_us += time_us_32() - t0;
    return buf;
}

uint32_t hal_video_take_blocked_us(void) {
    uint32_t v = s_blocked_us;
    s_blocked_us = 0;
    return v;
}

void hal_video_submit_scanline(uint16_t *buf) {
    queue_add_blocking_u32(&dvi.q_colour_valid, &buf);
    const uint32_t lvl = queue_get_level_unsafe(&dvi.q_colour_valid);
    if (lvl <= 1u) s_starve_events++;
    if (lvl < s_min_valid) s_min_valid = lvl;
}

// Instantaneous VALID-queue depth. For callers that want to know WHERE in
// the frame the pipeline runs thin, which a per-frame minimum cannot say.
uint32_t hal_video_valid_level(void) {
    return queue_get_level_unsafe(&dvi.q_colour_valid);
}

uint32_t hal_video_take_min_valid_level(void) {
    uint32_t v = s_min_valid;
    s_min_valid = 0xFFFFFFFFu;
    return (v == 0xFFFFFFFFu) ? 0u : v;
}

uint32_t hal_video_scanbuf_count(void) { return (uint32_t)N_SCANBUF; }

uint32_t hal_video_take_starve_count(void) {
    uint32_t v = s_starve_events;
    s_starve_events = 0;
    return v;
}

// Runs on whatever core calls it and never returns -- see hal_video.h's
// hal_video_run() doc comment for the required call-order (only after
// scanlines are ready to be fed continuously).
void __not_in_flash("dvi") hal_video_run(void) {
    dvi_register_irqs_this_core(&dvi, DMA_IRQ_0);
    dvi_start(&dvi);
    dvi_scanbuf_main_16bpp(&dvi);
    __builtin_unreachable();
}
