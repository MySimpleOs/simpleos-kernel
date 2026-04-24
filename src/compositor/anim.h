#pragma once

/* Animation engine — Q16.16 fixed-point, because the kernel is built
 * with -mgeneral-regs-only (no FP/SIMD). Two kinds of animation: spring
 * (semi-implicit Euler, interruptible) and timed easing curves.
 *
 * Each anim can optionally bind itself to an int32_t or uint8_t the
 * caller cares about; after every tick the fixed value is rounded,
 * clamped, and written to that pointer. That lets the caller set up an
 * anim and then forget about it — the compositor thread's frame call
 * keeps the destination field fresh.
 */

#include <stdint.h>

/* Fixed-point helpers (Q16.16). One = 65536. */
typedef int32_t fx16;

#define FX_ONE              (1 << 16)
#define FX_HALF             (1 << 15)
#define FX_FROM_INT(i)      ((fx16) ((int32_t)(i) << 16))
#define FX_FROM_MILLI(m)    ((fx16) (((int64_t)(m) * FX_ONE) / 1000))

static inline int32_t FX_TO_INT_ROUND(fx16 f) {
    return (int32_t) ((f + (f >= 0 ? FX_HALF : -FX_HALF)) >> 16);
}
static inline fx16    fx_mul(fx16 a, fx16 b) {
    return (fx16) (((int64_t) a * (int64_t) b + FX_HALF) >> 16);
}
static inline fx16    fx_div(fx16 a, fx16 b) {
    if (b == 0) return 0;
    return (fx16) (((int64_t) a << 16) / b);
}

typedef enum {
    ANIM_SPRING,
    ANIM_EASE,
} anim_kind_t;

typedef enum {
    EASE_LINEAR,
    EASE_IN_CUBIC,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_CUBIC,
    EASE_OUT_BACK,
} anim_easing_t;

typedef enum {
    BIND_NONE,
    BIND_I32,
    BIND_U8,
} anim_bind_t;

struct anim {
    uint8_t       used;
    anim_kind_t   kind;
    fx16          current;       /* live animated value                      */
    fx16          velocity;      /* spring velocity                          */
    fx16          from;
    fx16          target;
    fx16          t;             /* ease progress 0..ONE                     */
    fx16          duration;      /* seconds (ease only)                      */
    fx16          stiffness;
    fx16          damping;
    anim_easing_t easing;

    anim_bind_t   bind_kind;
    void         *bind_ptr;
    fx16          bind_scale;    /* applied before writing to bound target   */
    fx16          bind_offset;
    int32_t       bind_min;      /* clamp range (inclusive)                  */
    int32_t       bind_max;

    uint8_t       done;
    uint8_t       loop;          /* 1 = ping-pong from<->target on done      */
};

/* Allocate / release from a static pool. Returns NULL if pool is full. */
struct anim *anim_new(void);
void         anim_free(struct anim *a);

/* Start / configure animations. Re-calling on an already-running anim
 * preserves current + velocity so spring overrides "feel right". */
void anim_spring(struct anim *a, fx16 from, fx16 to,
                 fx16 stiffness, fx16 damping);
void anim_ease  (struct anim *a, fx16 from, fx16 to,
                 fx16 duration_s, anim_easing_t easing);

/* Redirect an in-flight anim to a new target, preserving current value
 * and (for spring) velocity — the reason springs feel natural when the
 * user retargets mid-flight. */
void anim_retarget(struct anim *a, fx16 new_target);
void anim_cancel  (struct anim *a);
int  anim_done    (const struct anim *a);
fx16 anim_value   (const struct anim *a);
void anim_set_loop(struct anim *a, int enable);

/* Optional binding: after each tick, (current*scale + offset) rounded
 * to int and clamped to [min,max] is written to *ptr. */
void anim_bind_i32(struct anim *a, int32_t *ptr,
                   fx16 scale, fx16 offset, int32_t min, int32_t max);
void anim_bind_u8 (struct anim *a, uint8_t *ptr,
                   fx16 scale, fx16 offset, int32_t min, int32_t max);

/* Advance every active animation by dt (seconds, fixed). Called once
 * per frame from the compositor thread. */
void anim_tick_all(fx16 dt);
