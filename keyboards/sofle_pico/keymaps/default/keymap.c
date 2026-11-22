// Copyright 2024 Ryan Neff (@JellyTitan)
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H
#include "transactions.h"
#include "keymap_introspection.h"
#include "split_util.h"
#include "sync_timer.h"
#include <string.h>

#ifdef VIA_ENABLE
#    include "via.h"
#    include "dynamic_keymap.h"
#endif

#ifdef RAW_ENABLE
#    include "raw_hid.h"
#    ifndef RAW_EPSIZE
#        define RAW_EPSIZE 32
#    endif
#endif

#ifdef RGB_MATRIX_ENABLE
#    include "rgb_matrix.h"
#endif

#define USR_BTN_PIN GP24
#define USR_LED_PIN GP25

enum sofle_layers {
    _QWERTY,
    _LOWER,
    _RAISE,
    _ADJUST,
};

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    // clang-format off
    [_QWERTY] = LAYOUT(
        KC_GRV,   KC_1,     KC_2,     KC_3,     KC_4,     KC_5,                               KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     XXXXXXX,
        KC_TAB,   KC_Q,     KC_W,     KC_E,     KC_R,     KC_T,                               KC_Y,     KC_U,     KC_I,     KC_O,     KC_P,     KC_BSPC,
        KC_ESC,   KC_A,     KC_S,     KC_D,     KC_F,     KC_G,                               KC_H,     KC_J,     KC_K,     KC_L,     KC_SCLN,  KC_QUOT,
        KC_LSFT,  KC_Z,     KC_X,     KC_C,     KC_V,     KC_B,     KC_MPLY,        KC_MPLY,  KC_N,     KC_M,     KC_COMM,  KC_DOT,   KC_SLSH,  KC_RSFT,
                            KC_LGUI,  KC_LALT,  KC_LCTL,  TL_LOWR,  KC_SPC,         KC_ENT,   TL_UPPR,  KC_RCTL,  KC_RALT,  KC_RGUI
    ),
    [_LOWER] = LAYOUT(
        _______,  KC_F1,    KC_F2,    KC_F3,    KC_F4,    KC_F5,                              KC_F6,    KC_F7,    KC_F8,    KC_F9,    KC_F10,   KC_F11,
        KC_GRV,   KC_1,     KC_2,     KC_3,     KC_4,     KC_5,                               KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     KC_F12,
        _______,  KC_EXLM,  KC_AT,    KC_HASH,  KC_DLR,   KC_PERC,                            KC_CIRC,  KC_AMPR,  KC_ASTR,  KC_LPRN,  KC_RPRN,  KC_PIPE,
        _______,  KC_UNDS,  KC_MINS,  KC_PLUS,  KC_EQL,   XXXXXXX,  _______,        _______,  KC_LCBR,  KC_RCBR,  KC_LBRC,  KC_RBRC,  KC_BSLS,  _______,
                            _______,  _______,  _______,  _______,  _______,        _______,  _______,  _______,  _______,  _______
    ),
    [_RAISE] = LAYOUT(
        _______,  _______,  _______,  _______,  _______,  _______,                            _______,  _______,  _______,  _______,  _______,  _______,
        _______,  KC_INS,   KC_PSCR,  KC_APP,   KC_CAPS,  A(KC_F4),                           KC_PGUP,  C(KC_LEFT), KC_UP,  C(KC_RGHT), XXXXXXX, C(KC_BSPC),
        _______,  KC_LALT,  KC_LCTL,  KC_LSFT,  XXXXXXX,  KC_CALC,                            KC_PGDN,  KC_LEFT,  KC_DOWN,  KC_RGHT,  KC_BSPC,  KC_DEL,
        _______,  C(KC_Z),  C(KC_X),  C(KC_C),  C(KC_V),  XXXXXXX,  _______,        _______,  XXXXXXX,  KC_MPRV,  KC_MPLY,  KC_MNXT,  XXXXXXX,  _______,
                            _______,  _______,  _______,  _______,  _______,        _______,  _______,  _______,  _______,  _______
    ),
    [_ADJUST] = LAYOUT(
        QK_BOOT,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,                            RM_HUED,  RM_HUEU,  RM_SATD,  RM_SATU,  XXXXXXX,  XXXXXXX,
        XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,                            RM_NEXT,  RM_VALD,  RM_VALU,  RM_TOGG,  RM_SPDU,  XXXXXXX,
        XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,                            RM_PREV,  KC_VOLD,  KC_MUTE,  KC_VOLU,  RM_SPDD,  XXXXXXX,
        XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,        XXXXXXX,  RM_TOGG,  _______,  _______,  _______,  XXXXXXX,  XXXXXXX,
                            _______,  _______,  _______,  _______,  _______,        _______,  _______,  _______,  _______,  _______
    )
    // clang-format on
};

const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [_QWERTY] = {ENCODER_CCW_CW(MS_WHLU, MS_WHLD), ENCODER_CCW_CW(KC_VOLU, KC_VOLD)},
    [_LOWER]  = {ENCODER_CCW_CW(RM_VALD, RM_VALU), ENCODER_CCW_CW(RM_HUED, RM_HUEU)},
    [_RAISE]  = {ENCODER_CCW_CW(RM_SPDD, RM_SPDU), ENCODER_CCW_CW(RM_PREV, RM_NEXT)},
    [_ADJUST] = {ENCODER_CCW_CW(_______, _______), ENCODER_CCW_CW(_______, _______)},
};

#define SLAVE_HDR_LIGHTS_OFF  0x01
#define SLAVE_HDR_CAPS_LOCK   0x02
#define SLAVE_HDR_WAKE_ACTIVE 0x04

typedef struct {
    uint8_t flags;
    uint8_t top_layer;
    uint8_t mods;
    uint8_t reserved;
} slave_sync_hdr_t;

typedef struct {
    slave_sync_hdr_t hdr;
    uint16_t         keys[5][6];
    uint16_t         enc_ccw;
    uint16_t         enc_cw;
} slave_sync_packet_t;

_Static_assert(sizeof(slave_sync_packet_t) <= RPC_M2S_BUFFER_SIZE, "slave_sync_packet_t exceeds RPC_M2S_BUFFER_SIZE");

static bool             lights_disabled  = false;
static slave_sync_hdr_t slave_hdr_state  = {0, 0, 0, 0};
static uint16_t         slave_keys[5][6] = {{0}};
static uint16_t         slave_enc_ccw    = KC_NO;
static uint16_t         slave_enc_cw     = KC_NO;
static bool             slave_has_hdr    = false;
static bool             slave_has_keys   = false;

static uint16_t resolve_active_keycode(layer_state_t active_mask, uint8_t top_layer, uint8_t row, uint8_t col) {
    uint8_t max_layers = keymap_layer_count();
    if (top_layer >= max_layers) {
        top_layer = (uint8_t)(max_layers - 1);
    }
    for (int8_t l = (int8_t)top_layer; l >= 0; l--) {
        if ((active_mask & ((layer_state_t)1 << l)) || l == 0) {
            uint16_t kc = keycode_at_keymap_location((uint8_t)l, row, col);
            if (kc != KC_TRNS) {
                return kc;
            }
        }
    }
    return KC_NO;
}

static uint16_t resolve_active_encoder(layer_state_t active_mask, uint8_t top_layer, uint8_t enc_idx, bool clockwise) {
#if defined(ENCODER_ENABLE) && defined(ENCODER_MAP_ENABLE)
    uint8_t max_layers = encodermap_layer_count();
    if (top_layer >= max_layers) {
        top_layer = (uint8_t)(max_layers - 1);
    }
    for (int8_t l = (int8_t)top_layer; l >= 0; l--) {
        if ((active_mask & ((layer_state_t)1 << l)) || l == 0) {
            uint16_t kc = keycode_at_encodermap_location((uint8_t)l, enc_idx, clockwise);
            if (kc != KC_TRNS) {
                return kc;
            }
        }
    }
#endif
    return KC_NO;
}

static void populate_half_keys(bool is_left, layer_state_t active_mask, uint8_t top_layer, uint16_t out_keys[5][6], uint16_t *out_ccw, uint16_t *out_cw) {
    uint8_t row_offset = is_left ? 0 : 5;
    for (uint8_t r = 0; r < 5; r++) {
        for (uint8_t c = 0; c < 6; c++) {
            out_keys[r][c] = resolve_active_keycode(active_mask, top_layer, (uint8_t)(r + row_offset), c);
        }
    }
    uint8_t enc_idx = is_left ? 0 : 1;
    // Right half PCB is flipped, reversing physical encoder A/B rotation relative to QMK CW/CCW
    *out_ccw        = resolve_active_encoder(active_mask, top_layer, enc_idx, !is_left);
    *out_cw         = resolve_active_encoder(active_mask, top_layer, enc_idx, is_left);
}

#if defined(VIA_ENABLE) && (VIA_EEPROM_CUSTOM_CONFIG_SIZE >= 4)
static uint32_t compute_flash_keymap_hash(void) {
    uint32_t hash   = 2166136261UL;
    uint8_t  layers = keymap_layer_count_raw();
    for (uint8_t l = 0; l < layers; l++) {
        for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                uint16_t kc = keycode_at_keymap_location_raw(l, r, c);
                hash ^= (uint8_t)(kc & 0xFF);
                hash *= 16777619UL;
                hash ^= (uint8_t)(kc >> 8);
                hash *= 16777619UL;
            }
        }
#    if defined(ENCODER_ENABLE) && defined(ENCODER_MAP_ENABLE)
        for (uint8_t e = 0; e < NUM_ENCODERS; e++) {
            for (uint8_t d = 0; d < 2; d++) {
                uint16_t kc = keycode_at_encodermap_location_raw(l, e, d != 0);
                hash ^= (uint8_t)(kc & 0xFF);
                hash *= 16777619UL;
                hash ^= (uint8_t)(kc >> 8);
                hash *= 16777619UL;
            }
        }
