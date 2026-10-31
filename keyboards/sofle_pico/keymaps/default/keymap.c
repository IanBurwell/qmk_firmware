// Copyright 2024 Ryan Neff (@JellyTitan)
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H
#include "transactions.h"

#define USR_BTN_PIN GP24
#define USR_LED_PIN GP25

static bool lights_disabled = false;

static void usr_btn_sync_slave_handler(uint8_t in_buflen, const void *in_data, uint8_t out_buflen, void *out_data) {
    if (in_buflen >= sizeof(bool)) {
        bool prev_disabled = lights_disabled;
        lights_disabled    = *(const bool *)in_data;
        if (prev_disabled && !lights_disabled) {
            oled_on();
        }
    }
    if (out_buflen >= sizeof(bool)) {
        *(bool *)out_data = !gpio_read_pin(USR_BTN_PIN);
    }
}

void keyboard_post_init_user(void) {
    gpio_set_pin_output(USR_LED_PIN);
    gpio_set_pin_input_high(USR_BTN_PIN);
    transaction_register_rpc(USR_BTN_SYNC, usr_btn_sync_slave_handler);
}

bool oled_task_user(void) {
    if (lights_disabled) {
        oled_off();
        return false;
    }
    return true;
}

void housekeeping_task_user(void) {
    if (is_keyboard_master()) {
        // Solid LED on master half
        gpio_write_pin_high(USR_LED_PIN);

        static uint16_t poll_timer      = 0;
        static bool     last_master_btn = false;
        static bool     last_slave_btn  = false;

        if (timer_elapsed(poll_timer) >= 20) {
            poll_timer = timer_read();

            bool master_btn = !gpio_read_pin(USR_BTN_PIN);
            bool slave_btn  = false;

            transaction_rpc_exec(USR_BTN_SYNC, sizeof(lights_disabled), &lights_disabled, sizeof(slave_btn), &slave_btn);

            // Master button: toggle OLEDs and RGB LEDs on/off
            if (master_btn && !last_master_btn) {
                lights_disabled = !lights_disabled;
                if (lights_disabled) {
                    rgb_matrix_disable_noeeprom();
                    oled_off();
                } else {
                    rgb_matrix_enable_noeeprom();
                    oled_on();
                }
            }
            last_master_btn = master_btn;

            // Non-master button: cycle RGB Matrix profile
            if (slave_btn && !last_slave_btn) {
                rgb_matrix_step();
            }
            last_slave_btn = slave_btn;
        }
    } else {
        // Caps Lock indicator on non-master half
        gpio_write_pin(USR_LED_PIN, host_keyboard_led_state().caps_lock);
    }

    if (lights_disabled && is_oled_on()) {
        oled_off();
    }

}

enum sofle_layers {
    _QWERTY,
    _LOWER,
    _RAISE,
    _ADJUST,
};

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    // clang-format off
        [_QWERTY] = LAYOUT(
        KC_GRV,   KC_1,   KC_2,    KC_3,    KC_4,    KC_5,                          KC_6,    KC_7,    KC_8,    KC_9,   KC_0,    KC_GRV,
        KC_TAB,   KC_Q,   KC_W,    KC_E,    KC_R,    KC_T,                          KC_Y,    KC_U,    KC_I,    KC_O,   KC_P,    KC_BSPC,
        KC_ESC,   KC_A,   KC_S,    KC_D,    KC_F,    KC_G,                          KC_H,    KC_J,    KC_K,    KC_L,   KC_SCLN, KC_QUOT,
        KC_LSFT,  KC_Z,   KC_X,    KC_C,    KC_V,    KC_B,    KC_MUTE,     KC_MPLY, KC_N,    KC_M,    KC_COMM, KC_DOT, KC_SLSH, KC_RSFT,
                          KC_LGUI, KC_LALT, KC_LCTL, TL_LOWR, KC_ENT,      KC_SPC , TL_UPPR, KC_RCTL, KC_RALT, KC_RGUI
        ),
        [_LOWER] = LAYOUT(
        _______, KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,                           KC_F6,   KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,
        KC_GRV,  KC_1,    KC_2,    KC_3,    KC_4,    KC_5,                            KC_6,    KC_7,    KC_8,    KC_9,    KC_0,    KC_F12,
        _______, KC_EXLM, KC_AT,   KC_HASH, KC_DLR,  KC_PERC,                         KC_CIRC, KC_AMPR, KC_ASTR, KC_LPRN, KC_RPRN, KC_PIPE,
        _______, KC_EQL,  KC_MINS, KC_PLUS, KC_LCBR, KC_RCBR, _______,       _______, KC_LBRC, KC_RBRC, KC_SCLN, KC_COLN, KC_BSLS, _______,
                          _______, _______, _______, _______, _______,       _______, _______, _______, _______, _______
        ),
        [_RAISE] = LAYOUT(
        _______, _______, _______, _______, _______, _______,                          _______, _______, _______, _______, _______, _______,
        _______, KC_INS,  KC_PSCR, KC_APP,  XXXXXXX, XXXXXXX,                          KC_PGUP, XXXXXXX, KC_UP  , XXXXXXX, XXXXXXX, KC_BSPC,
        _______, KC_LALT, KC_LCTL, KC_LSFT, XXXXXXX, KC_CAPS,                          KC_PGDN, KC_LEFT, KC_DOWN, KC_RGHT, KC_DEL , KC_BSPC,
        _______, C(KC_Z), C(KC_X), C(KC_C), C(KC_V), XXXXXXX, _______,        _______, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, _______,
                          _______, _______, _______, _______, _______,        _______, _______, _______, _______, _______
        ),
        [_ADJUST] = LAYOUT(
        XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX,                       RM_TOGG, RM_VALU, RM_VALD, XXXXXXX, XXXXXXX, XXXXXXX,
        QK_BOOT, XXXXXXX, XXXXXXX, XXXXXXX, CG_TOGG, XXXXXXX,                       RM_NEXT, RM_PREV, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX,
        XXXXXXX, XXXXXXX, CG_TOGG, XXXXXXX, XXXXXXX, XXXXXXX,                       XXXXXXX, KC_VOLD, KC_MUTE, KC_VOLU, XXXXXXX, XXXXXXX,
        XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX,     XXXXXXX, XXXXXXX, KC_MPRV, KC_MPLY, KC_MNXT, XXXXXXX, XXXXXXX,
                          _______, _______, _______, _______, _______,     _______, _______, _______, _______, _______
        )
    // clang-format on
};

const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [_QWERTY] = {ENCODER_CCW_CW(KC_VOLD, KC_VOLU), ENCODER_CCW_CW(KC_MPRV, KC_MNXT)},
    [_LOWER]  = {ENCODER_CCW_CW(RM_VALD, RM_VALU), ENCODER_CCW_CW(RM_HUED, RM_HUEU)},
    [_RAISE]  = {ENCODER_CCW_CW(RM_SPDD, RM_SPDU), ENCODER_CCW_CW(RM_PREV, RM_NEXT)},
    [_ADJUST] = {ENCODER_CCW_CW(_______, _______), ENCODER_CCW_CW(_______, _______)},
};
