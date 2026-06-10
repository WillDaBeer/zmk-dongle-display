/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/modifiers.h>

#include "modifiers.h"

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_CAPSWORD)
// Provided by the config repo module (config/include/caps_word_ind.h), exposed
// on the global include path via its zephyr_include_directories(include).
#include "caps_word_ind.h"
#include <zmk/hid_indicators.h> // host caps-lock LED state on the central
LV_IMG_DECLARE(shift_filled_icon);
#endif

struct modifiers_state {    
    uint8_t modifiers;
};

struct modifier_symbol {    
    uint8_t modifier;
    const lv_img_dsc_t *symbol_dsc;
    lv_obj_t *symbol;
    lv_obj_t *selection_line; 
    bool is_active;
};

LV_IMG_DECLARE(control_icon);
struct modifier_symbol ms_control = {
    .modifier = MOD_LCTL | MOD_RCTL,
    .symbol_dsc = &control_icon,
};

LV_IMG_DECLARE(shift_icon);
struct modifier_symbol ms_shift = {
    .modifier = MOD_LSFT | MOD_RSFT,
    .symbol_dsc = &shift_icon,
};

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_MAC_MODIFIERS)
LV_IMG_DECLARE(opt_icon);
struct modifier_symbol ms_opt = {
    .modifier = MOD_LALT | MOD_RALT,
    .symbol_dsc = &opt_icon,
};

LV_IMG_DECLARE(cmd_icon);
struct modifier_symbol ms_cmd = {
    .modifier = MOD_LGUI | MOD_RGUI,
    .symbol_dsc = &cmd_icon,
};

struct modifier_symbol *modifier_symbols[] = {
    // this order determines the order of the symbols
    &ms_cmd,
    &ms_opt,
    &ms_control,
    &ms_shift
};
#else
LV_IMG_DECLARE(alt_icon);
struct modifier_symbol ms_alt = {
    .modifier = MOD_LALT | MOD_RALT,
    .symbol_dsc = &alt_icon,
};

LV_IMG_DECLARE(win_icon);
struct modifier_symbol ms_win = {
    .modifier = MOD_LGUI | MOD_RGUI,
    .symbol_dsc = &win_icon,
};

struct modifier_symbol *modifier_symbols[] = {
    // this order determines the order of the symbols
    &ms_win,
    &ms_alt,
    &ms_control,
    &ms_shift
};
#endif

#define NUM_SYMBOLS (sizeof(modifier_symbols) / sizeof(struct modifier_symbol *))

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static void anim_y_cb(void *var, int32_t v) {
    lv_obj_set_y(var, v);
}

static void move_object_y(void *obj, int32_t from, int32_t to) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_values(&a, from, to);
    lv_anim_start(&a);
}

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_CAPSWORD)
// Two-state shift-symbol indicator:
//   off                      -> shift_icon (frame/outline)
//   caps-word OR caps-lock    -> shift_filled_icon (full)
// TOTEM's shift symbol is a 14x14 image (not a font glyph), and a per-state box
// was tried but its border/padding displaced the symbol and misaligned the mod
// row — so caps-word and caps-lock share the filled arrow (no distinction).
// Independent of the held-shift up/down + underline logic in set_modifiers(),
// so normal shift indication is unaffected. Idempotent: only redraws on change.
//
// Threading: neither caps-word activation nor host caps-lock changes are a
// keycode event the modifiers listener hears, so we poll on a k_timer. The
// timer callback runs in system-timer context and LVGL is single-threaded, so
// it only SUBMITS to ZMK's display work queue; the lv_* calls run there.

// HID keyboard LED report bit for caps-lock (USB HID spec; matches LED_CLCK in
// the hid_indicators widget). Read on the central via the host LED state.
#define LED_CAPS_LOCK 0x02

static inline bool caps_lock_active(void) {
    return (zmk_hid_indicators_get_current_profile() & LED_CAPS_LOCK) != 0;
}

// Two states: frame (off) / filled (caps-word OR caps-lock). TOTEM's shift
// symbol is a 14x14 image, not a font glyph, so there is no caps-lock-specific
// arrow; both caps modes show the filled arrow. (A box was tried but its
// border/padding displaced the 14px symbol and misaligned the row, so it was
// dropped.)
static bool caps_shown = false;