#    endif
    }
    return hash;
}
#endif

static void wake_local_displays_and_rgb(void) {
    set_activity_timestamps(sync_timer_read32(), last_encoder_activity_time(), last_pointing_device_activity_time());
    oled_on();
#ifdef RGB_MATRIX_ENABLE
    rgb_matrix_set_suspend_state(false);
#endif
}

static void usr_btn_sync_slave_handler(uint8_t in_buflen, const void *in_data, uint8_t out_buflen, void *out_data) {
    if (in_buflen >= sizeof(slave_sync_hdr_t)) {
        slave_sync_hdr_t hdr;
        memcpy(&hdr, in_data, sizeof(hdr));
        bool prev_disabled = lights_disabled;
        lights_disabled    = (hdr.flags & SLAVE_HDR_LIGHTS_OFF) != 0;
        slave_hdr_state    = hdr;
        slave_has_hdr      = true;
        if (!lights_disabled && (prev_disabled || (hdr.flags & SLAVE_HDR_WAKE_ACTIVE) != 0)) {
            wake_local_displays_and_rgb();
        }
    }
    if (in_buflen >= sizeof(slave_sync_packet_t)) {
        slave_sync_packet_t pkt;
        memcpy(&pkt, in_data, sizeof(pkt));
        memcpy(slave_keys, pkt.keys, sizeof(slave_keys));
        slave_enc_ccw  = pkt.enc_ccw;
        slave_enc_cw   = pkt.enc_cw;
        slave_has_keys = true;
    }
    if (out_buflen >= sizeof(bool)) {
        bool btn = !gpio_read_pin(USR_BTN_PIN);
        memcpy(out_data, &btn, sizeof(btn));
    }
}

typedef enum {
    OLED_CONN_TRRS = 0,
    OLED_CONN_SYNC,
    OLED_CONN_WAIT,
    OLED_CONN_DISC,
} oled_conn_mode_t;

typedef struct {
    bool    valid;
    bool    lights_disabled;
    uint8_t enable;
    uint8_t mode;
    uint8_t h;
    uint8_t s;
    uint8_t v;
    uint8_t speed;
} rgb_sync_snapshot_t;

#define RAW_SYNC_CMD_ID           0xF0
#define RAW_SYNC_MAGIC_0          0x53 // 'S'
#define RAW_SYNC_MAGIC_1          0x50 // 'P'
#define RAW_MSG_HOST_PING         0x01
#define RAW_MSG_HALF_STATE        0x02
#define RAW_MSG_IDENTIFY          0x03
#define RAW_MSG_BOOTLOADER        0x04
#define RAW_BOOTLOADER_DELAY_MS   60
#define HOST_SYNC_TIMEOUT_MS      2500
#define ACTIVITY_WAKE_THROTTLE_MS 1000

#define HALF_FLAG_LOWER_HELD    0x01
#define HALF_FLAG_UPPER_HELD    0x02
#define HALF_FLAG_LIGHTS_OFF    0x04
#define HALF_FLAG_RGB_CHANGED   0x08
#define HALF_FLAG_SYNC_TIMER    0x10
#define HALF_FLAG_PING_REPLY    0x20
#define HALF_FLAG_ACTIVITY_WAKE 0x40

static bool                host_service_connected   = false;
static bool                host_peer_connected      = false;
static uint32_t            host_service_last_rx     = 0;
static bool                bootloader_pending       = false;
static uint32_t            bootloader_req_time      = 0;
static uint32_t            last_activity_sync_tx    = 0;
static bool                last_activity_sync_valid = false;
static bool                local_lower_held         = false;
static bool                local_upper_held         = false;
static bool                remote_lower_held        = false;
static bool                remote_upper_held        = false;
static uint8_t             remote_mods              = 0;
static uint8_t             last_sent_mods           = 0;
static uint8_t             local_rgb_seq            = 0;
static uint8_t             local_state_seq          = 0;
static rgb_sync_snapshot_t last_rgb_snapshot        = {0};

static uint8_t get_local_mods(void) {
    uint8_t mods = get_mods() | get_weak_mods();
#ifndef NO_ACTION_ONESHOT
    mods |= get_oneshot_mods();
#endif
    return mods;
}

static void snapshot_current_rgb(rgb_sync_snapshot_t *out) {
    out->valid           = true;
    out->lights_disabled = lights_disabled;
#ifdef RGB_MATRIX_ENABLE
    hsv_t hsv   = rgb_matrix_get_hsv();
    out->enable = rgb_matrix_is_enabled() ? 1 : 0;
    out->mode   = rgb_matrix_get_mode();
    out->h      = hsv.h;
    out->s      = hsv.s;
    out->v      = hsv.v;
    out->speed  = rgb_matrix_get_speed();
#else
    out->enable = 0;
    out->mode   = 0;
    out->h      = 0;
    out->s      = 0;
    out->v      = 0;
    out->speed  = 0;
#endif
}

static void apply_combined_tri_layers(void) {
    bool want_lower = local_lower_held || remote_lower_held;
    bool want_upper = local_upper_held || remote_upper_held;
    bool changed    = false;

    if (want_lower && !layer_state_is(_LOWER)) {
        layer_on(_LOWER);
        changed = true;
    } else if (!want_lower && layer_state_is(_LOWER)) {
        layer_off(_LOWER);
        changed = true;
    }

    if (want_upper && !layer_state_is(_RAISE)) {
        layer_on(_RAISE);
        changed = true;
    } else if (!want_upper && layer_state_is(_RAISE)) {
        layer_off(_RAISE);
        changed = true;
    }

    bool want_adjust = want_lower && want_upper;
    if (changed || (layer_state_is(_ADJUST) != want_adjust)) {
        update_tri_layer(_LOWER, _RAISE, _ADJUST);
    }
}

static void host_sync_send_state(bool rgb_changed, bool sync_rgb_timer, bool is_reply, bool activity_wake) {
#ifdef RAW_ENABLE
    if (!is_keyboard_master() || is_transport_connected() || !host_service_connected) {
        return;
    }
    if (!is_reply && !host_peer_connected) {
        return;
    }

    uint8_t pkt[RAW_EPSIZE];
    memset(pkt, 0, sizeof(pkt));

    rgb_sync_snapshot_t cur_rgb;
    snapshot_current_rgb(&cur_rgb);

    uint8_t flags = 0;
    if (local_lower_held) flags |= HALF_FLAG_LOWER_HELD;
    if (local_upper_held) flags |= HALF_FLAG_UPPER_HELD;
    if (cur_rgb.lights_disabled) flags |= HALF_FLAG_LIGHTS_OFF;
    if (rgb_changed) {
        flags |= HALF_FLAG_RGB_CHANGED;
        local_rgb_seq++;
    }
    if (sync_rgb_timer) flags |= HALF_FLAG_SYNC_TIMER;
    if (is_reply) flags |= HALF_FLAG_PING_REPLY;
    if (activity_wake) {
        flags |= HALF_FLAG_ACTIVITY_WAKE;
        last_activity_sync_tx    = timer_read32();
        last_activity_sync_valid = true;
    }

    local_state_seq++;

    pkt[0]  = RAW_SYNC_CMD_ID;
    pkt[1]  = RAW_SYNC_MAGIC_0;
    pkt[2]  = RAW_SYNC_MAGIC_1;
    pkt[3]  = RAW_MSG_HALF_STATE;
    pkt[4]  = is_keyboard_left() ? 1 : 0;
    pkt[5]  = flags;
    pkt[6]  = get_local_mods();
    pkt[7]  = cur_rgb.enable;
    pkt[8]  = cur_rgb.mode;
    pkt[9]  = cur_rgb.h;
    pkt[10] = cur_rgb.s;
    pkt[11] = cur_rgb.v;
    pkt[12] = cur_rgb.speed;
    pkt[13] = local_rgb_seq;
    pkt[14] = local_state_seq;

    raw_hid_send(pkt, sizeof(pkt));
#else
    (void)rgb_changed;
    (void)sync_rgb_timer;
    (void)is_reply;
    (void)activity_wake;
#endif
}

static void clear_host_sync_state(void) {
    bool had_remote = host_service_connected || host_peer_connected || remote_lower_held || remote_upper_held || (remote_mods != 0);
    host_service_connected   = false;
    host_peer_connected      = false;
    last_activity_sync_valid = false;
    remote_lower_held        = false;
    remote_upper_held        = false;
    remote_mods              = 0;
    if (had_remote) {
        apply_combined_tri_layers();
    }
}

