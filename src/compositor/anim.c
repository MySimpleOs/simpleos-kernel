#include "anim.h"

#include <stdint.h>
#include <stddef.h>

#define ANIM_POOL 64

static struct anim pool[ANIM_POOL];

/* ---------- pool ------------------------------------------------- */

struct anim *anim_new(void) {
    for (int i = 0; i < ANIM_POOL; i++) {
        if (!pool[i].used) {
            struct anim *a = &pool[i];
            for (unsigned b = 0; b < sizeof(*a); b++) ((uint8_t *) a)[b] = 0;
            a->used      = 1;
            a->bind_kind = BIND_NONE;
            a->bind_min  = -0x7fffffff;
            a->bind_max  =  0x7fffffff;
            return a;
        }
    }
    return NULL;
}

void anim_free(struct anim *a) {
    if (a) a->used = 0;
}

/* ---------- configuration ---------------------------------------- */

void anim_spring(struct anim *a, fx16 from, fx16 to,
                 fx16 stiffness, fx16 damping) {
    if (!a) return;
    a->kind      = ANIM_SPRING;
    if (!a->current || a->done) a->current = from;
    a->from      = from;
    a->target    = to;
    a->stiffness = stiffness;
    a->damping   = damping;
    a->done      = 0;
}

void anim_ease(struct anim *a, fx16 from, fx16 to,
               fx16 duration_s, anim_easing_t easing) {
    if (!a) return;
    a->kind     = ANIM_EASE;
    a->current  = from;
    a->from     = from;
    a->target   = to;
    a->duration = duration_s > 0 ? duration_s : FX_ONE;
    a->easing   = easing;
    a->t        = 0;
    a->velocity = 0;
    a->done     = 0;
}

void anim_retarget(struct anim *a, fx16 new_target) {
    if (!a) return;
    a->target = new_target;
    a->done   = 0;
    if (a->kind == ANIM_EASE) {
        a->from = a->current;
        a->t    = 0;
    }
}

void anim_cancel(struct anim *a)      { if (a) a->done = 1; }
int  anim_done  (const struct anim *a){ return !a || a->done; }
fx16 anim_value (const struct anim *a){ return a ? a->current : 0; }
void anim_set_loop(struct anim *a, int enable) { if (a) a->loop = enable ? 1 : 0; }

/* ---------- binding ---------------------------------------------- */

void anim_bind_i32(struct anim *a, int32_t *ptr,
                   fx16 scale, fx16 offset, int32_t min, int32_t max) {
    if (!a) return;
    a->bind_kind   = BIND_I32;
    a->bind_ptr    = ptr;
    a->bind_scale  = scale ? scale : FX_ONE;
    a->bind_offset = offset;
    a->bind_min    = min;
    a->bind_max    = max;
}

void anim_bind_u8(struct anim *a, uint8_t *ptr,
                  fx16 scale, fx16 offset, int32_t min, int32_t max) {
    if (!a) return;
    a->bind_kind   = BIND_U8;
    a->bind_ptr    = ptr;
    a->bind_scale  = scale ? scale : FX_ONE;
    a->bind_offset = offset;
    a->bind_min    = min < 0 ? 0   : min;
    a->bind_max    = max > 255 ? 255 : max;
}

/* ---------- easings ---------------------------------------------- */

/* All take / return Q16.16 in [0, ONE]. */

static fx16 ease_linear(fx16 t) { return t; }