static void caps_word_work_cb(struct k_work *work) {
    bool fill = caps_word_ind_is_active() || caps_lock_active();
    if (fill == caps_shown) {
        return;
    }
    caps_shown = fill;

    const lv_img_dsc_t *src = fill ? &shift_filled_icon : &shift_icon;
    ms_shift.symbol_dsc = src;
    if (ms_shift.symbol != NULL) {
        lv_img_set_src(ms_shift.symbol, src);
    }
}

static K_WORK_DEFINE(caps_word_work, caps_word_work_cb);

static void caps_word_timer_cb(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_display_work_q(), &caps_word_work);
}

static K_TIMER_DEFINE(caps_word_timer, caps_word_timer_cb, NULL);
#endif

static void set_modifiers(lv_obj_t *widget, struct modifiers_state state) {
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        bool mod_is_active = state.modifiers & modifier_symbols[i]->modifier;

        if (mod_is_active && !modifier_symbols[i]->is_active) {
            move_object_y(modifier_symbols[i]->symbol, 1, 0);
            move_object_y(modifier_symbols[i]->selection_line, SIZE_SYMBOLS + 4, SIZE_SYMBOLS + 2);
            modifier_symbols[i]->is_active = true;
        } else if (!mod_is_active && modifier_symbols[i]->is_active) {
            move_object_y(modifier_symbols[i]->symbol, 0, 1);
            move_object_y(modifier_symbols[i]->selection_line, SIZE_SYMBOLS + 2, SIZE_SYMBOLS + 4);
            modifier_symbols[i]->is_active = false;
        }
    }
}

void modifiers_update_cb(struct modifiers_state state) {
    struct zmk_widget_modifiers *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_modifiers(widget->obj, state); }
}

static struct modifiers_state modifiers_get_state(const zmk_event_t *eh) {
    return (struct modifiers_state) {
        .modifiers = zmk_hid_get_explicit_mods()
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_modifiers, struct modifiers_state,
                            modifiers_update_cb, modifiers_get_state)

ZMK_SUBSCRIPTION(widget_modifiers, zmk_keycode_state_changed);

int zmk_widget_modifiers_init(struct zmk_widget_modifiers *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);

    lv_obj_set_size(widget->obj, NUM_SYMBOLS * (SIZE_SYMBOLS + 1) + 1, SIZE_SYMBOLS + 3);
    
    static lv_style_t style_line;
    lv_style_init(&style_line);
    lv_style_set_line_width(&style_line, 2);

    static const lv_point_precise_t selection_line_points[] = { {0, 0}, {SIZE_SYMBOLS, 0} };

    for (int i = 0; i < NUM_SYMBOLS; i++) {
        modifier_symbols[i]->symbol = lv_img_create(widget->obj);
        lv_obj_align(modifier_symbols[i]->symbol, LV_ALIGN_TOP_LEFT, 1 + (SIZE_SYMBOLS + 1) * i, 1);
        lv_img_set_src(modifier_symbols[i]->symbol, modifier_symbols[i]->symbol_dsc);

        modifier_symbols[i]->selection_line = lv_line_create(widget->obj);
        lv_line_set_points(modifier_symbols[i]->selection_line, selection_line_points, 2);
        lv_obj_add_style(modifier_symbols[i]->selection_line, &style_line, 0);
        lv_obj_align_to(modifier_symbols[i]->selection_line, modifier_symbols[i]->symbol, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 3);
    }

    sys_slist_append(&widgets, &widget->node);

    widget_modifiers_init();

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_CAPSWORD)
    // Poll every 100ms to drive the frame<->filled shift indicator (caps-word
    // or caps-lock -> filled). No box styling here: it displaced the symbol.
    k_timer_start(&caps_word_timer, K_MSEC(100), K_MSEC(100));
#endif

    return 0;
}

lv_obj_t *zmk_widget_modifiers_obj(struct zmk_widget_modifiers *widget) {
    return widget->obj;
}