#if defined(VIA_ENABLE) && defined(RAW_ENABLE)
bool via_command_kb(uint8_t *data, uint8_t length) {
    if (length < 15 || data[0] != RAW_SYNC_CMD_ID || data[1] != RAW_SYNC_MAGIC_0 || data[2] != RAW_SYNC_MAGIC_1) {
        return false;
    }

    if (!is_keyboard_master()) {
        return true;
    }

    uint8_t msg_type = data[3];
    if (msg_type == RAW_MSG_IDENTIFY) {
        uint8_t pkt[RAW_EPSIZE];
        memset(pkt, 0, sizeof(pkt));
        uint32_t up = timer_read32();
        pkt[0]      = RAW_SYNC_CMD_ID;
        pkt[1]      = RAW_SYNC_MAGIC_0;
        pkt[2]      = RAW_SYNC_MAGIC_1;
        pkt[3]      = RAW_MSG_IDENTIFY;
        pkt[4]      = is_keyboard_left() ? 1 : 0;
        pkt[5]      = is_transport_connected() ? 1 : 0;
        pkt[6]      = data[4];
        pkt[7]      = data[5];
        pkt[8]      = (uint8_t)(up & 0xFF);
        pkt[9]      = (uint8_t)((up >> 8) & 0xFF);
        pkt[10]     = (uint8_t)((up >> 16) & 0xFF);
        pkt[11]     = (uint8_t)((up >> 24) & 0xFF);
        raw_hid_send(pkt, sizeof(pkt));
        return true;
    }

    if (msg_type == RAW_MSG_BOOTLOADER) {
        uint8_t my_side  = is_keyboard_left() ? 1 : 0;
        bool    magic_ok = (data[5] == 'B' && data[6] == 'O' && data[7] == 'O' && data[8] == 'T');
        bool    side_ok  = (data[4] == 2 || data[4] == my_side);
        bool    accepted = magic_ok && side_ok;

        uint8_t pkt[RAW_EPSIZE];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = RAW_SYNC_CMD_ID;
        pkt[1] = RAW_SYNC_MAGIC_0;
        pkt[2] = RAW_SYNC_MAGIC_1;
        pkt[3] = RAW_MSG_BOOTLOADER;
        pkt[4] = my_side;
        pkt[5] = accepted ? 1 : 0;
        pkt[6] = data[9];
        pkt[7] = data[10];
        raw_hid_send(pkt, sizeof(pkt));

        if (accepted) {
            bootloader_pending  = true;
            bootloader_req_time = timer_read32();
        }
        return true;
    }

    if (is_transport_connected()) {
        return true;
    }

    if (msg_type == RAW_MSG_HOST_PING) {
        host_service_last_rx   = timer_read32();
        host_service_connected = true;
        bool new_peer          = (data[4] != 0);
        if (host_peer_connected && !new_peer) {
            remote_lower_held = false;
            remote_upper_held = false;
            remote_mods       = 0;
            apply_combined_tri_layers();
        }
        host_peer_connected = new_peer;

        if (data[5] != 0) {
            bool auth_rgb = (data[6] != 0);
            last_sent_mods = get_local_mods();
            snapshot_current_rgb(&last_rgb_snapshot);
            host_sync_send_state(auth_rgb, auth_rgb, true, false);
        }
        return true;
    }

    if (msg_type == RAW_MSG_HALF_STATE) {
        bool sender_is_left = (data[4] != 0);
        if (sender_is_left == is_keyboard_left()) {
            return true;
        }

        host_service_last_rx   = timer_read32();
        host_service_connected = true;
        host_peer_connected    = true;

        uint8_t flags     = data[5];
        remote_lower_held = (flags & HALF_FLAG_LOWER_HELD) != 0;
        remote_upper_held = (flags & HALF_FLAG_UPPER_HELD) != 0;
        remote_mods       = data[6];
        apply_combined_tri_layers();

        if ((flags & HALF_FLAG_RGB_CHANGED) != 0) {
            bool prev_lights_disabled = lights_disabled;
            bool new_lights_disabled  = (flags & HALF_FLAG_LIGHTS_OFF) != 0;
            bool new_enable           = (data[7] != 0);
            uint8_t new_mode          = data[8];
            uint8_t new_hue           = data[9];
            uint8_t new_sat           = data[10];
            uint8_t new_val           = data[11];
            uint8_t new_speed         = data[12];

            lights_disabled = new_lights_disabled;
            if (new_lights_disabled) {
                oled_off();
#    ifdef RGB_MATRIX_ENABLE
                rgb_matrix_disable_noeeprom();
#    endif
            } else {
                if (prev_lights_disabled) {
                    wake_local_displays_and_rgb();
                }
#    ifdef RGB_MATRIX_ENABLE
                if (new_enable) {
                    if (!rgb_matrix_is_enabled()) {
                        if (prev_lights_disabled) {
                            rgb_matrix_enable_noeeprom();
                        } else {
                            rgb_matrix_enable();
                        }
                    }
                    if (new_mode < 1 || new_mode >= RGB_MATRIX_EFFECT_MAX) {
                        new_mode = RGB_MATRIX_SOLID_COLOR;
                    }
                    if (rgb_matrix_get_mode() != new_mode) {
                        rgb_matrix_mode(new_mode);
                    }
                    hsv_t cur_hsv = rgb_matrix_get_hsv();
                    if (cur_hsv.h != new_hue || cur_hsv.s != new_sat || cur_hsv.v != new_val) {
                        rgb_matrix_sethsv(new_hue, new_sat, new_val);
                    }
                    if (rgb_matrix_get_speed() != new_speed) {
                        rgb_matrix_set_speed(new_speed);
                    }
                    if ((flags & HALF_FLAG_SYNC_TIMER) != 0) {
                        g_rgb_timer = 0;
                    }
                } else if (rgb_matrix_is_enabled()) {
                    rgb_matrix_disable();
                }
#    endif
            }
            snapshot_current_rgb(&last_rgb_snapshot);
        }

        if ((flags & HALF_FLAG_ACTIVITY_WAKE) != 0 && !lights_disabled) {
            wake_local_displays_and_rgb();
        }
        return true;
    }

    return true;
}
#endif