static fx16 ease_in_cubic(fx16 t) {
    fx16 t2 = fx_mul(t, t);
    return fx_mul(t2, t);
}
static fx16 ease_out_cubic(fx16 t) {
    fx16 inv  = FX_ONE - t;
    fx16 inv2 = fx_mul(inv, inv);
    return FX_ONE - fx_mul(inv2, inv);
}
static fx16 ease_in_out_cubic(fx16 t) {
    if (t < FX_HALF) {
        fx16 t2 = fx_mul(t, t);
        return 4 * fx_mul(t2, t);
    }
    fx16 s  = FX_ONE - t;
    fx16 s2 = fx_mul(s, s);
    return FX_ONE - 4 * fx_mul(s2, s);
}
static fx16 ease_out_back(fx16 t) {
    /* c1 = 2.70158 ≈ 177003 (Q16.16); c3 = 1.70158 ≈ 111467 */
    const fx16 c1 = 177003;
    const fx16 c3 = 111467;
    fx16 u  = t - FX_ONE;
    fx16 u2 = fx_mul(u, u);
    fx16 u3 = fx_mul(u2, u);
    /* return 1 + c1*u^3 + c3*u^2 */
    return FX_ONE + fx_mul(c1, u3) + fx_mul(c3, u2);
}

static fx16 apply_ease(anim_easing_t e, fx16 t) {
    switch (e) {
    case EASE_LINEAR:       return ease_linear(t);
    case EASE_IN_CUBIC:     return ease_in_cubic(t);
    case EASE_OUT_CUBIC:    return ease_out_cubic(t);
    case EASE_IN_OUT_CUBIC: return ease_in_out_cubic(t);
    case EASE_OUT_BACK:     return ease_out_back(t);
    }
    return t;
}

/* ---------- step + tick ------------------------------------------ */

static fx16 fx_abs(fx16 v) { return v < 0 ? -v : v; }

static void step_spring(struct anim *a, fx16 dt) {
    /* Semi-implicit Euler: update velocity first (force=k*err - c*v),
     * then apply to position. Stable for reasonable k/c at 120 Hz. */
    fx16 err = a->target - a->current;
    fx16 force = fx_mul(a->stiffness, err) - fx_mul(a->damping, a->velocity);
    a->velocity += fx_mul(force, dt);
    a->current  += fx_mul(a->velocity, dt);

    /* Settle threshold: tight enough for pixel precision (~0.5/256). */
    const fx16 pos_eps = FX_ONE / 256;
    const fx16 vel_eps = FX_ONE / 64;
    if (fx_abs(err) < pos_eps && fx_abs(a->velocity) < vel_eps) {
        a->current  = a->target;
        a->velocity = 0;
        a->done     = 1;
    }
}

static void step_ease(struct anim *a, fx16 dt) {
    /* Advance t by dt / duration (both in seconds, fixed-point). */
    a->t += fx_div(dt, a->duration);
    if (a->t >= FX_ONE) {
        a->t    = FX_ONE;
        a->done = 1;
    }
    fx16 k     = apply_ease(a->easing, a->t);
    fx16 delta = a->target - a->from;
    a->current = a->from + fx_mul(k, delta);
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void apply_binding(struct anim *a) {
    if (a->bind_kind == BIND_NONE || !a->bind_ptr) return;

    fx16 scaled = fx_mul(a->current, a->bind_scale) + a->bind_offset;
    int32_t iv  = FX_TO_INT_ROUND(scaled);
    iv = clamp_i32(iv, a->bind_min, a->bind_max);

    if (a->bind_kind == BIND_I32) {
        *(volatile int32_t *) a->bind_ptr = iv;
    } else { /* BIND_U8 */
        *(volatile uint8_t *) a->bind_ptr = (uint8_t) (iv & 0xff);
    }
}

void anim_tick_all(fx16 dt) {
    for (int i = 0; i < ANIM_POOL; i++) {
        struct anim *a = &pool[i];
        if (!a->used) continue;

        if (a->done) {
            if (!a->loop) continue;
            /* Ping-pong: swap from<->target, restart. Kept separate from
             * the live value so the transition is continuous. */
            fx16 tmp = a->from;
            a->from   = a->target;
            a->target = tmp;
            a->done   = 0;
            if (a->kind == ANIM_EASE) {
                a->t       = 0;
                a->current = a->from;
            }
        }

        switch (a->kind) {
        case ANIM_SPRING: step_spring(a, dt); break;
        case ANIM_EASE:   step_ease(a, dt);   break;
        }
        apply_binding(a);
    }
}