static void sync_local_state_if_needed(void) {
    if (!is_keyboard_master() || is_transport_connected()) {
        return;
    }
    uint8_t             cur_mods = get_local_mods();
    rgb_sync_snapshot_t cur_rgb;
    snapshot_current_rgb(&cur_rgb);

    bool mods_changed = (cur_mods != last_sent_mods);
    bool mode_changed = !last_rgb_snapshot.valid || (cur_rgb.mode != last_rgb_snapshot.mode) || (cur_rgb.enable != last_rgb_snapshot.enable) || (cur_rgb.lights_disabled != last_rgb_snapshot.lights_disabled);
    bool rgb_changed  = !last_rgb_snapshot.valid || (memcmp(&cur_rgb, &last_rgb_snapshot, sizeof(cur_rgb)) != 0);

    if (mods_changed || rgb_changed) {
        last_sent_mods    = cur_mods;
        last_rgb_snapshot = cur_rgb;
        host_sync_send_state(rgb_changed, mode_changed, false, true);
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (is_keyboard_master() && !is_transport_connected()) {
        if (keycode == QK_TRI_LAYER_LOWER) {
            local_lower_held = record->event.pressed;
            apply_combined_tri_layers();
            last_sent_mods = get_local_mods();
            host_sync_send_state(false, false, false, record->event.pressed);
            return false;
        }
        if (keycode == QK_TRI_LAYER_UPPER) {
            local_upper_held = record->event.pressed;
            apply_combined_tri_layers();
            last_sent_mods = get_local_mods();
            host_sync_send_state(false, false, false, record->event.pressed);
            return false;
        }
        if (record->event.pressed && host_service_connected && host_peer_connected) {
            if (!last_activity_sync_valid || timer_elapsed32(last_activity_sync_tx) >= ACTIVITY_WAKE_THROTTLE_MS) {
                last_sent_mods = get_local_mods();
                host_sync_send_state(false, false, false, true);
            }
        }
    }
    return true;
}

void post_process_record_user(uint16_t keycode, keyrecord_t *record) {
    (void)keycode;
    (void)record;
    sync_local_state_if_needed();
}

void keyboard_post_init_user(void) {
    gpio_set_pin_output(USR_LED_PIN);
    gpio_set_pin_input_high(USR_BTN_PIN);
    transaction_register_rpc(USR_BTN_SYNC, usr_btn_sync_slave_handler);

#if defined(RGB_MATRIX_ENABLE)
    if (is_keyboard_master()) {
        if (!rgb_matrix_config.mode || rgb_matrix_config.mode >= RGB_MATRIX_EFFECT_MAX || rgb_matrix_config.flags == 0) {
            eeconfig_update_rgb_matrix_default();
        }
    }
#endif

#if defined(VIA_ENABLE) && (VIA_EEPROM_CUSTOM_CONFIG_SIZE >= 4)
    if (is_keyboard_master()) {
        uint32_t flash_hash  = compute_flash_keymap_hash();
        uint32_t stored_hash = 0;
        via_read_custom_config(&stored_hash, 0, sizeof(stored_hash));
        if (stored_hash != flash_hash) {
            dynamic_keymap_reset();
            via_update_custom_config(&flash_hash, 0, sizeof(flash_hash));
        }
    }
#endif

    snapshot_current_rgb(&last_rgb_snapshot);
    last_sent_mods = get_local_mods();
}

void housekeeping_task_user(void) {
    if (is_keyboard_master()) {
        if (bootloader_pending && timer_elapsed32(bootloader_req_time) >= RAW_BOOTLOADER_DELAY_MS) {
            bootloader_pending = false;
            oled_off();
            reset_keyboard();
            return;
        }
        bool trrs_connected = is_transport_connected();
        if (trrs_connected) {
            clear_host_sync_state();
            // Solid LED on TRRS master half
            gpio_write_pin_high(USR_LED_PIN);
        } else {
            if (host_service_connected && timer_elapsed32(host_service_last_rx) > HOST_SYNC_TIMEOUT_MS) {
                clear_host_sync_state();
            }
            // Dual-USB mode LED indicator: solid on Left when synced, Caps Lock on Right
            if (is_keyboard_left()) {
                gpio_write_pin(USR_LED_PIN, host_service_connected && host_peer_connected);
            } else {
                gpio_write_pin(USR_LED_PIN, host_keyboard_led_state().caps_lock);
            }
        }

        static uint16_t poll_timer               = 0;
        static bool     last_master_btn          = false;
        static bool     last_slave_btn           = false;
        static uint16_t last_sent_keys[5][6]     = {{0}};
        static uint16_t last_sent_ccw            = 0xFFFF;
        static uint16_t last_sent_cw             = 0xFFFF;
        static uint8_t  periodic_full_sync_count = 0;

        if (timer_elapsed(poll_timer) >= 20) {
            poll_timer = timer_read();

            bool master_btn = !gpio_read_pin(USR_BTN_PIN);
            bool slave_btn  = false;

            // User button (GP24):
            // - Left half (or TRRS master): toggle OLED & RGB lights
            // - Right half (Dual-USB): cycle RGB Matrix profile (synced to Left if paired)
            if (master_btn && !last_master_btn) {
                if (trrs_connected || is_keyboard_left()) {
                    lights_disabled = !lights_disabled;
                    if (lights_disabled) {
                        rgb_matrix_disable_noeeprom();
                        oled_off();
                    } else {
                        rgb_matrix_enable_noeeprom();
                        wake_local_displays_and_rgb();
                    }
                } else {
                    rgb_matrix_step();
                }
            }
            last_master_btn = master_btn;

            if (trrs_connected) {
                layer_state_t active_mask = layer_state | default_layer_state;
                uint8_t       top_layer   = get_highest_layer(active_mask);
                uint8_t       mods        = get_local_mods();
                bool          caps        = host_keyboard_led_state().caps_lock;
                uint8_t       hdr_flags   = 0;
                if (lights_disabled) hdr_flags |= SLAVE_HDR_LIGHTS_OFF;
                if (caps) hdr_flags |= SLAVE_HDR_CAPS_LOCK;
                if (!lights_disabled && last_input_activity_elapsed() < 2000) hdr_flags |= SLAVE_HDR_WAKE_ACTIVE;

                slave_sync_packet_t pkt;
                pkt.hdr.flags     = hdr_flags;
                pkt.hdr.top_layer = top_layer;
                pkt.hdr.mods      = mods;
                pkt.hdr.reserved  = 0;

                bool slave_is_left = !is_keyboard_left();
                populate_half_keys(slave_is_left, active_mask, top_layer, pkt.keys, &pkt.enc_ccw, &pkt.enc_cw);

                bool keys_changed = (memcmp(pkt.keys, last_sent_keys, sizeof(pkt.keys)) != 0) || (pkt.enc_ccw != last_sent_ccw) || (pkt.enc_cw != last_sent_cw);
                periodic_full_sync_count++;

                if (keys_changed || periodic_full_sync_count >= 12) {
                    if (transaction_rpc_exec(USR_BTN_SYNC, sizeof(pkt), &pkt, sizeof(slave_btn), &slave_btn)) {
                        memcpy(last_sent_keys, pkt.keys, sizeof(last_sent_keys));
                        last_sent_ccw            = pkt.enc_ccw;
                        last_sent_cw             = pkt.enc_cw;
                        periodic_full_sync_count = 0;
                    }
                } else {
                    transaction_rpc_exec(USR_BTN_SYNC, sizeof(pkt.hdr), &pkt.hdr, sizeof(slave_btn), &slave_btn);
                }

                // Non-master button over TRRS: cycle RGB Matrix profile
                if (slave_btn && !last_slave_btn) {
                    rgb_matrix_step();
                }
                last_slave_btn = slave_btn;
            } else {
                sync_local_state_if_needed();
            }
        }
    } else {
        // Caps Lock indicator on non-master half over TRRS
        bool caps = slave_has_hdr ? ((slave_hdr_state.flags & SLAVE_HDR_CAPS_LOCK) != 0) : host_keyboard_led_state().caps_lock;
        gpio_write_pin(USR_LED_PIN, caps);
    }

    if (lights_disabled && is_oled_on()) {
        oled_off();
    }
}

#ifdef OLED_ENABLE

#    define OLED_PORTRAIT_W 64
#    define OLED_PORTRAIT_H 128

#    define CHAR_ARROW_UP   '\x01'
#    define CHAR_ARROW_DOWN '\x02'
#    define CHAR_ARROW_LEFT '\x03'
#    define CHAR_ARROW_RGHT '\x04'

// 5x7 font for single-character labels (ASCII 0x20..0x7E, 5 bytes per character)
static const uint8_t PROGMEM oled_font5x7[][5] = {
    [' ' - 0x20]  = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['!' - 0x20]  = {0x00, 0x00, 0x5F, 0x00, 0x00},
    ['"' - 0x20]  = {0x00, 0x07, 0x00, 0x07, 0x00},
    ['#' - 0x20]  = {0x14, 0x7F, 0x14, 0x7F, 0x14},
    ['$' - 0x20]  = {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    ['%' - 0x20]  = {0x23, 0x13, 0x08, 0x64, 0x62},
    ['&' - 0x20]  = {0x36, 0x49, 0x56, 0x20, 0x50},
    ['\'' - 0x20] = {0x00, 0x08, 0x07, 0x03, 0x00},
    ['(' - 0x20]  = {0x00, 0x1C, 0x22, 0x41, 0x00},
    [')' - 0x20]  = {0x00, 0x41, 0x22, 0x1C, 0x00},
    ['*' - 0x20]  = {0x2A, 0x1C, 0x7F, 0x1C, 0x2A},
    ['+' - 0x20]  = {0x08, 0x08, 0x3E, 0x08, 0x08},
    [',' - 0x20]  = {0x00, 0x40, 0x30, 0x00, 0x00},
    ['-' - 0x20]  = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.' - 0x20]  = {0x00, 0x00, 0x30, 0x30, 0x00},
    ['/' - 0x20]  = {0x20, 0x10, 0x08, 0x04, 0x02},
    ['0' - 0x20]  = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1' - 0x20]  = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2' - 0x20]  = {0x72, 0x49, 0x49, 0x49, 0x46},
    ['3' - 0x20]  = {0x21, 0x41, 0x49, 0x4D, 0x33},
    ['4' - 0x20]  = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5' - 0x20]  = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6' - 0x20]  = {0x3C, 0x4A, 0x49, 0x49, 0x31},
    ['7' - 0x20]  = {0x41, 0x21, 0x11, 0x09, 0x07},
    ['8' - 0x20]  = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9' - 0x20]  = {0x46, 0x49, 0x49, 0x29, 0x1E},
    [':' - 0x20]  = {0x00, 0x00, 0x14, 0x00, 0x00},
    [';' - 0x20]  = {0x00, 0x40, 0x34, 0x00, 0x00},
    ['<' - 0x20]  = {0x00, 0x08, 0x14, 0x22, 0x41},
    ['=' - 0x20]  = {0x14, 0x14, 0x14, 0x14, 0x14},
    ['>' - 0x20]  = {0x00, 0x41, 0x22, 0x14, 0x08},
    ['?' - 0x20]  = {0x02, 0x01, 0x59, 0x09, 0x06},
    ['@' - 0x20]  = {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    ['A' - 0x20]  = {0x7C, 0x12, 0x11, 0x12, 0x7C},
    ['B' - 0x20]  = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C' - 0x20]  = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D' - 0x20]  = {0x7F, 0x41, 0x41, 0x41, 0x3E},
    ['E' - 0x20]  = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F' - 0x20]  = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G' - 0x20]  = {0x3E, 0x41, 0x41, 0x51, 0x73},
    ['H' - 0x20]  = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I' - 0x20]  = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J' - 0x20]  = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K' - 0x20]  = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L' - 0x20]  = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M' - 0x20]  = {0x7F, 0x02, 0x1C, 0x02, 0x7F},
    ['N' - 0x20]  = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O' - 0x20]  = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P' - 0x20]  = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q' - 0x20]  = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R' - 0x20]  = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S' - 0x20]  = {0x26, 0x49, 0x49, 0x49, 0x32},
    ['T' - 0x20]  = {0x03, 0x01, 0x7F, 0x01, 0x03},
    ['U' - 0x20]  = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V' - 0x20]  = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W' - 0x20]  = {0x3F, 0x40, 0x38, 0x40, 0x3F},
    ['X' - 0x20]  = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y' - 0x20]  = {0x03, 0x04, 0x78, 0x04, 0x03},
    ['Z' - 0x20]  = {0x61, 0x59, 0x49, 0x4D, 0x43},
    ['[' - 0x20]  = {0x00, 0x7F, 0x41, 0x41, 0x00},
    ['\\' - 0x20] = {0x02, 0x04, 0x08, 0x10, 0x20},
    [']' - 0x20]  = {0x00, 0x41, 0x41, 0x7F, 0x00},
    ['^' - 0x20]  = {0x04, 0x02, 0x01, 0x02, 0x04},
    ['_' - 0x20]  = {0x40, 0x40, 0x40, 0x40, 0x40},
    ['`' - 0x20]  = {0x00, 0x03, 0x07, 0x08, 0x00},
    ['a' - 0x20]  = {0x20, 0x54, 0x54, 0x78, 0x40},
    ['b' - 0x20]  = {0x7F, 0x28, 0x44, 0x44, 0x38},
    ['c' - 0x20]  = {0x38, 0x44, 0x44, 0x44, 0x28},
    ['d' - 0x20]  = {0x38, 0x44, 0x44, 0x28, 0x7F},
    ['e' - 0x20]  = {0x38, 0x54, 0x54, 0x54, 0x18},
    ['f' - 0x20]  = {0x00, 0x08, 0x7E, 0x09, 0x02},
    ['g' - 0x20]  = {0x18, 0x54, 0x54, 0x4C, 0x38},
    ['h' - 0x20]  = {0x7F, 0x08, 0x04, 0x04, 0x78},
    ['i' - 0x20]  = {0x00, 0x44, 0x7D, 0x40, 0x00},
    ['j' - 0x20]  = {0x20, 0x40, 0x44, 0x3D, 0x00},
    ['k' - 0x20]  = {0x7F, 0x10, 0x28, 0x44, 0x00},
    ['l' - 0x20]  = {0x00, 0x41, 0x7F, 0x40, 0x00},
    ['m' - 0x20]  = {0x7C, 0x04, 0x78, 0x04, 0x78},
    ['n' - 0x20]  = {0x7C, 0x08, 0x04, 0x04, 0x78},
    ['o' - 0x20]  = {0x38, 0x44, 0x44, 0x44, 0x38},
    ['p' - 0x20]  = {0x7C, 0x14, 0x24, 0x24, 0x18},
    ['q' - 0x20]  = {0x18, 0x24, 0x24, 0x14, 0x7C},
    ['r' - 0x20]  = {0x7C, 0x08, 0x04, 0x04, 0x08},
    ['s' - 0x20]  = {0x48, 0x54, 0x54, 0x54, 0x24},
    ['t' - 0x20]  = {0x04, 0x04, 0x3F, 0x44, 0x24},
    ['u' - 0x20]  = {0x3C, 0x40, 0x40, 0x20, 0x7C},
    ['v' - 0x20]  = {0x1C, 0x20, 0x40, 0x20, 0x1C},
    ['w' - 0x20]  = {0x3C, 0x40, 0x30, 0x40, 0x3C},
    ['x' - 0x20]  = {0x44, 0x28, 0x10, 0x28, 0x44},
    ['y' - 0x20]  = {0x1C, 0x50, 0x50, 0x50, 0x3C},
    ['z' - 0x20]  = {0x44, 0x64, 0x54, 0x4C, 0x44},
    ['{' - 0x20]  = {0x00, 0x08, 0x36, 0x41, 0x41},
    ['|' - 0x20]  = {0x00, 0x00, 0x7F, 0x00, 0x00},
    ['}' - 0x20]  = {0x41, 0x41, 0x36, 0x08, 0x00},
    ['~' - 0x20]  = {0x08, 0x04, 0x08, 0x10, 0x08},
};

static const uint8_t PROGMEM oled_arrows5x7[4][5] = {
    {0x04, 0x02, 0x7F, 0x02, 0x04}, // CHAR_ARROW_UP
    {0x10, 0x20, 0x7F, 0x20, 0x10}, // CHAR_ARROW_DOWN
    {0x08, 0x1C, 0x2A, 0x08, 0x08}, // CHAR_ARROW_LEFT
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, // CHAR_ARROW_RGHT
};

static void get_glyph3x7(char c, uint8_t out[3]) {
    switch (c) {
        case 'A': out[0] = 0x7E; out[1] = 0x11; out[2] = 0x7E; break;
        case 'B': out[0] = 0x7F; out[1] = 0x49; out[2] = 0x36; break;
        case 'C': out[0] = 0x3E; out[1] = 0x41; out[2] = 0x22; break;
        case 'D': out[0] = 0x7F; out[1] = 0x41; out[2] = 0x3E; break;
        case 'E': out[0] = 0x7F; out[1] = 0x49; out[2] = 0x41; break;
        case 'F': out[0] = 0x7F; out[1] = 0x09; out[2] = 0x01; break;
        case 'G': out[0] = 0x3E; out[1] = 0x41; out[2] = 0x79; break;
        case 'H': out[0] = 0x7F; out[1] = 0x08; out[2] = 0x7F; break;
        case 'I': out[0] = 0x41; out[1] = 0x7F; out[2] = 0x41; break;
        case 'J': out[0] = 0x20; out[1] = 0x40; out[2] = 0x3F; break;
        case 'K': out[0] = 0x7F; out[1] = 0x08; out[2] = 0x77; break;
        case 'L': out[0] = 0x7F; out[1] = 0x40; out[2] = 0x40; break;
        case 'M': out[0] = 0x7F; out[1] = 0x06; out[2] = 0x7F; break;
        case 'N': out[0] = 0x7F; out[1] = 0x06; out[2] = 0x7C; break;
        case 'O': out[0] = 0x3E; out[1] = 0x41; out[2] = 0x3E; break;
        case 'P': out[0] = 0x7F; out[1] = 0x09; out[2] = 0x06; break;
        case 'Q': out[0] = 0x3E; out[1] = 0x61; out[2] = 0x7E; break;
        case 'R': out[0] = 0x7F; out[1] = 0x19; out[2] = 0x66; break;
        case 'S': out[0] = 0x26; out[1] = 0x49; out[2] = 0x32; break;
        case 'T': out[0] = 0x01; out[1] = 0x7F; out[2] = 0x01; break;
        case 'U': out[0] = 0x3F; out[1] = 0x40; out[2] = 0x3F; break;
        case 'V': out[0] = 0x1F; out[1] = 0x60; out[2] = 0x1F; break;
        case 'W': out[0] = 0x7F; out[1] = 0x30; out[2] = 0x7F; break;
        case 'X': out[0] = 0x63; out[1] = 0x1C; out[2] = 0x63; break;
        case 'Y': out[0] = 0x07; out[1] = 0x78; out[2] = 0x07; break;
        case 'Z': out[0] = 0x61; out[1] = 0x59; out[2] = 0x43; break;
        case '0': out[0] = 0x7F; out[1] = 0x41; out[2] = 0x7F; break;
        case '1': out[0] = 0x42; out[1] = 0x7F; out[2] = 0x40; break;
        case '2': out[0] = 0x79; out[1] = 0x49; out[2] = 0x4F; break;
        case '3': out[0] = 0x49; out[1] = 0x49; out[2] = 0x7F; break;
        case '4': out[0] = 0x0F; out[1] = 0x08; out[2] = 0x7F; break;
        case '5': out[0] = 0x4F; out[1] = 0x49; out[2] = 0x79; break;
        case '6': out[0] = 0x7F; out[1] = 0x49; out[2] = 0x79; break;
        case '7': out[0] = 0x01; out[1] = 0x01; out[2] = 0x7F; break;
        case '8': out[0] = 0x7F; out[1] = 0x49; out[2] = 0x7F; break;
        case '9': out[0] = 0x4F; out[1] = 0x49; out[2] = 0x7F; break;
        case '+': out[0] = 0x08; out[1] = 0x1C; out[2] = 0x08; break;
        case '-': out[0] = 0x08; out[1] = 0x08; out[2] = 0x08; break;
        case '<': out[0] = 0x08; out[1] = 0x1C; out[2] = 0x22; break;
        case '>': out[0] = 0x22; out[1] = 0x1C; out[2] = 0x08; break;
        case '^': out[0] = 0x02; out[1] = 0x01; out[2] = 0x02; break;
        case '~': out[0] = 0x08; out[1] = 0x04; out[2] = 0x08; break;
        case '/': out[0] = 0x60; out[1] = 0x1C; out[2] = 0x03; break;
        default:  out[0] = 0x02; out[1] = 0x59; out[2] = 0x06; break;
    }
}

static bool get_glyph4x7_wide(char c, uint8_t out[4]) {
    switch (c) {
        case 'G': out[0] = 0x3E; out[1] = 0x41; out[2] = 0x51; out[3] = 0x72; return true;
        case 'M': out[0] = 0x7F; out[1] = 0x06; out[2] = 0x06; out[3] = 0x7F; return true;
        case 'N': out[0] = 0x7F; out[1] = 0x06; out[2] = 0x18; out[3] = 0x7F; return true;
        case 'W': out[0] = 0x7F; out[1] = 0x30; out[2] = 0x30; out[3] = 0x7F; return true;
        default: return false;
    }
}

static char basic_to_ascii(uint8_t kc, bool shift, bool caps) {
    if (kc >= KC_A && kc <= KC_Z) {
        char base = (char)('a' + (kc - KC_A));
        return (shift != caps) ? (char)(base - 'a' + 'A') : base;
    }
    if (kc >= KC_1 && kc <= KC_0) {
        static const char PROGMEM unshifted[10] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
        static const char PROGMEM shifted[10]   = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
        uint8_t idx = (uint8_t)(kc - KC_1);
        return shift ? (char)pgm_read_byte(&shifted[idx]) : (char)pgm_read_byte(&unshifted[idx]);
    }
    switch (kc) {
        case KC_MINS: return shift ? '_' : '-';
        case KC_EQL:  return shift ? '+' : '=';
        case KC_LBRC: return shift ? '{' : '[';
        case KC_RBRC: return shift ? '}' : ']';
        case KC_BSLS:
        case KC_NUBS: return shift ? '|' : '\\';
        case KC_NUHS: return shift ? '~' : '#';
        case KC_SCLN: return shift ? ':' : ';';
        case KC_QUOT: return shift ? '"' : '\'';
        case KC_GRV:  return shift ? '~' : '`';
        case KC_COMM: return shift ? '<' : ',';
        case KC_DOT:  return shift ? '>' : '.';
        case KC_SLSH: return shift ? '?' : '/';
        default:      return 0;
    }
}

static char basic_to_short_char(uint8_t kc) {
    char c = basic_to_ascii(kc, true, false);
    if (c != 0) {
        if (c >= 'a' && c <= 'z') {
            return (char)(c - 'a' + 'A');
        }
        return c;
    }
    switch (kc) {
        case KC_BSPC: return 'B';
        case KC_DEL:  return 'D';
        case KC_TAB:  return 'T';
        case KC_ESC:  return 'E';
        case KC_ENT:  return 'N';
        case KC_SPC:  return 'S';
        case KC_LEFT: return '<';
        case KC_RGHT: return '>';
        case KC_UP:   return 'U';
        case KC_DOWN: return 'N';
        case KC_HOME: return 'H';
        case KC_END:  return 'E';
        case KC_PGUP: return 'P';
        case KC_PGDN: return 'D';
        case KC_INS:  return 'I';
        default:      return '?';
    }
}

static void format_keycode_label(uint16_t kc, bool shift, bool caps, char out[4]) {
    out[0] = '\0';
    out[1] = '\0';
    out[2] = '\0';
    out[3] = '\0';

    if (kc == KC_NO || kc == KC_TRNS) {
        return;
    }

    if (IS_QK_MOD_TAP(kc) || IS_QK_LAYER_TAP(kc)) {
        kc = kc & 0xFF;
        if (kc == KC_NO) {
            return;
        }
    }

    if (IS_QK_MODS(kc)) {
        uint8_t mods    = QK_MODS_GET_MODS(kc);
        uint8_t base    = QK_MODS_GET_BASIC_KEYCODE(kc);
        bool    has_sft = (mods & 0x02) != 0;
        bool    has_ctl = (mods & 0x01) != 0;
        bool    has_alt = (mods & 0x04) != 0;
        bool    has_gui = (mods & 0x08) != 0;

        if (!has_ctl && !has_alt && !has_gui && has_sft) {
            char ch = basic_to_ascii(base, true, false);
            if (ch != 0) {
                out[0] = ch;
                return;
            }
            out[0] = 'S';
            out[1] = basic_to_short_char(base);
            return;
        }

        if (has_alt && !has_ctl && !has_gui && base >= KC_F1 && base <= KC_F9) {
            out[0] = '!';
            out[1] = 'F';
            out[2] = (char)('1' + (base - KC_F1));
            return;
        }

        if (has_ctl) {
            out[0] = '^';
        } else if (has_alt) {
            out[0] = 'A';
        } else {
            out[0] = 'G';
        }
        out[1] = basic_to_short_char(base);
        return;
    }

    if (kc <= 0xFF) {
        char ch = basic_to_ascii((uint8_t)kc, shift, caps);
        if (ch != 0) {
            out[0] = ch;
            return;
        }
    }

    if (kc >= KC_F1 && kc <= KC_F9) {
        out[0] = 'F';
        out[1] = (char)('1' + (kc - KC_F1));
        return;
    }
    if (kc >= KC_F10 && kc <= KC_F12) {
        out[0] = 'F';
        out[1] = '1';
        out[2] = (char)('0' + (kc - KC_F10));
        return;
    }
    if (kc >= KC_F13 && kc <= KC_F19) {
        out[0] = 'F';
        out[1] = '1';
        out[2] = (char)('3' + (kc - KC_F13));
        return;
    }
    if (kc >= KC_F20 && kc <= KC_F24) {
        out[0] = '2';
        out[1] = (char)('0' + (kc - KC_F20));
        return;
    }

    if (kc == TL_LOWR) {
        out[0] = 'L';
        out[1] = 'W';
        return;
    }
    if (kc == TL_UPPR) {
        out[0] = 'R';
        out[1] = 'S';
        return;
    }
    if (IS_QK_MOMENTARY(kc)) {
        uint8_t l = kc & 0x1F;
        if (l == 1) {
            out[0] = 'L';
            out[1] = 'W';
            return;
        }
        if (l == 2) {
            out[0] = 'R';
            out[1] = 'S';
            return;
        }
        if (l == 3) {
            out[0] = 'A';
            out[1] = 'J';
            return;
        }
        out[0] = 'L';
        out[1] = (char)('0' + (l % 10));
        return;
    }
    if (IS_QK_TOGGLE_LAYER(kc)) {
        out[0] = 'T';
        out[1] = (char)('0' + ((kc & 0x1F) % 10));
        return;
    }
    if (IS_QK_TO(kc)) {
        out[0] = '>';
        out[1] = (char)('0' + ((kc & 0x1F) % 10));
        return;
    }
    if (IS_QK_LAYER_TAP_TOGGLE(kc)) {
        out[0] = '~';
        out[1] = (char)('0' + ((kc & 0x1F) % 10));
        return;
    }
    if (IS_QK_DEF_LAYER(kc)) {
        out[0] = 'D';
        out[1] = (char)('0' + ((kc & 0x1F) % 10));
        return;
    }
    if (IS_QK_ONE_SHOT_LAYER(kc)) {
        out[0] = 'O';
        out[1] = (char)('0' + ((kc & 0x1F) % 10));
        return;
    }
    if (IS_QK_ONE_SHOT_MOD(kc)) {
        uint8_t m = kc & 0x1F;
        if (m & 0x02) {
            out[0] = 'S';
            out[1] = 'F';
            return;
        }
        if (m & 0x01) {
            out[0] = 'C';
            out[1] = 'T';
            return;
        }
        if (m & 0x04) {
            out[0] = 'A';
            out[1] = 'L';
            return;
        }
        out[0] = 'G';
        out[1] = 'U';
        return;
    }
    if (IS_QK_MACRO(kc)) {
        uint8_t idx = (kc - QK_MACRO) & 0x0F;
        out[0]      = 'M';
        out[1]      = (idx < 10) ? (char)('0' + idx) : (char)('A' + (idx - 10));
        return;
    }

    switch (kc) {
        case KC_TAB:  out[0] = 'T'; out[1] = 'B'; return;
        case KC_ESC:  out[0] = 'E'; out[1] = 'S'; return;
        case KC_BSPC: out[0] = 'B'; out[1] = 'S'; return;
        case KC_DEL:  out[0] = 'D'; out[1] = 'L'; return;
        case KC_SPC:  out[0] = 'S'; out[1] = 'P'; return;
        case KC_ENT:
        case KC_PENT: out[0] = 'E'; out[1] = 'N'; return;
        case KC_LSFT:
        case KC_RSFT: out[0] = 'S'; out[1] = 'F'; return;
        case KC_LCTL:
        case KC_RCTL: out[0] = 'C'; out[1] = 'T'; return;
        case KC_LALT:
        case KC_RALT: out[0] = 'A'; out[1] = 'L'; return;
        case KC_LGUI:
        case KC_RGUI: out[0] = 'G'; out[1] = 'U'; return;
        case KC_INS:  out[0] = 'I'; out[1] = 'N'; return;
        case KC_PSCR: out[0] = 'P'; out[1] = 'S'; return;
        case KC_SCRL: out[0] = 'S'; out[1] = 'L'; return;
        case KC_PAUS: out[0] = 'P'; out[1] = 'A'; return;
        case KC_APP:  out[0] = 'A'; out[1] = 'P'; return;
        case KC_CAPS: out[0] = 'C'; out[1] = 'P'; return;
        case KC_CALC: out[0] = 'C'; out[1] = 'L'; return;
        case KC_HOME: out[0] = 'H'; out[1] = 'M'; return;
        case KC_END:  out[0] = 'E'; out[1] = 'D'; return;
        case KC_PGUP: out[0] = 'P'; out[1] = 'U'; return;
        case KC_PGDN: out[0] = 'P'; out[1] = 'D'; return;
        case KC_UP:   out[0] = CHAR_ARROW_UP;   return;
        case KC_DOWN: out[0] = CHAR_ARROW_DOWN; return;
        case KC_LEFT: out[0] = CHAR_ARROW_LEFT; return;
        case KC_RGHT: out[0] = CHAR_ARROW_RGHT; return;
        case KC_MPLY: out[0] = 'P'; out[1] = 'L'; return;
        case KC_MSTP: out[0] = 'S'; out[1] = 'T'; return;
        case KC_MUTE:
        case KC_KB_MUTE: out[0] = 'M'; out[1] = 'U'; return;
        case KC_VOLU:
        case KC_KB_VOLUME_UP: out[0] = 'V'; out[1] = '+'; return;
        case KC_VOLD:
        case KC_KB_VOLUME_DOWN: out[0] = 'V'; out[1] = '-'; return;
        case KC_MNXT:
        case KC_MFFD: out[0] = '>'; out[1] = '>'; return;
        case KC_MPRV:
        case KC_MRWD: out[0] = '<'; out[1] = '<'; return;
        case KC_BRIU: out[0] = 'B'; out[1] = '+'; return;
        case KC_BRID: out[0] = 'B'; out[1] = '-'; return;
        case KC_EJCT: out[0] = 'E'; out[1] = 'J'; return;
        case KC_PWR:  out[0] = 'P'; out[1] = 'W'; return;
        case KC_SLEP: out[0] = 'Z'; out[1] = 'Z'; return;
        case KC_WAKE: out[0] = 'W'; out[1] = 'K'; return;
        case KC_UNDO: out[0] = 'U'; out[1] = 'N'; return;
        case KC_AGIN: out[0] = 'R'; out[1] = 'D'; return;
        case KC_CUT:  out[0] = 'C'; out[1] = 'X'; return;
        case KC_COPY: out[0] = 'C'; out[1] = 'C'; return;
        case KC_PSTE: out[0] = 'C'; out[1] = 'V'; return;
        case KC_FIND: out[0] = 'F'; out[1] = 'D'; return;
        case KC_MENU: out[0] = 'M'; out[1] = 'N'; return;
        case KC_PEQL: out[0] = '='; return;
        case KC_PCMM: out[0] = ','; return;
        case MS_WHLU: out[0] = 'W'; out[1] = 'U'; return;
        case MS_WHLD: out[0] = 'W'; out[1] = 'D'; return;
        case MS_WHLL: out[0] = 'W'; out[1] = 'L'; return;
        case MS_WHLR: out[0] = 'W'; out[1] = 'R'; return;
        case MS_UP:   out[0] = 'M'; out[1] = 'U'; return;
        case MS_DOWN: out[0] = 'M'; out[1] = 'D'; return;
        case MS_LEFT: out[0] = 'M'; out[1] = 'L'; return;
        case MS_RGHT: out[0] = 'M'; out[1] = 'R'; return;
        case MS_BTN1:
        case MS_BTN2:
        case MS_BTN3:
        case MS_BTN4:
        case MS_BTN5:
        case MS_BTN6:
        case MS_BTN7:
        case MS_BTN8:
            out[0] = 'M';
            out[1] = (char)('1' + (kc - MS_BTN1));
            return;
        case RM_TOGG: out[0] = 'R'; out[1] = 'G'; return;
        case RM_NEXT: out[0] = 'R'; out[1] = '+'; return;
        case RM_PREV: out[0] = 'R'; out[1] = '-'; return;
        case RM_HUEU: out[0] = 'H'; out[1] = '+'; return;
        case RM_HUED: out[0] = 'H'; out[1] = '-'; return;
        case RM_SATU: out[0] = 'S'; out[1] = '+'; return;
        case RM_SATD: out[0] = 'S'; out[1] = '-'; return;
        case RM_VALU: out[0] = 'B'; out[1] = '+'; return;
        case RM_VALD: out[0] = 'B'; out[1] = '-'; return;
        case RM_SPDU: out[0] = 'P'; out[1] = '+'; return;
        case RM_SPDD: out[0] = 'P'; out[1] = '-'; return;
        case QK_BOOT: out[0] = 'B'; out[1] = 'T'; return;
        case QK_RBT:  out[0] = 'R'; out[1] = 'B'; return;
        case EE_CLR:  out[0] = 'E'; out[1] = 'E'; return;
        case CG_SWAP:
        case CG_NORM:
        case CG_TOGG: out[0] = 'C'; out[1] = 'G'; return;
        case CW_TOGG: out[0] = 'C'; out[1] = 'W'; return;
        case KC_NUM:  out[0] = 'N'; out[1] = 'M'; return;
        case KC_PSLS: out[0] = '/'; return;
        case KC_PAST: out[0] = '*'; return;
        case KC_PMNS: out[0] = '-'; return;
        case KC_PPLS: out[0] = '+'; return;
        case KC_PDOT: out[0] = '.'; return;
        default:
            if (kc >= KC_P1 && kc <= KC_P9) {
                out[0] = (char)('1' + (kc - KC_P1));
                return;
            }
            if (kc == KC_P0) {
                out[0] = '0';
                return;
            }
            {
                static const char PROGMEM hex[16] = "0123456789ABCDEF";
                out[0] = (char)pgm_read_byte(&hex[(kc >> 4) & 0x0F]);
                out[1] = (char)pgm_read_byte(&hex[kc & 0x0F]);
            }
            return;
    }
}

static bool is_kc_active_pill(uint16_t kc, uint8_t top_layer, uint8_t mods, bool caps) {
    if ((kc == KC_LSFT || kc == KC_RSFT) && (mods & MOD_MASK_SHIFT)) return true;
    if ((kc == KC_LCTL || kc == KC_RCTL) && (mods & MOD_MASK_CTRL))  return true;
    if ((kc == KC_LALT || kc == KC_RALT) && (mods & MOD_MASK_ALT))   return true;
    if ((kc == KC_LGUI || kc == KC_RGUI) && (mods & MOD_MASK_GUI))   return true;
    if (kc == KC_CAPS && caps) return true;
    if (kc == TL_LOWR && (top_layer == _LOWER || top_layer == _ADJUST)) return true;
    if (kc == TL_UPPR && (top_layer == _RAISE || top_layer == _ADJUST)) return true;
    if (IS_QK_MOMENTARY(kc) && (kc & 0x1F) == top_layer && top_layer > 0) return true;
    return false;
}

static inline void fb_set_pixel(uint8_t *fb, uint8_t x, uint8_t y, bool on) {
    if (x >= OLED_PORTRAIT_W || y >= OLED_PORTRAIT_H) return;
    uint16_t idx = ((uint16_t)(y >> 3) * OLED_PORTRAIT_W) + x;
    uint8_t  bit = (uint8_t)(1U << (y & 7));
    if (on) {
        fb[idx] |= bit;
    } else {
        fb[idx] &= (uint8_t)~bit;
    }
}

static void fb_draw_cols7(uint8_t *fb, uint8_t x, uint8_t y, const uint8_t *cols, uint8_t width, bool invert) {
    for (uint8_t dx = 0; dx < width; dx++) {
        uint8_t col_bits = cols[dx];
        for (uint8_t dy = 0; dy < 7; dy++) {
            bool bit = (col_bits & (1U << dy)) != 0;
            fb_set_pixel(fb, (uint8_t)(x + dx), (uint8_t)(y + dy), invert ? !bit : bit);
        }
    }
}

static void fb_draw_cell(uint8_t *fb, uint8_t cx, uint8_t cy, const char label[4], bool active, bool is_knob, bool is_empty) {
    if (is_empty || label[0] == '\0') {
        if (active) {
            for (uint8_t dx = 1; dx <= 7; dx++) {
                fb_set_pixel(fb, (uint8_t)(cx + dx), cy, true);
                fb_set_pixel(fb, (uint8_t)(cx + dx), (uint8_t)(cy + 13), true);
            }
            for (uint8_t dy = 1; dy <= 12; dy++) {
                fb_set_pixel(fb, cx, (uint8_t)(cy + dy), true);
                fb_set_pixel(fb, (uint8_t)(cx + 8), (uint8_t)(cy + dy), true);
            }
        }
        fb_set_pixel(fb, (uint8_t)(cx + 4), (uint8_t)(cy + 7), true);
        return;
    }

    if (active) {
        for (uint8_t dy = 0; dy < 14; dy++) {
            uint8_t x0 = (dy == 0 || dy == 13) ? 1 : 0;
            uint8_t x1 = (dy == 0 || dy == 13) ? 7 : 8;
            for (uint8_t dx = x0; dx <= x1; dx++) {
                fb_set_pixel(fb, (uint8_t)(cx + dx), (uint8_t)(cy + dy), true);
            }
        }
    } else {
        for (uint8_t dx = 1; dx <= 7; dx++) {
            fb_set_pixel(fb, (uint8_t)(cx + dx), cy, true);
            fb_set_pixel(fb, (uint8_t)(cx + dx), (uint8_t)(cy + 13), true);
        }
        if (is_knob) {
            fb_set_pixel(fb, cx, (uint8_t)(cy + 1), true);
            fb_set_pixel(fb, (uint8_t)(cx + 8), (uint8_t)(cy + 1), true);
            fb_set_pixel(fb, cx, (uint8_t)(cy + 12), true);
            fb_set_pixel(fb, (uint8_t)(cx + 8), (uint8_t)(cy + 12), true);
        }
    }

    uint8_t ty = (uint8_t)(cy + 3);
    if (label[1] == '\0') {
        uint8_t c = (uint8_t)label[0];
        uint8_t glyph[5];
        if (c >= 1 && c <= 4) {
            for (uint8_t i = 0; i < 5; i++) {
                glyph[i] = pgm_read_byte(&oled_arrows5x7[c - 1][i]);
            }
        } else if (c >= 0x20 && c <= 0x7E) {
            for (uint8_t i = 0; i < 5; i++) {
                glyph[i] = pgm_read_byte(&oled_font5x7[c - 0x20][i]);
            }
        } else {
            for (uint8_t i = 0; i < 5; i++) {
                glyph[i] = pgm_read_byte(&oled_font5x7['?' - 0x20][i]);
            }
        }
        fb_draw_cols7(fb, (uint8_t)(cx + 2), ty, glyph, 5, active);
    } else if (label[2] == '\0') {
        uint8_t g0[4], g1[4];
        bool    w0 = (!active) && get_glyph4x7_wide(label[0], g0);
        bool    w1 = (!active) && get_glyph4x7_wide(label[1], g1);
        if (w0 != w1) {
            uint8_t len0 = w0 ? 4 : 3;
            uint8_t len1 = w1 ? 4 : 3;
            if (!w0) get_glyph3x7(label[0], g0);
            if (!w1) get_glyph3x7(label[1], g1);
            fb_draw_cols7(fb, (uint8_t)(cx + 1), ty, g0, len0, false);
            fb_draw_cols7(fb, (uint8_t)(cx + 1 + len0 + 1), ty, g1, len1, false);
        } else {
            get_glyph3x7(label[0], g0);
            get_glyph3x7(label[1], g1);
            fb_draw_cols7(fb, (uint8_t)(cx + 1), ty, g0, 3, active);
            fb_draw_cols7(fb, (uint8_t)(cx + 5), ty, g1, 3, active);
        }
    } else {
        uint8_t gA[3], gB[3];
        if (active) {
            const uint8_t f2[2] = {0x7F, 0x09};
            if (label[0] == '!' || label[0] == '1') {
                uint8_t c0 = (label[0] == '!') ? 0x5F : 0x7F;
                get_glyph3x7(label[2], gB);
                fb_draw_cols7(fb, (uint8_t)(cx + 1), ty, &c0, 1, true);
                fb_draw_cols7(fb, (uint8_t)(cx + 3), ty, f2, 2, true);
                if (label[2] == '1') {
                    uint8_t c2 = 0x7F;
                    fb_draw_cols7(fb, (uint8_t)(cx + 7), ty, &c2, 1, true);
                } else {
                    gB[2] &= 0x3E;
                    fb_draw_cols7(fb, (uint8_t)(cx + 6), ty, gB, 3, true);
                }
            } else {
                uint8_t c1 = (label[1] == '!') ? 0x5F : 0x7F;
                get_glyph3x7(label[2], gB);
                fb_draw_cols7(fb, (uint8_t)(cx + 1), ty, f2, 2, true);
                fb_draw_cols7(fb, (uint8_t)(cx + 4), ty, &c1, 1, true);
                if (label[2] == '1') {
                    uint8_t c2 = 0x7F;
                    fb_draw_cols7(fb, (uint8_t)(cx + 7), ty, &c2, 1, true);
                } else {
                    gB[2] &= 0x3E;
                    fb_draw_cols7(fb, (uint8_t)(cx + 6), ty, gB, 3, true);
                }
            }
        } else if (label[0] == '!' || label[0] == '1') {
            uint8_t c0 = (label[0] == '!') ? 0x5F : 0x7F;
            get_glyph3x7(label[1], gA);
            get_glyph3x7(label[2], gB);
            fb_draw_cols7(fb, cx, ty, &c0, 1, false);
            fb_draw_cols7(fb, (uint8_t)(cx + 2), ty, gA, 3, false);
            fb_draw_cols7(fb, (uint8_t)(cx + 6), ty, gB, 3, false);
        } else {
            uint8_t c1 = (label[1] == '!') ? 0x5F : 0x7F;
            get_glyph3x7(label[0], gA);
            get_glyph3x7(label[2], gB);
            fb_draw_cols7(fb, cx, ty, gA, 3, false);
            fb_draw_cols7(fb, (uint8_t)(cx + 4), ty, &c1, 1, false);
            fb_draw_cols7(fb, (uint8_t)(cx + 6), ty, gB, 3, false);
        }
    }
}

static void render_oled_layout(uint8_t *fb, bool is_left, uint8_t top_layer, oled_conn_mode_t conn_mode, uint8_t mods, bool caps, const uint8_t pressed_rows[5], const uint16_t keys[5][6], uint16_t enc_ccw, uint16_t enc_cw) {
    memset(fb, 0, OLED_MATRIX_SIZE);

    bool shift = (mods & MOD_MASK_SHIFT) != 0;
    bool ctrl  = (mods & MOD_MASK_CTRL) != 0;
    bool alt   = (mods & MOD_MASK_ALT) != 0;
    bool gui   = (mods & MOD_MASK_GUI) != 0;

    static const char PROGMEM conn_names[4][5] = {
        "TRRS",
        "SYNC",
        "WAIT",
        "N/C",
    };
    const char *cname = (conn_mode <= OLED_CONN_DISC) ? conn_names[conn_mode] : "N/C";
    char        hdr[7];
    hdr[0]       = (top_layer < 10) ? (char)('0' + top_layer) : '?';
    hdr[1]       = ':';
    uint8_t hlen = 2;
    for (uint8_t i = 0; i < 4; i++) {
        char ch = (conn_mode <= OLED_CONN_DISC) ? (char)pgm_read_byte(&cname[i]) : cname[i];
        if (ch == '\0') break;
        hdr[hlen++] = ch;
    }
    hdr[hlen] = '\0';

    uint8_t hx = 0;
    for (uint8_t i = 0; i < 6; i++) {
        char ch = hdr[i];
        if (ch == '\0') break;
        if (ch == ':') {
            uint8_t colon = 0x14;
            fb_draw_cols7(fb, hx, 1, &colon, 1, false);
            hx = (uint8_t)(hx + 2);
        } else {
            uint8_t g[3];
            get_glyph3x7(ch, g);
            fb_draw_cols7(fb, hx, 1, g, 3, false);
            hx = (uint8_t)(hx + 4);
        }
    }

    const char mod_chars[5]  = {'S', 'C', 'A', 'G', 'L'};
    const bool mod_active[5] = {shift, ctrl, alt, gui, caps};
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t bx = (uint8_t)(34 + i * 6);
        uint8_t g[3];
        get_glyph3x7(mod_chars[i], g);
        if (mod_active[i]) {
            for (uint8_t dy = 0; dy < 9; dy++) {
                for (uint8_t dx = 0; dx < 5; dx++) {
                    fb_set_pixel(fb, (uint8_t)(bx + dx), dy, true);
                }
            }
            fb_draw_cols7(fb, (uint8_t)(bx + 1), 1, g, 3, true);
        } else {
            fb_draw_cols7(fb, (uint8_t)(bx + 1), 1, g, 3, false);
        }
    }

    for (uint8_t x = 0; x < OLED_PORTRAIT_W; x++) {
        fb_set_pixel(fb, x, 10, true);
    }

    static const uint8_t col_x[6]     = {0, 11, 22, 33, 44, 55};
    static const uint8_t stagger_l[6] = {4, 4, 2, 0, 2, 3};
    static const uint8_t stagger_r[6] = {3, 2, 0, 2, 4, 4};
    static const int8_t  thumb_l[6]   = {2, 1, 0, 1, 2, -2};
    static const int8_t  thumb_r[6]   = {-2, 2, 1, 0, 1, 2};

    for (uint8_t r = 0; r < 5; r++) {
        for (uint8_t disp_c = 0; disp_c < 6; disp_c++) {
            uint8_t  mat_c   = is_left ? disp_c : (uint8_t)(5 - disp_c);
            uint16_t kc      = keys[r][mat_c];
            char     lbl[4];
            format_keycode_label(kc, shift, caps, lbl);
            bool    pressed = (pressed_rows[r] & (1U << mat_c)) != 0;
            bool    act     = pressed || is_kc_active_pill(kc, top_layer, mods, caps);
            uint8_t cx      = col_x[disp_c];
            bool    is_knob = (r == 4 && mat_c == 5);
            uint8_t cy;
            if (r < 4) {
                uint8_t stg = is_left ? stagger_l[disp_c] : stagger_r[disp_c];
                cy          = (uint8_t)(13 + r * 17 + stg);
            } else {
                int8_t arc = is_left ? thumb_l[disp_c] : thumb_r[disp_c];
                cy         = (uint8_t)(89 + arc);
            }
            fb_draw_cell(fb, cx, cy, lbl, act, is_knob, kc == KC_NO);
        }
    }

    for (uint8_t x = 0; x < OLED_PORTRAIT_W; x += 2) {
        fb_set_pixel(fb, x, 109, true);
    }

    char lbl_ccw[4], lbl_cw[4];
    format_keycode_label(enc_ccw, shift, caps, lbl_ccw);
    format_keycode_label(enc_cw, shift, caps, lbl_cw);

    uint8_t g_lt[3], g_gt[3], g_side[3];
    get_glyph3x7('<', g_lt);
    get_glyph3x7('>', g_gt);
    get_glyph3x7(is_left ? 'L' : 'R', g_side);

    fb_draw_cols7(fb, 1, 115, g_lt, 3, false);
    fb_draw_cell(fb, 8, 112, lbl_ccw, false, false, enc_ccw == KC_NO);

    for (uint8_t dx = 1; dx <= 5; dx++) {
        fb_set_pixel(fb, (uint8_t)(28 + dx), 114, true);
        fb_set_pixel(fb, (uint8_t)(28 + dx), 122, true);
    }
    for (uint8_t dy = 1; dy <= 7; dy++) {
        fb_set_pixel(fb, 28, (uint8_t)(114 + dy), true);
        fb_set_pixel(fb, 34, (uint8_t)(114 + dy), true);
    }
    fb_draw_cols7(fb, 30, 115, g_side, 3, false);

    fb_draw_cell(fb, 47, 112, lbl_cw, false, false, enc_cw == KC_NO);
    fb_draw_cols7(fb, 59, 115, g_gt, 3, false);
}

typedef struct {
    bool             valid;
    bool             is_left;
    uint8_t          top_layer;
    oled_conn_mode_t conn_mode;
    uint8_t          mods;
    bool             caps;
    uint8_t          pressed_rows[5];
    uint16_t         keys[5][6];
    uint16_t         enc_ccw;
    uint16_t         enc_cw;
} oled_render_cache_t;

static oled_render_cache_t oled_cache                   = {0};
static uint8_t             oled_frame[OLED_MATRIX_SIZE] = {0};

bool oled_task_user(void) {
    if (lights_disabled) {
        oled_off();
        return false;
    }

    bool             is_left = is_keyboard_left();
    uint8_t          top_layer;
    oled_conn_mode_t conn_mode;
    uint8_t          mods;
    bool             caps;
    uint8_t          pressed_rows[5];
    uint16_t         cur_keys[5][6];
    uint16_t         cur_ccw;
    uint16_t         cur_cw;

    uint8_t row_offset = is_left ? 0 : 5;
    for (uint8_t r = 0; r < 5; r++) {
        pressed_rows[r] = (uint8_t)(matrix_get_row((uint8_t)(r + row_offset)) & 0x3F);
    }

    if (!is_keyboard_master() && slave_has_hdr && slave_has_keys) {
        top_layer = slave_hdr_state.top_layer;
        conn_mode = OLED_CONN_TRRS;
        mods      = slave_hdr_state.mods;
        caps      = (slave_hdr_state.flags & SLAVE_HDR_CAPS_LOCK) != 0;
        memcpy(cur_keys, slave_keys, sizeof(cur_keys));
        cur_ccw = slave_enc_ccw;
        cur_cw  = slave_enc_cw;
    } else {
        layer_state_t active_mask = layer_state | default_layer_state;
        top_layer                 = get_highest_layer(active_mask);
        mods                      = get_local_mods();
        if (is_transport_connected() || !is_keyboard_master()) {
            conn_mode = OLED_CONN_TRRS;
        } else {
            mods |= remote_mods;
            if (host_service_connected && host_peer_connected) {
                conn_mode = OLED_CONN_SYNC;
            } else if (host_service_connected) {
                conn_mode = OLED_CONN_WAIT;
            } else {
                conn_mode = OLED_CONN_DISC;
            }
        }
        caps = host_keyboard_led_state().caps_lock;
        populate_half_keys(is_left, active_mask, top_layer, cur_keys, &cur_ccw, &cur_cw);
    }

    bool changed = !oled_cache.valid || (oled_cache.is_left != is_left) || (oled_cache.top_layer != top_layer) || (oled_cache.conn_mode != conn_mode) || (oled_cache.mods != mods) || (oled_cache.caps != caps) || (oled_cache.enc_ccw != cur_ccw) || (oled_cache.enc_cw != cur_cw) || (memcmp(oled_cache.pressed_rows, pressed_rows, sizeof(pressed_rows)) != 0) || (memcmp(oled_cache.keys, cur_keys, sizeof(cur_keys)) != 0);

    if (changed) {
        render_oled_layout(oled_frame, is_left, top_layer, conn_mode, mods, caps, pressed_rows, cur_keys, cur_ccw, cur_cw);
        oled_cache.valid     = true;
        oled_cache.is_left   = is_left;
        oled_cache.top_layer = top_layer;
        oled_cache.conn_mode = conn_mode;
        oled_cache.mods      = mods;
        oled_cache.caps      = caps;
        memcpy(oled_cache.pressed_rows, pressed_rows, sizeof(oled_cache.pressed_rows));
        memcpy(oled_cache.keys, cur_keys, sizeof(oled_cache.keys));
        oled_cache.enc_ccw = cur_ccw;
        oled_cache.enc_cw  = cur_cw;
    }

    oled_set_cursor(0, 0);
    oled_write_raw((const char *)oled_frame, OLED_MATRIX_SIZE);
    return false;
}

#endif
