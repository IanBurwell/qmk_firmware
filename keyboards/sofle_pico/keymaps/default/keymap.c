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

#ifdef OS_DETECTION_ENABLE
#    include "os_detection.h"
#endif

#include "usb_device_state.h"

#if defined(PROTOCOL_CHIBIOS) && defined(MCU_RP)
#    include "usb_main.h"

extern usb_endpoint_out_t usb_endpoints_out[USB_ENDPOINT_OUT_COUNT];

#    define SOFLE_USB_RECOVERY_ENABLED 1
#    define USB_STATE_IS_SUSPENDED() (USB_DRIVER.state == USB_SUSPENDED)
#    define USB_STATE_IS_ACTIVE() (USB_DRIVER.state == USB_ACTIVE)
#    define USB_REMOTE_WAKEUP_ENABLED() ((USB_DRIVER.status & 2U) != 0U)

extern void watchdog_enable(uint32_t delay_ms, bool pause_on_debug);
extern void watchdog_update(void);

#    define SOFLE_WDT_TIMEOUT_MS 4000U
static bool sofle_wdt_enabled = false;

static void sofle_wdt_enable(void) {
    watchdog_enable(SOFLE_WDT_TIMEOUT_MS, true);
    sofle_wdt_enabled = true;
}

static void sofle_wdt_feed(void) {
    if (sofle_wdt_enabled) {
        watchdog_update();
    }
}

static void sofle_wdt_disable(void) {
    if (sofle_wdt_enabled) {
        osalSysLock();
        WATCHDOG->CTRL &= ~WATCHDOG_CTRL_ENABLE;
        osalSysUnlock();
        sofle_wdt_enabled = false;
    }
}

static void sofle_usb_arm_sof_wakeup(void) {
    osalSysLock();
    if (USB_DRIVER.state == USB_SUSPENDED && (USB->INTE & USB_INTE_DEV_SOF) == 0U) {
        // Clear any stale pre-suspend SOF latch before arming SOF resume interrupt
        (void)USB->SOFRD;
        USB->INTE |= USB_INTE_DEV_SOF;
    }
    osalSysUnlock();
}

static void sofle_usb_restart(void) {
    restart_usb_driver(&USB_DRIVER);
}

static void sofle_usb_rearm_raw_out(void) {
#    ifdef RAW_ENABLE
    osalSysLock();
    if (usbGetDriverStateI(&USB_DRIVER) == USB_ACTIVE) {
        usb_endpoint_out_t *ep = &usb_endpoints_out[USB_ENDPOINT_OUT_RAW];
        if (ep->config.usbp == &USB_DRIVER && USB_DRIVER.epc[ep->config.ep] != NULL && USB_DRIVER.epc[ep->config.ep]->out_state != NULL) {
            if (!usbGetReceiveStatusI(ep->config.usbp, ep->config.ep)) {
                uint8_t *buffer = ibqGetEmptyBufferI(&ep->ibqueue);
                if (buffer != NULL) {
                    USBOutEndpointState *oesp = USB_DRIVER.epc[ep->config.ep]->out_state;
                    size_t               len  = ep->ibqueue.bsize - sizeof(size_t);
                    if ((USB_DPSRAM->BUFCTRL[ep->config.ep].OUT & USB_BUFFER_BUFFER0_AVAILABLE) != 0U) {
                        // Hardware buffer is still armed from pre-suspend; preserve DATA0/DATA1 PID
                        oesp->rxbuf  = buffer;
                        oesp->rxcnt  = 0;
                        oesp->rxsize = len;
                        oesp->rxpkts = 1;
                        USB_DRIVER.receiving |= (uint16_t)(1U << ep->config.ep);
                    } else {
                        usbStartReceiveI(ep->config.usbp, ep->config.ep, buffer, len);
                    }
                }
            } else {
                // Recover if receiving flag is set while hardware OUT buffer is neither armed nor pending ISR
                uint32_t out_ctrl = USB_DPSRAM->BUFCTRL[ep->config.ep].OUT;
                uint32_t out_bit  = 1U << (ep->config.ep * 2U + 1U);
                if ((out_ctrl & (USB_BUFFER_BUFFER0_AVAILABLE | USB_BUFFER_BUFFER0_FULL)) == 0U && (USB->BUFSTATUS & out_bit) == 0U) {
                    uint8_t *buffer = ibqGetEmptyBufferI(&ep->ibqueue);
                    if (buffer != NULL) {
                        USB_DRIVER.receiving &= (uint16_t)~(1U << ep->config.ep);
                        usbStartReceiveI(ep->config.usbp, ep->config.ep, buffer, ep->ibqueue.bsize - sizeof(size_t));
                    }
                }
            }
        }
    }
    osalSysUnlock();
#    endif
}
#elif defined(SOFLE_USB_RECOVERY_TEST)
#    define SOFLE_USB_RECOVERY_ENABLED 1
#else
static inline void sofle_wdt_enable(void) {}
static inline void sofle_wdt_feed(void) {}
static inline void sofle_wdt_disable(void) {}
#endif

#define USR_BTN_PIN GP24
#define USR_LED_PIN GP25

enum sofle_layers {
    _QWERTY,
    _LOWER,
    _RAISE,
    _ADJUST,
};

enum sofle_keycodes {
    HA_HEX_TOGG = SAFE_RANGE,
    WIN_AUD_SWP,
    HOST_SYNC_SETUP,
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
        _______,  KC_INS,   KC_PSCR,  KC_APP,   KC_CAPS,  A(KC_F4),                           KC_HOME,  C(KC_LEFT), KC_UP,  C(KC_RGHT), KC_END, C(KC_BSPC),
        _______,  G(KC_1),  G(KC_2),  G(KC_3),  G(KC_4),  KC_CALC,                            KC_PGUP,  KC_LEFT,  KC_DOWN,  KC_RGHT,  KC_BSPC,  KC_DEL,
        _______,  C(KC_Z),  C(KC_X),  C(KC_C),  C(KC_V),  G(KC_DOT), _______,       _______,  KC_PGDN,  KC_MPRV,  KC_MPLY,  KC_MNXT,  XXXXXXX,  _______,
                            _______,  _______,  _______,  _______,  _______,        _______,  _______,  _______,  _______,  _______
    ),
    [_ADJUST] = LAYOUT(
        QK_BOOT,  HOST_SYNC_SETUP, XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,                      RM_HUED,  RM_HUEU,  RM_SATD,  RM_SATU,  XXXXXXX,  XXXXXXX,
        XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,                            RM_NEXT,  RM_VALD,  RM_VALU,  RM_TOGG,  RM_SPDU,  XXXXXXX,
        XXXXXXX,  XXXXXXX,  XXXXXXX,  WIN_AUD_SWP, HA_HEX_TOGG, XXXXXXX,                      RM_PREV,  KC_VOLD,  KC_MUTE,  KC_VOLU,  RM_SPDD,  XXXXXXX,
        XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,  XXXXXXX,        XXXXXXX,  RM_TOGG,  _______,  _______,  _______,  XXXXXXX,  XXXXXXX,
                            _______,  _______,  _______,  _______,  _______,        _______,  _______,  _______,  _______,  _______
    )
    // clang-format on
};

const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [_QWERTY] = {ENCODER_CCW_CW(MS_WHLU, MS_WHLD), ENCODER_CCW_CW(KC_VOLU, KC_VOLD)},
    [_LOWER]  = {ENCODER_CCW_CW(RM_VALD, RM_VALU), ENCODER_CCW_CW(RM_HUED, RM_HUEU)},
    [_RAISE]  = {ENCODER_CCW_CW(RM_SPDD, RM_SPDU), ENCODER_CCW_CW(RM_PREV, RM_NEXT)},
    [_ADJUST] = {ENCODER_CCW_CW(RM_VALD, RM_VALU), ENCODER_CCW_CW(RM_HUEU, RM_HUED)},
};

#define SLAVE_HDR_LIGHTS_OFF  0x01
#define SLAVE_HDR_CAPS_LOCK   0x02
#define SLAVE_HDR_WAKE_ACTIVE 0x04
#define SLAVE_HDR_AUDIO_SHIFT 3
#define SLAVE_HDR_AUDIO_MASK  0x18 // 2-bit host_audio_output_t (0=unknown, 1=headset, 2=speaker)

typedef enum {
    OLED_APP_KEYMAP = 0, // Default: dynamic keymap & encoder layout
    OLED_APP_RGB_INFO,   // RGB Matrix settings (Left) & controls guide (Right)
    OLED_APP_COUNT,
} oled_app_t;

typedef struct {
    uint8_t flags;
    uint8_t top_layer;
    uint8_t mods;
    uint8_t oled_app;
} slave_sync_hdr_t;

typedef struct {
    slave_sync_hdr_t hdr;
    uint16_t         keys[5][6];
    uint16_t         enc_ccw;
    uint16_t         enc_cw;
    uint16_t         peer_enc_ccw;
    uint16_t         peer_enc_cw;
} slave_sync_packet_t;

_Static_assert(sizeof(slave_sync_packet_t) <= RPC_M2S_BUFFER_SIZE, "slave_sync_packet_t exceeds RPC_M2S_BUFFER_SIZE");

static bool             lights_disabled    = false;
static oled_app_t       current_oled_app   = OLED_APP_KEYMAP;
static slave_sync_hdr_t slave_hdr_state    = {0, 0, 0, 0};
static uint16_t         slave_keys[5][6]   = {{0}};
static uint16_t         slave_enc_ccw      = KC_NO;
static uint16_t         slave_enc_cw       = KC_NO;
static uint16_t         slave_peer_enc_ccw = KC_NO;
static uint16_t         slave_peer_enc_cw  = KC_NO;
static bool             slave_has_hdr      = false;
static bool             slave_has_keys     = false;

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

static void populate_rgb_right_panel(uint16_t out_keys[5][6], uint16_t *out_r_ccw, uint16_t *out_r_cw, uint16_t *out_l_ccw, uint16_t *out_l_cw) {
    layer_state_t adj_mask = ((layer_state_t)1 << _ADJUST) | ((layer_state_t)1 << _QWERTY);
    for (uint8_t r = 0; r < 5; r++) {
        for (uint8_t c = 0; c < 6; c++) {
            out_keys[r][c] = keycode_at_keymap_location(_ADJUST, (uint8_t)(r + 5), c);
        }
    }
    // Left encoder (idx 0): normal polarity (CCW = false, CW = true)
    *out_l_ccw = resolve_active_encoder(adj_mask, _ADJUST, 0, false);
    *out_l_cw  = resolve_active_encoder(adj_mask, _ADJUST, 0, true);
    // Right encoder (idx 1): Right half PCB is flipped (physical CCW = true, physical CW = false)
    *out_r_ccw = resolve_active_encoder(adj_mask, _ADJUST, 1, true);
    *out_r_cw  = resolve_active_encoder(adj_mask, _ADJUST, 1, false);
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

static void set_oled_app(oled_app_t new_app);

static void cycle_oled_app(void) {
    set_oled_app((oled_app_t)((current_oled_app + 1) % OLED_APP_COUNT));
}

static void toggle_lights_disabled(void) {
    lights_disabled = !lights_disabled;
    if (lights_disabled) {
#ifdef RGB_MATRIX_ENABLE
        rgb_matrix_disable_noeeprom();
#endif
        oled_off();
    } else {
#ifdef RGB_MATRIX_ENABLE
        rgb_matrix_enable_noeeprom();
#endif
        wake_local_displays_and_rgb();
    }
}

static void usr_btn_sync_slave_handler(uint8_t in_buflen, const void *in_data, uint8_t out_buflen, void *out_data) {
    if (in_buflen >= sizeof(slave_sync_hdr_t)) {
        slave_sync_hdr_t hdr;
        memcpy(&hdr, in_data, sizeof(hdr));
        bool prev_disabled = lights_disabled;
        lights_disabled    = (hdr.flags & SLAVE_HDR_LIGHTS_OFF) != 0;
        set_oled_app((hdr.oled_app < OLED_APP_COUNT) ? (oled_app_t)hdr.oled_app : OLED_APP_KEYMAP);
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
        slave_enc_ccw      = pkt.enc_ccw;
        slave_enc_cw       = pkt.enc_cw;
        slave_peer_enc_ccw = pkt.peer_enc_ccw;
        slave_peer_enc_cw  = pkt.peer_enc_cw;
        slave_has_keys     = true;
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

#ifdef RGB_MATRIX_ENABLE
#    define RGB_PRESET_SLOTS         32
#    define RGB_PRESET_MAGIC         0x52503031UL
#    define RGB_PRESET_SAVE_DELAY_MS 1500

typedef struct {
    uint8_t h;
    uint8_t s;
    uint8_t v;
    uint8_t speed;
} rgb_mode_preset_t;

typedef struct {
    uint32_t          magic;
    uint32_t          valid_mask;
    rgb_mode_preset_t slots[RGB_PRESET_SLOTS];
} rgb_mode_presets_store_t;

_Static_assert(sizeof(rgb_mode_presets_store_t) == EECONFIG_USER_DATA_SIZE, "rgb_mode_presets_store_t must match EECONFIG_USER_DATA_SIZE");

static rgb_mode_presets_store_t rgb_presets                  = {0};
static bool                     rgb_presets_loaded           = false;
static bool                     rgb_presets_dirty            = false;
static uint32_t                 rgb_presets_dirty_since      = 0;
static bool                     synced_rgb_eeprom_dirty      = false;
static uint32_t                 synced_rgb_eeprom_dirty_since = 0;
static uint8_t                  tracked_rgb_mode             = 0;
static uint8_t                  tracked_rgb_h           = 0;
static uint8_t                  tracked_rgb_s           = 0;
static uint8_t                  tracked_rgb_v           = 0;
static uint8_t                  tracked_rgb_speed       = 0;

static uint8_t rgb_clamp_val(uint8_t v) {
#    ifdef RGB_MATRIX_MAXIMUM_BRIGHTNESS
    return (v > RGB_MATRIX_MAXIMUM_BRIGHTNESS) ? RGB_MATRIX_MAXIMUM_BRIGHTNESS : v;
#    else
    return v;
#    endif
}

static rgb_mode_preset_t rgb_preset_default_for_mode(uint8_t mode) {
    uint8_t max_v = rgb_clamp_val(RGB_MATRIX_DEFAULT_VAL);
    switch (mode) {
        case RGB_MATRIX_SOLID_COLOR:
            return (rgb_mode_preset_t){RGB_MATRIX_DEFAULT_HUE, RGB_MATRIX_DEFAULT_SAT, max_v, 128};
#    ifdef ENABLE_RGB_MATRIX_GRADIENT_UP_DOWN_VAL
        case RGB_MATRIX_GRADIENT_UP_DOWN_VAL:
            return (rgb_mode_preset_t){RGB_MATRIX_DEFAULT_HUE, RGB_MATRIX_DEFAULT_SAT, max_v, 128};
#    endif
#    ifdef ENABLE_RGB_MATRIX_GRADIENT_UP_DOWN
        case RGB_MATRIX_GRADIENT_UP_DOWN:
            return (rgb_mode_preset_t){148, 255, max_v, 128};
#    endif
#    ifdef ENABLE_RGB_MATRIX_RAINBOW_MOVING_CHEVRON
        case RGB_MATRIX_RAINBOW_MOVING_CHEVRON:
            return (rgb_mode_preset_t){0, 255, max_v, 128};
#    endif
        default:
            return (rgb_mode_preset_t){RGB_MATRIX_DEFAULT_HUE, 255, max_v, 128};
    }
}

static rgb_mode_preset_t rgb_presets_get(uint8_t mode) {
    if (mode >= 1 && mode <= RGB_PRESET_SLOTS) {
        uint32_t bit = (1UL << (mode - 1));
        if ((rgb_presets.valid_mask & bit) != 0) {
            rgb_mode_preset_t p = rgb_presets.slots[mode - 1];
            p.v                 = rgb_clamp_val(p.v);
            return p;
        }
    }
    return rgb_preset_default_for_mode(mode);
}

static void rgb_presets_store_slot(uint8_t mode, uint8_t h, uint8_t s, uint8_t v, uint8_t speed, bool mark_dirty) {
    if (mode < 1 || mode > RGB_PRESET_SLOTS) {
        return;
    }
    v             = rgb_clamp_val(v);
    uint8_t  idx  = (uint8_t)(mode - 1);
    uint32_t bit  = (1UL << idx);
    bool     diff = ((rgb_presets.valid_mask & bit) == 0) || (rgb_presets.slots[idx].h != h) || (rgb_presets.slots[idx].s != s) || (rgb_presets.slots[idx].v != v) || (rgb_presets.slots[idx].speed != speed);
    if (!diff) {
        return;
    }
    rgb_presets.slots[idx].h     = h;
    rgb_presets.slots[idx].s     = s;
    rgb_presets.slots[idx].v     = v;
    rgb_presets.slots[idx].speed = speed;
    rgb_presets.valid_mask |= bit;
    if (mark_dirty) {
        rgb_presets_dirty       = true;
        rgb_presets_dirty_since = timer_read32();
    }
}

static void rgb_synced_eeprom_mark_dirty(void) {
    synced_rgb_eeprom_dirty       = true;
    synced_rgb_eeprom_dirty_since = timer_read32();
}

static void rgb_presets_flush_if_dirty(bool force) {
    if (!is_keyboard_master()) {
        return;
    }
    if (rgb_presets_dirty && (force || timer_elapsed32(rgb_presets_dirty_since) >= RGB_PRESET_SAVE_DELAY_MS)) {
        eeconfig_update_user_datablock(&rgb_presets, 0, sizeof(rgb_presets));
        rgb_presets_dirty = false;
    }
    if (synced_rgb_eeprom_dirty && (force || timer_elapsed32(synced_rgb_eeprom_dirty_since) >= RGB_PRESET_SAVE_DELAY_MS)) {
        eeconfig_force_flush_rgb_matrix();
        synced_rgb_eeprom_dirty = false;
    }
}

static void rgb_presets_init_defaults(bool write_eeprom) {
    memset(&rgb_presets, 0, sizeof(rgb_presets));
    rgb_presets.magic  = RGB_PRESET_MAGIC;
    uint8_t max_modes  = (RGB_MATRIX_EFFECT_MAX > 1) ? (uint8_t)(RGB_MATRIX_EFFECT_MAX - 1) : 1;
    if (max_modes > RGB_PRESET_SLOTS) {
        max_modes = RGB_PRESET_SLOTS;
    }
    for (uint8_t m = 1; m <= max_modes; m++) {
        rgb_presets.slots[m - 1] = rgb_preset_default_for_mode(m);
        rgb_presets.valid_mask |= (1UL << (m - 1));
    }
    if (write_eeprom) {
        eeconfig_update_user_datablock(&rgb_presets, 0, sizeof(rgb_presets));
        rgb_presets_dirty = false;
    }
}

static void rgb_presets_load_or_init(void) {
    if (!is_keyboard_master()) {
        return;
    }
    bool valid = eeconfig_is_user_datablock_valid();
    if (valid) {
        eeconfig_read_user_datablock(&rgb_presets, 0, sizeof(rgb_presets));
        if (rgb_presets.magic != RGB_PRESET_MAGIC) {
            valid = false;
        }
    }
    uint8_t cur_mode  = rgb_matrix_get_mode();
    hsv_t   cur_hsv   = rgb_matrix_get_hsv();
    uint8_t cur_v     = rgb_clamp_val(cur_hsv.v);
    uint8_t cur_speed = rgb_matrix_get_speed();

    if (!valid) {
        rgb_presets_init_defaults(false);
        // Preserve the keyboard's active mode color on first upgrade to per-mode storage
        rgb_presets_store_slot(cur_mode, cur_hsv.h, cur_hsv.s, cur_v, cur_speed, false);
        eeconfig_update_user_datablock(&rgb_presets, 0, sizeof(rgb_presets));
    } else {
        rgb_presets_store_slot(cur_mode, cur_hsv.h, cur_hsv.s, cur_v, cur_speed, false);
    }

    rgb_presets_dirty  = false;
    tracked_rgb_mode   = cur_mode;
    tracked_rgb_h      = cur_hsv.h;
    tracked_rgb_s      = cur_hsv.s;
    tracked_rgb_v      = cur_v;
    tracked_rgb_speed  = cur_speed;
    rgb_presets_loaded = true;
}

static void rgb_presets_record_synced_state(uint8_t mode, uint8_t h, uint8_t s, uint8_t v, uint8_t speed) {
    v                 = rgb_clamp_val(v);
    tracked_rgb_mode  = mode;
    tracked_rgb_h     = h;
    tracked_rgb_s     = s;
    tracked_rgb_v     = v;
    tracked_rgb_speed = speed;
    rgb_presets_store_slot(mode, h, s, v, speed, true);
}

static void rgb_presets_track_and_apply_local(void) {
    if (!is_keyboard_master() || !rgb_presets_loaded || lights_disabled || !rgb_matrix_is_enabled()) {
        return;
    }
    uint8_t cur_mode  = rgb_matrix_get_mode();
    hsv_t   cur_hsv   = rgb_matrix_get_hsv();
    uint8_t cur_speed = rgb_matrix_get_speed();

    if (cur_mode != tracked_rgb_mode) {
        rgb_mode_preset_t p = rgb_presets_get(cur_mode);
        rgb_matrix_sethsv(p.h, p.s, p.v);
        rgb_matrix_set_speed(p.speed);
        hsv_t applied      = rgb_matrix_get_hsv();
        uint8_t applied_sp = rgb_matrix_get_speed();
        tracked_rgb_mode   = cur_mode;
        tracked_rgb_h      = applied.h;
        tracked_rgb_s      = applied.s;
        tracked_rgb_v      = applied.v;
        tracked_rgb_speed  = applied_sp;
        rgb_presets_store_slot(cur_mode, applied.h, applied.s, applied.v, applied_sp, false);
    } else if (cur_hsv.h != tracked_rgb_h || cur_hsv.s != tracked_rgb_s || cur_hsv.v != tracked_rgb_v || cur_speed != tracked_rgb_speed) {
        tracked_rgb_h     = cur_hsv.h;
        tracked_rgb_s     = cur_hsv.s;
        tracked_rgb_v     = cur_hsv.v;
        tracked_rgb_speed = cur_speed;
        rgb_presets_store_slot(cur_mode, cur_hsv.h, cur_hsv.s, cur_hsv.v, cur_speed, true);
    }
}
#else
static inline void rgb_presets_flush_if_dirty(bool force) { (void)force; }
static inline void rgb_presets_init_defaults(bool write_eeprom) { (void)write_eeprom; }
static inline void rgb_presets_load_or_init(void) {}
static inline void rgb_presets_track_and_apply_local(void) {}
#endif

#define RAW_SYNC_CMD_ID           0xF0
#define RAW_SYNC_MAGIC_0          0x53 // 'S'
#define RAW_SYNC_MAGIC_1          0x50 // 'P'
#define RAW_MSG_HOST_PING         0x01
#define RAW_MSG_HALF_STATE        0x02
#define RAW_MSG_IDENTIFY          0x03
#define RAW_MSG_BOOTLOADER        0x04
#define RAW_MSG_HOST_ACTION       0x05
#define HOST_ACTION_HA_HEX_TOGGLE 0x01
#define HOST_ACTION_AUDIO_SWAP    0x02
#define RAW_MSG_HOST_STATUS       0x06
#define RAW_BOOTLOADER_DELAY_MS   60
#define HOST_SYNC_TIMEOUT_MS      2500
#define ACTIVITY_WAKE_THROTTLE_MS 1000

typedef enum {
    HOST_AUDIO_UNKNOWN = 0, // Unknown/disconnected -> show Speaker icon
    HOST_AUDIO_HEADSET = 1, // Currently on Headset -> pressing switches to Speakers -> show Speaker icon
    HOST_AUDIO_SPEAKER = 2, // Currently on Speakers -> pressing switches to Headset -> show Headphones icon
} host_audio_output_t;

#define HALF_FLAG_LOWER_HELD    0x01
#define HALF_FLAG_UPPER_HELD    0x02
#define HALF_FLAG_LIGHTS_OFF    0x04
#define HALF_FLAG_RGB_CHANGED   0x08
#define HALF_FLAG_SYNC_TIMER    0x10
#define HALF_FLAG_PING_REPLY    0x20
#define HALF_FLAG_ACTIVITY_WAKE 0x40
#define HALF_FLAG_APP_CHANGED   0x80

static bool                host_service_connected   = false;
static bool                host_peer_connected      = false;
static uint32_t            host_service_last_rx     = 0;
static uint8_t             host_audio_output        = HOST_AUDIO_UNKNOWN;
static bool                host_status_active       = false;
static uint32_t            host_status_last_rx      = 0;
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
static uint8_t             local_host_action_seq    = 0;
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

    if (want_lower && !layer_state_is(_LOWER)) {
        layer_on(_LOWER);
    } else if (!want_lower && layer_state_is(_LOWER)) {
        layer_off(_LOWER);
    }

    if (want_upper && !layer_state_is(_RAISE)) {
        layer_on(_RAISE);
    } else if (!want_upper && layer_state_is(_RAISE)) {
        layer_off(_RAISE);
    }

    bool want_adjust = (want_lower && want_upper) || (current_oled_app == OLED_APP_RGB_INFO);
    if (want_adjust && !layer_state_is(_ADJUST)) {
        layer_on(_ADJUST);
    } else if (!want_adjust && layer_state_is(_ADJUST)) {
        layer_off(_ADJUST);
    }
}

static void sync_app_layer_override(void) {
    if (!is_keyboard_master()) {
        return;
    }
    if (current_oled_app == OLED_APP_RGB_INFO) {
        if (!layer_state_is(_ADJUST)) {
            layer_on(_ADJUST);
        }
    } else {
        if (!is_transport_connected()) {
            apply_combined_tri_layers();
        } else {
            update_tri_layer(_LOWER, _RAISE, _ADJUST);
        }
    }
}

static void set_oled_app(oled_app_t new_app) {
    oled_app_t clamped = (new_app < OLED_APP_COUNT) ? new_app : OLED_APP_KEYMAP;
    if (current_oled_app == clamped) {
        return;
    }
    current_oled_app = clamped;
    sync_app_layer_override();
}

layer_state_t layer_state_set_user(layer_state_t state) {
    if (is_keyboard_master() && current_oled_app == OLED_APP_RGB_INFO) {
        state |= ((layer_state_t)1 << _ADJUST);
    }
    return state;
}

static void host_sync_send_state(bool rgb_changed, bool sync_rgb_timer, bool is_reply, bool activity_wake, bool app_changed) {
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
    if (app_changed) {
        flags |= HALF_FLAG_APP_CHANGED;
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
    pkt[15] = (uint8_t)current_oled_app;

    raw_hid_send(pkt, sizeof(pkt));
#else
    (void)rgb_changed;
    (void)sync_rgb_timer;
    (void)is_reply;
    (void)activity_wake;
    (void)app_changed;
#endif
}

static void host_sync_send_action(uint8_t action_id) {
#ifdef RAW_ENABLE
    if (!is_keyboard_master()) {
        return;
    }

    uint8_t pkt[RAW_EPSIZE];
    memset(pkt, 0, sizeof(pkt));
    local_host_action_seq++;

    pkt[0] = RAW_SYNC_CMD_ID;
    pkt[1] = RAW_SYNC_MAGIC_0;
    pkt[2] = RAW_SYNC_MAGIC_1;
    pkt[3] = RAW_MSG_HOST_ACTION;
    pkt[4] = is_keyboard_left() ? 1 : 0;
    pkt[5] = action_id;
    pkt[6] = local_host_action_seq;

    raw_hid_send(pkt, sizeof(pkt));
#else
    (void)action_id;
#endif
}

static void clear_dual_usb_peer_state(void) {
    bool was_host_paired  = host_peer_connected;
    bool had_app_override = !is_transport_connected() && was_host_paired && is_keyboard_left() && (current_oled_app != OLED_APP_KEYMAP);
    bool had_remote       = host_service_connected || host_peer_connected || remote_lower_held || remote_upper_held || (remote_mods != 0) || had_app_override;
    host_service_connected   = false;
    host_peer_connected      = false;
    last_activity_sync_valid = false;
    remote_lower_held        = false;
    remote_upper_held        = false;
    remote_mods              = 0;
    if (had_app_override) {
        current_oled_app = OLED_APP_KEYMAP;
    }
    if (had_remote) {
        apply_combined_tri_layers();
    }
}

static void clear_host_sync_state(void) {
    host_audio_output  = HOST_AUDIO_UNKNOWN;
    host_status_active = false;
    clear_dual_usb_peer_state();
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

    if (msg_type == RAW_MSG_HOST_STATUS) {
        uint8_t audio_state = data[4];
        host_audio_output   = (audio_state <= HOST_AUDIO_SPEAKER) ? audio_state : HOST_AUDIO_UNKNOWN;
        host_status_active  = (host_audio_output != HOST_AUDIO_UNKNOWN);
        host_status_last_rx = timer_read32();
        return true;
    }

    if (is_transport_connected()) {
        if (msg_type == RAW_MSG_HOST_PING) {
            host_status_last_rx = timer_read32();
        }
        return true;
    }

    if (msg_type == RAW_MSG_HOST_PING) {
        host_service_last_rx   = timer_read32();
        host_status_last_rx    = host_service_last_rx;
        host_service_connected = true;
        bool new_peer          = (data[4] != 0);
        if (host_peer_connected && !new_peer) {
            remote_lower_held = false;
            remote_upper_held = false;
            remote_mods       = 0;
            if (is_keyboard_left()) {
                current_oled_app = OLED_APP_KEYMAP;
            }
            apply_combined_tri_layers();
        }
        host_peer_connected = new_peer;

        if (data[5] != 0) {
            bool auth_rgb = (data[6] != 0);
            last_sent_mods = get_local_mods();
            snapshot_current_rgb(&last_rgb_snapshot);
            host_sync_send_state(auth_rgb, auth_rgb, true, false, auth_rgb);
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

        if ((flags & HALF_FLAG_APP_CHANGED) != 0 && length >= 16) {
            set_oled_app((data[15] < OLED_APP_COUNT) ? (oled_app_t)data[15] : OLED_APP_KEYMAP);
        }

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
                bool rgb_ee_changed = false;
                if (new_enable) {
                    if (!rgb_matrix_is_enabled()) {
                        rgb_matrix_enable_noeeprom();
                        if (!prev_lights_disabled) {
                            rgb_ee_changed = true;
                        }
                    }
                    if (new_mode < 1 || new_mode >= RGB_MATRIX_EFFECT_MAX) {
                        new_mode = RGB_MATRIX_SOLID_COLOR;
                    }
                    if (rgb_matrix_get_mode() != new_mode) {
                        rgb_matrix_mode_noeeprom(new_mode);
                        rgb_ee_changed = true;
                    }
                    hsv_t cur_hsv = rgb_matrix_get_hsv();
                    if (cur_hsv.h != new_hue || cur_hsv.s != new_sat || cur_hsv.v != new_val) {
                        rgb_matrix_sethsv_noeeprom(new_hue, new_sat, new_val);
                        rgb_ee_changed = true;
                    }
                    if (rgb_matrix_get_speed() != new_speed) {
                        rgb_matrix_set_speed_noeeprom(new_speed);
                        rgb_ee_changed = true;
                    }
                    if ((flags & HALF_FLAG_SYNC_TIMER) != 0) {
                        g_rgb_timer = 0;
                    }
                    hsv_t synced_hsv = rgb_matrix_get_hsv();
                    rgb_presets_record_synced_state(rgb_matrix_get_mode(), synced_hsv.h, synced_hsv.s, synced_hsv.v, rgb_matrix_get_speed());
                } else if (rgb_matrix_is_enabled()) {
                    rgb_matrix_disable_noeeprom();
                    rgb_ee_changed = true;
                }
                if (rgb_ee_changed) {
                    rgb_synced_eeprom_mark_dirty();
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
        host_sync_send_state(rgb_changed, mode_changed, false, true, false);
    }
}

#define HOST_SETUP_PRE_DELAY_MS  250U
#define HOST_SETUP_WIN_WAIT_MS   650U
#define HOST_SETUP_LIN_WAIT_MS   950U
#define HOST_SETUP_MAC_SPOT_MS   450U
#define HOST_SETUP_MAC_WAIT_MS   950U
#define HOST_SETUP_CHAR_MS       8U
#define HOST_SETUP_ENTER_WAIT_MS 80U

typedef enum {
    HOST_SETUP_IDLE = 0,
    HOST_SETUP_PRE_DELAY,
    HOST_SETUP_MAC_TERM,
    HOST_SETUP_WAIT_TERM,
    HOST_SETUP_TYPING,
    HOST_SETUP_FINISH,
} host_setup_stage_t;

typedef enum {
    HOST_SETUP_OS_WINDOWS = 0,
    HOST_SETUP_OS_LINUX,
    HOST_SETUP_OS_MACOS,
} host_setup_os_t;

static const char PROGMEM host_setup_cmd_win[] =
    "powershell -NoP -EP Bypass -C \"irm https://raw.githubusercontent.com/IanBurwell/qmk_firmware/ib-sofle-pico/keyboards/sofle_pico/keymaps/default/host_sync/install.ps1|iex\"";

static const char PROGMEM host_setup_cmd_unix[] =
    "U=https://raw.githubusercontent.com/IanBurwell/qmk_firmware/ib-sofle-pico/keyboards/sofle_pico/keymaps/default/host_sync/install.sh;(curl -fsSL $U||wget -qO- $U)>/tmp/s.sh&&bash /tmp/s.sh";

static const char PROGMEM host_setup_mac_app[] = "Terminal";

static host_setup_stage_t host_setup_stage        = HOST_SETUP_IDLE;
static host_setup_os_t    host_setup_os           = HOST_SETUP_OS_WINDOWS;
static const char        *host_setup_ptr          = NULL;
static uint16_t           host_setup_idx          = 0;
static uint32_t           host_setup_timer        = 0;
static uint16_t           host_setup_delay_ms     = 0;
static bool               host_setup_restore_caps = false;

static host_setup_os_t resolve_host_setup_os(uint8_t mods) {
    if (mods & MOD_MASK_SHIFT) {
        return HOST_SETUP_OS_LINUX;
    }
    if (mods & MOD_MASK_CTRL) {
        return HOST_SETUP_OS_WINDOWS;
    }
#ifdef OS_DETECTION_ENABLE
    switch (detected_host_os()) {
        case OS_LINUX:
            return HOST_SETUP_OS_LINUX;
        case OS_MACOS:
            return HOST_SETUP_OS_MACOS;
        default:
            return HOST_SETUP_OS_WINDOWS;
    }
#else
    return HOST_SETUP_OS_WINDOWS;
#endif
}

static void host_setup_start(host_setup_os_t target_os) {
    host_setup_os           = target_os;
    host_setup_ptr          = (target_os == HOST_SETUP_OS_WINDOWS) ? host_setup_cmd_win : host_setup_cmd_unix;
    host_setup_idx          = 0;
    host_setup_restore_caps = false;
    host_setup_timer        = timer_read32();
    host_setup_stage        = HOST_SETUP_PRE_DELAY;
}

static void host_setup_abort(void) {
    if (host_setup_stage == HOST_SETUP_IDLE) {
        return;
    }
    if (host_setup_restore_caps) {
        tap_code(KC_CAPS);
        host_setup_restore_caps = false;
    }
    host_setup_stage = HOST_SETUP_IDLE;
    host_setup_ptr   = NULL;
    host_setup_idx   = 0;
}

static void host_setup_launch_terminal(void) {
    clear_mods();
    clear_weak_mods();
    clear_oneshot_mods();
    send_keyboard_report();
    if (host_setup_os == HOST_SETUP_OS_WINDOWS) {
        tap_code16(LGUI(KC_R));
        host_setup_delay_ms = HOST_SETUP_WIN_WAIT_MS;
        host_setup_timer    = timer_read32();
        host_setup_stage    = HOST_SETUP_WAIT_TERM;
    } else if (host_setup_os == HOST_SETUP_OS_LINUX) {
        tap_code16(LCTL(LALT(KC_T)));
        host_setup_delay_ms = HOST_SETUP_LIN_WAIT_MS;
        host_setup_timer    = timer_read32();
        host_setup_stage    = HOST_SETUP_WAIT_TERM;
    } else {
        tap_code16(LGUI(KC_SPC));
        host_setup_idx   = 0;
        host_setup_timer = timer_read32();
        host_setup_stage = HOST_SETUP_MAC_TERM;
    }
}

static void host_setup_task(void) {
    switch (host_setup_stage) {
        case HOST_SETUP_IDLE:
            return;

        case HOST_SETUP_PRE_DELAY:
            if (timer_elapsed32(host_setup_timer) < HOST_SETUP_PRE_DELAY_MS) {
                return;
            }
            if (host_keyboard_led_state().caps_lock) {
                tap_code(KC_CAPS);
                host_setup_restore_caps = true;
            }
            host_setup_launch_terminal();
            return;

        case HOST_SETUP_MAC_TERM: {
            uint16_t elapsed = (host_setup_idx == 0) ? HOST_SETUP_MAC_SPOT_MS : HOST_SETUP_CHAR_MS;
            if (timer_elapsed32(host_setup_timer) < elapsed) {
                return;
            }
            char ch = (char)pgm_read_byte(&host_setup_mac_app[host_setup_idx]);
            if (ch == '\0') {
                tap_code(KC_ENT);
                host_setup_idx      = 0;
                host_setup_delay_ms = HOST_SETUP_MAC_WAIT_MS;
                host_setup_timer    = timer_read32();
                host_setup_stage    = HOST_SETUP_WAIT_TERM;
            } else {
                send_char(ch);
                host_setup_idx++;
                host_setup_timer = timer_read32();
            }
            return;
        }

        case HOST_SETUP_WAIT_TERM:
            if (timer_elapsed32(host_setup_timer) < host_setup_delay_ms) {
                return;
            }
            host_setup_idx   = 0;
            host_setup_timer = timer_read32();
            host_setup_stage = HOST_SETUP_TYPING;
            return;

        case HOST_SETUP_TYPING: {
            if (host_setup_idx > 0 && timer_elapsed32(host_setup_timer) < HOST_SETUP_CHAR_MS) {
                return;
            }
            char ch = (char)pgm_read_byte(&host_setup_ptr[host_setup_idx]);
            if (ch == '\0') {
                host_setup_timer = timer_read32();
                host_setup_stage = HOST_SETUP_FINISH;
            } else {
                send_char(ch);
                host_setup_idx++;
                host_setup_timer = timer_read32();
            }
            return;
        }

        case HOST_SETUP_FINISH:
            if (timer_elapsed32(host_setup_timer) < HOST_SETUP_ENTER_WAIT_MS) {
                return;
            }
            tap_code(KC_ENT);
            if (host_setup_restore_caps) {
                tap_code(KC_CAPS);
                host_setup_restore_caps = false;
            }
            host_setup_stage = HOST_SETUP_IDLE;
            host_setup_ptr   = NULL;
            host_setup_idx   = 0;
            return;
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (host_setup_stage != HOST_SETUP_IDLE && keycode == KC_ESC && record->event.pressed) {
        host_setup_abort();
        return false;
    }
    if (is_keyboard_master() && !is_transport_connected()) {
        if (keycode == QK_TRI_LAYER_LOWER) {
            local_lower_held = record->event.pressed;
            apply_combined_tri_layers();
            last_sent_mods = get_local_mods();
            host_sync_send_state(false, false, false, record->event.pressed, false);
            return false;
        }
        if (keycode == QK_TRI_LAYER_UPPER) {
            local_upper_held = record->event.pressed;
            apply_combined_tri_layers();
            last_sent_mods = get_local_mods();
            host_sync_send_state(false, false, false, record->event.pressed, false);
            return false;
        }
        if (record->event.pressed && host_service_connected && host_peer_connected) {
            if (!last_activity_sync_valid || timer_elapsed32(last_activity_sync_tx) >= ACTIVITY_WAKE_THROTTLE_MS) {
                last_sent_mods = get_local_mods();
                host_sync_send_state(false, false, false, true, false);
            }
        }
    }
    if (keycode == HA_HEX_TOGG) {
        if (record->event.pressed) {
            host_sync_send_action(HOST_ACTION_HA_HEX_TOGGLE);
        }
        return false;
    }
    if (keycode == WIN_AUD_SWP) {
        if (record->event.pressed) {
            host_sync_send_action(HOST_ACTION_AUDIO_SWAP);
        }
        return false;
    }
    if (keycode == HOST_SYNC_SETUP) {
        if (record->event.pressed && is_keyboard_master() && host_setup_stage == HOST_SETUP_IDLE) {
            host_setup_start(resolve_host_setup_os(get_mods() | get_oneshot_mods()));
        }
        return false;
    }
    return true;
}

void post_process_record_user(uint16_t keycode, keyrecord_t *record) {
    (void)keycode;
    (void)record;
    rgb_presets_track_and_apply_local();
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
        rgb_presets_load_or_init();
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

    if (is_keyboard_master()) {
        sofle_wdt_enable();
    }
}

void eeconfig_init_user(void) {
    rgb_presets_init_defaults(true);
#ifdef RGB_MATRIX_ENABLE
    rgb_mode_preset_t p = rgb_presets_get(RGB_MATRIX_DEFAULT_MODE);
    tracked_rgb_mode    = RGB_MATRIX_DEFAULT_MODE;
    tracked_rgb_h       = p.h;
    tracked_rgb_s       = p.s;
    tracked_rgb_v       = p.v;
    tracked_rgb_speed   = p.speed;
#endif
}

bool shutdown_user(bool jump_to_bootloader) {
    (void)jump_to_bootloader;
    sofle_wdt_disable();
    rgb_presets_flush_if_dirty(true);
    return true;
}

#ifdef SOFLE_USB_RECOVERY_ENABLED
#    define USB_SUSPEND_RECOVER_NO_RWU_MS 300
#    define USB_SUSPEND_RECOVER_RWU_MS    1500
#    define USB_UNCONFIGURED_RECOVER_MS   3000
#    define USB_RESTART_COOLDOWN_MS       5000

static bool     suspend_key_was_pressed    = false;
static bool     suspend_wake_attempt_active = false;
static uint32_t suspend_wake_attempt_start = 0;
static bool     usb_unconfigured_active    = false;
static uint32_t usb_unconfigured_since     = 0;
static bool     last_usb_restart_valid     = false;
static uint32_t last_usb_restart_time      = 0;

static bool is_local_input_pressed(void) {
    if (!gpio_read_pin(USR_BTN_PIN)) {
        return true;
    }
    uint8_t row_offset = is_keyboard_left() ? 0 : 5;
    for (uint8_t r = 0; r < 5; r++) {
        if ((matrix_get_row((uint8_t)(r + row_offset)) & 0x3F) != 0) {
            return true;
        }
    }
    return false;
}

static bool sofle_usb_can_restart(void) {
    return !last_usb_restart_valid || timer_elapsed32(last_usb_restart_time) >= USB_RESTART_COOLDOWN_MS;
}

static void sofle_usb_trigger_restart(void) {
    suspend_wake_attempt_active = false;
    usb_unconfigured_active     = false;
    last_usb_restart_valid      = true;
    last_usb_restart_time       = timer_read32();
    sofle_usb_restart();
}
#endif

void suspend_power_down_user(void) {
    sofle_wdt_feed();
    if (!is_keyboard_master() || bootloader_pending) {
        return;
    }
#ifdef SOFLE_USB_RECOVERY_ENABLED
    sofle_usb_arm_sof_wakeup();
    bool pressed = is_local_input_pressed();
    if (pressed && !suspend_key_was_pressed && !suspend_wake_attempt_active) {
        suspend_wake_attempt_active = true;
        suspend_wake_attempt_start  = timer_read32();
    }
    suspend_key_was_pressed = pressed;

    if (suspend_wake_attempt_active && USB_STATE_IS_SUSPENDED()) {
        uint32_t delay_ms = USB_REMOTE_WAKEUP_ENABLED() ? USB_SUSPEND_RECOVER_RWU_MS : USB_SUSPEND_RECOVER_NO_RWU_MS;
        if (timer_elapsed32(suspend_wake_attempt_start) >= delay_ms && sofle_usb_can_restart()) {
            sofle_usb_trigger_restart();
        }
    }
#endif
}

void suspend_wakeup_init_user(void) {
#ifdef SOFLE_USB_RECOVERY_ENABLED
    suspend_wake_attempt_active = false;
    suspend_key_was_pressed     = false;
    usb_unconfigured_active     = false;
    sofle_usb_rearm_raw_out();
#endif
    if (!lights_disabled) {
        wake_local_displays_and_rgb();
    }
}

void notify_usb_device_state_change_user(struct usb_device_state state) {
    if (state.configure_state == USB_DEVICE_STATE_CONFIGURED) {
#ifdef SOFLE_USB_RECOVERY_ENABLED
        suspend_wake_attempt_active = false;
        suspend_key_was_pressed     = false;
        usb_unconfigured_active     = false;
#endif
        if (!lights_disabled) {
            wake_local_displays_and_rgb();
        }
    }
}

void housekeeping_task_user(void) {
    sofle_wdt_feed();
    if (is_keyboard_master()) {
        if (bootloader_pending && timer_elapsed32(bootloader_req_time) >= RAW_BOOTLOADER_DELAY_MS) {
            bootloader_pending = false;
            sofle_wdt_disable();
            rgb_presets_flush_if_dirty(true);
            oled_off();
            reset_keyboard();
            return;
        }
#ifdef SOFLE_USB_RECOVERY_ENABLED
        suspend_wake_attempt_active = false;
        suspend_key_was_pressed     = false;
        if (USB_STATE_IS_ACTIVE()) {
            usb_unconfigured_active = false;
#    ifdef RGB_MATRIX_ENABLE
            if (!lights_disabled && rgb_matrix_get_suspend_state()) {
                wake_local_displays_and_rgb();
            }
#    endif
        } else if (!USB_STATE_IS_SUSPENDED() && !bootloader_pending) {
            if (!usb_unconfigured_active) {
                usb_unconfigured_active = true;
                usb_unconfigured_since  = timer_read32();
            } else if (timer_elapsed32(usb_unconfigured_since) >= USB_UNCONFIGURED_RECOVER_MS && sofle_usb_can_restart()) {
                sofle_usb_trigger_restart();
                return;
            }
        }
#endif
        bool trrs_connected = is_transport_connected();
        if (trrs_connected) {
            clear_dual_usb_peer_state();
        } else if (host_service_connected && timer_elapsed32(host_service_last_rx) > HOST_SYNC_TIMEOUT_MS) {
            clear_host_sync_state();
        }
        // Caps Lock indicator on master half (TRRS master & both Dual-USB halves)
        gpio_write_pin(USR_LED_PIN, host_keyboard_led_state().caps_lock);
        if (host_status_active && timer_elapsed32(host_status_last_rx) > HOST_SYNC_TIMEOUT_MS) {
            host_audio_output  = HOST_AUDIO_UNKNOWN;
            host_status_active = false;
        }
        host_setup_task();

        static uint16_t poll_timer               = 0;
        static bool     last_master_btn          = false;
        static bool     last_slave_btn           = false;
        static uint16_t last_sent_keys[5][6]     = {{0}};
        static uint16_t last_sent_ccw            = 0xFFFF;
        static uint16_t last_sent_cw             = 0xFFFF;
        static uint16_t last_sent_peer_ccw       = 0xFFFF;
        static uint16_t last_sent_peer_cw        = 0xFFFF;
        static uint8_t  periodic_full_sync_count = 0;

        if (timer_elapsed(poll_timer) >= 20) {
            poll_timer = timer_read();
#ifdef SOFLE_USB_RECOVERY_ENABLED
            if (USB_STATE_IS_ACTIVE()) {
                sofle_usb_rearm_raw_out();
            }
#endif

            bool master_btn = !gpio_read_pin(USR_BTN_PIN);
            bool slave_btn  = false;

            // Left GP24 button toggles OLED & RGB lights;
            // Right GP24 button cycles OLED app/mode and wakes displays.
            if (master_btn && !last_master_btn) {
                if (is_keyboard_left()) {
                    toggle_lights_disabled();
                } else {
                    cycle_oled_app();
                    if (!lights_disabled) {
                        wake_local_displays_and_rgb();
                    }
                    if (!trrs_connected) {
                        host_sync_send_state(false, false, false, !lights_disabled, true);
                    }
                }
            }
            last_master_btn = master_btn;

            rgb_presets_track_and_apply_local();

            if (trrs_connected) {
                layer_state_t active_mask = layer_state | default_layer_state;
                uint8_t       top_layer   = get_highest_layer(active_mask);
                uint8_t       mods        = get_local_mods();
                bool          caps        = host_keyboard_led_state().caps_lock;
                uint8_t       hdr_flags   = 0;
                if (lights_disabled) hdr_flags |= SLAVE_HDR_LIGHTS_OFF;
                if (caps) hdr_flags |= SLAVE_HDR_CAPS_LOCK;
                if (!lights_disabled && last_input_activity_elapsed() < 2000) hdr_flags |= SLAVE_HDR_WAKE_ACTIVE;
                hdr_flags |= (uint8_t)((host_audio_output & 0x03) << SLAVE_HDR_AUDIO_SHIFT);

                slave_sync_packet_t pkt;
                pkt.hdr.flags     = hdr_flags;
                pkt.hdr.top_layer = top_layer;
                pkt.hdr.mods      = mods;
                pkt.hdr.oled_app  = (uint8_t)current_oled_app;

                bool slave_is_left = !is_keyboard_left();
                if (current_oled_app == OLED_APP_RGB_INFO && !slave_is_left) {
                    populate_rgb_right_panel(pkt.keys, &pkt.enc_ccw, &pkt.enc_cw, &pkt.peer_enc_ccw, &pkt.peer_enc_cw);
                } else {
                    populate_half_keys(slave_is_left, active_mask, top_layer, pkt.keys, &pkt.enc_ccw, &pkt.enc_cw);
                    pkt.peer_enc_ccw = KC_NO;
                    pkt.peer_enc_cw  = KC_NO;
                }

                bool keys_changed = (memcmp(pkt.keys, last_sent_keys, sizeof(pkt.keys)) != 0) || (pkt.enc_ccw != last_sent_ccw) || (pkt.enc_cw != last_sent_cw) || (pkt.peer_enc_ccw != last_sent_peer_ccw) || (pkt.peer_enc_cw != last_sent_peer_cw);
                periodic_full_sync_count++;

                if (keys_changed || periodic_full_sync_count >= 12) {
                    if (transaction_rpc_exec(USR_BTN_SYNC, sizeof(pkt), &pkt, sizeof(slave_btn), &slave_btn)) {
                        memcpy(last_sent_keys, pkt.keys, sizeof(last_sent_keys));
                        last_sent_ccw            = pkt.enc_ccw;
                        last_sent_cw             = pkt.enc_cw;
                        last_sent_peer_ccw       = pkt.peer_enc_ccw;
                        last_sent_peer_cw        = pkt.peer_enc_cw;
                        periodic_full_sync_count = 0;
                    }
                } else {
                    transaction_rpc_exec(USR_BTN_SYNC, sizeof(pkt.hdr), &pkt.hdr, sizeof(slave_btn), &slave_btn);
                }

                // Slave GP24 over TRRS: Left toggles OLED & RGB lights, Right cycles OLED app/mode.
                if (slave_btn && !last_slave_btn) {
                    if (slave_is_left) {
                        toggle_lights_disabled();
                    } else {
                        cycle_oled_app();
                        if (!lights_disabled) {
                            wake_local_displays_and_rgb();
                        }
                    }
                }
                last_slave_btn = slave_btn;
            } else {
                sync_local_state_if_needed();
            }

            rgb_presets_flush_if_dirty(false);
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
#    define CHAR_ICON_BULB       ((char)0x80)
#    define CHAR_ICON_SPEAKER    ((char)0x81)
#    define CHAR_ICON_HEADPHONES ((char)0x82)
#    define CHAR_ICON_TAB        ((char)0x83)
#    define CHAR_ICON_BSPC       ((char)0x84)
#    define CHAR_ICON_DEL        ((char)0x85)
#    define CHAR_ICON_ENTER      ((char)0x86)
#    define CHAR_ICON_SPACE      ((char)0x87)
#    define CHAR_ICON_SHIFT      ((char)0x88)
#    define CHAR_ICON_CAPS       ((char)0x89)
#    define CHAR_ICON_GUI        ((char)0x8A)
#    define CHAR_ICON_PLAY       ((char)0x8B)
#    define CHAR_ICON_STOP       ((char)0x8C)
#    define CHAR_ICON_PREV       ((char)0x8D)
#    define CHAR_ICON_NEXT       ((char)0x8E)
#    define CHAR_ICON_MUTE       ((char)0x8F)
#    define CHAR_ICON_VOLU       ((char)0x90)
#    define CHAR_ICON_VOLD       ((char)0x91)
#    define CHAR_ICON_PGUP       ((char)0x92)
#    define CHAR_ICON_PGDN       ((char)0x93)
#    define CHAR_ICON_SCRL_UP    ((char)0x94)
#    define CHAR_ICON_SCRL_DN    ((char)0x95)
#    define CHAR_ICON_PSCR       ((char)0x96)
#    define CHAR_ICON_MENU       ((char)0x97)
#    define CHAR_ICON_CALC       ((char)0x98)
#    define CHAR_ICON_HOME       ((char)0x99)
#    define CHAR_ICON_END        ((char)0x9A)
#    define CHAR_ICON_ALT        ((char)0x9B)
#    define CHAR_ICON_CTL        ((char)0x9C)
#    define CHAR_ICON_LOWER      ((char)0x9D)
#    define CHAR_ICON_RAISE      ((char)0x9E)
#    define CHAR_ICON_WORD_LEFT  ((char)0x9F)
#    define CHAR_ICON_WORD_RGHT  ((char)0xA0)
#    define CHAR_ICON_CLOSE      ((char)0xA1)
#    define CHAR_ICON_POWER      ((char)0xA2)
#    define CHAR_ICON_BRI_UP     ((char)0xA3)
#    define CHAR_ICON_BRI_DN     ((char)0xA4)
#    define CHAR_ICON_BOOT       ((char)0xA5)
#    define CHAR_ICON_EMOJI      ((char)0xA6)
#    define CHAR_ICON_DOWNLOAD   ((char)0xA7)
#    define CHAR_ICON_FIRST      CHAR_ICON_BULB
#    define CHAR_ICON_LAST       CHAR_ICON_DOWNLOAD
#    define CHAR_ICON_COUNT      ((uint8_t)CHAR_ICON_LAST - (uint8_t)CHAR_ICON_FIRST + 1U)

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

static const uint16_t PROGMEM oled_icons7x9[CHAR_ICON_COUNT][7] = {
    {0x00C, 0x012, 0x0E1, 0x1A1, 0x0E1, 0x012, 0x00C}, // 0x05 CHAR_ICON_BULB
    {0x038, 0x038, 0x07C, 0x0FE, 0x000, 0x082, 0x07C}, // 0x06 CHAR_ICON_SPEAKER
    {0x078, 0x0F4, 0x002, 0x002, 0x002, 0x0F4, 0x078}, // 0x07 CHAR_ICON_HEADPHONES
    {0x04E, 0x040, 0x044, 0x0EE, 0x044, 0x004, 0x0E4}, // 0x08 CHAR_ICON_TAB (|<- / ->|)
    {0x010, 0x038, 0x07C, 0x054, 0x06C, 0x054, 0x07C}, // 0x09 CHAR_ICON_BSPC (<x] filled tag)
    {0x07C, 0x054, 0x06C, 0x054, 0x07C, 0x038, 0x010}, // 0x0A CHAR_ICON_DEL ([x> filled tag)
    {0x020, 0x070, 0x0A8, 0x020, 0x020, 0x020, 0x03E}, // 0x0B CHAR_ICON_ENTER (bent return arrow)
    {0x070, 0x040, 0x040, 0x040, 0x040, 0x040, 0x070}, // 0x0C CHAR_ICON_SPACE (low U bracket)
    {0x010, 0x018, 0x0F4, 0x082, 0x0F4, 0x018, 0x010}, // 0x0D CHAR_ICON_SHIFT (hollow up arrow)
    {0x008, 0x00C, 0x0BA, 0x0A1, 0x0BA, 0x00C, 0x008}, // 0x0E CHAR_ICON_CAPS (hollow arrow + lock bar)
    {0x0EE, 0x0EE, 0x0EE, 0x000, 0x0EE, 0x0EE, 0x0EE}, // 0x0F CHAR_ICON_GUI (4-pane Super/Win grid)
    {0x07C, 0x038, 0x010, 0x000, 0x07C, 0x000, 0x07C}, // 0x10 CHAR_ICON_PLAY (play/pause)
    {0x000, 0x07C, 0x07C, 0x07C, 0x07C, 0x07C, 0x000}, // 0x11 CHAR_ICON_STOP (5x5 square)
    {0x000, 0x07C, 0x000, 0x010, 0x038, 0x07C, 0x000}, // 0x12 CHAR_ICON_PREV (|<)
    {0x000, 0x07C, 0x038, 0x010, 0x000, 0x07C, 0x000}, // 0x13 CHAR_ICON_NEXT (>|)
    {0x038, 0x07C, 0x0FE, 0x000, 0x028, 0x010, 0x028}, // 0x14 CHAR_ICON_MUTE (speaker + x)
    {0x038, 0x07C, 0x0FE, 0x000, 0x010, 0x038, 0x010}, // 0x15 CHAR_ICON_VOLU (speaker + +)
    {0x038, 0x07C, 0x0FE, 0x000, 0x010, 0x010, 0x010}, // 0x16 CHAR_ICON_VOLD (speaker + -)
    {0x000, 0x088, 0x044, 0x022, 0x044, 0x088, 0x000}, // 0x17 CHAR_ICON_PGUP (double up chevron)
    {0x000, 0x022, 0x044, 0x088, 0x044, 0x022, 0x000}, // 0x18 CHAR_ICON_PGDN (double down chevron)
    {0x000, 0x0F0, 0x10A, 0x13B, 0x10A, 0x0F0, 0x000}, // 0x19 CHAR_ICON_SCRL_UP (arrow + mouse)
    {0x000, 0x01E, 0x0A1, 0x1A7, 0x0A1, 0x01E, 0x000}, // 0x1A CHAR_ICON_SCRL_DN (mouse + arrow)
    {0x0F8, 0x0F8, 0x08C, 0x0AC, 0x08C, 0x0F8, 0x0F8}, // 0x1B CHAR_ICON_PSCR (camera)
    {0x092, 0x000, 0x092, 0x092, 0x092, 0x092, 0x092}, // 0x1C CHAR_ICON_MENU (bulleted 3-line menu)
    {0x0FE, 0x101, 0x155, 0x105, 0x155, 0x101, 0x0FE}, // 0x1D CHAR_ICON_CALC (calculator)
    {0x07C, 0x000, 0x010, 0x038, 0x054, 0x010, 0x010}, // 0x99 CHAR_ICON_HOME (|<-- arrow to bar)
    {0x010, 0x010, 0x054, 0x038, 0x010, 0x000, 0x07C}, // 0x9A CHAR_ICON_END (-->| arrow to bar)
    {0x004, 0x004, 0x008, 0x010, 0x024, 0x044, 0x044}, // 0x9B CHAR_ICON_ALT (Option switch)
    {0x040, 0x060, 0x030, 0x018, 0x030, 0x060, 0x040}, // 0x9C CHAR_ICON_CTL (bold caret)
    {0x008, 0x018, 0x038, 0x078, 0x038, 0x018, 0x008}, // 0x9D CHAR_ICON_LOWER (solid down triangle)
    {0x040, 0x060, 0x070, 0x078, 0x070, 0x060, 0x040}, // 0x9E CHAR_ICON_RAISE (solid up triangle)
    {0x010, 0x028, 0x044, 0x000, 0x010, 0x028, 0x044}, // 0x9F CHAR_ICON_WORD_LEFT (double left chevron)
    {0x044, 0x028, 0x010, 0x000, 0x044, 0x028, 0x010}, // 0xA0 CHAR_ICON_WORD_RGHT (double right chevron)
    {0x0FE, 0x082, 0x0AA, 0x092, 0x0AA, 0x082, 0x0FE}, // 0xA1 CHAR_ICON_CLOSE (boxed X)
    {0x038, 0x044, 0x080, 0x09E, 0x080, 0x044, 0x038}, // 0xA2 CHAR_ICON_POWER (7x7 power symbol)
    {0x010, 0x044, 0x038, 0x0BA, 0x038, 0x044, 0x010}, // 0xA3 CHAR_ICON_BRI_UP (7x7 bright 8-ray solid sun)
    {0x010, 0x000, 0x038, 0x0AA, 0x038, 0x000, 0x010}, // 0xA4 CHAR_ICON_BRI_DN (7x7 dim 4-ray hollow sun)
    {0x028, 0x07C, 0x0C6, 0x054, 0x0C6, 0x07C, 0x028}, // 0xA5 CHAR_ICON_BOOT (7x7 QFP microchip)
    {0x038, 0x07C, 0x0D6, 0x0BE, 0x0D6, 0x07C, 0x038}, // 0xA6 CHAR_ICON_EMOJI (7x7 smiley face for Win+.)
    {0x0C0, 0x088, 0x090, 0x0BE, 0x090, 0x088, 0x0C0}, // 0xA7 CHAR_ICON_DOWNLOAD (download to tray)
};

static const char PROGMEM oled_icon_ascii_tags[CHAR_ICON_COUNT][2] = {
    {'L', 'T'}, {'S', 'P'}, {'H', 'P'}, {'T', 'B'}, {'B', 'S'}, {'D', 'L'},
    {'E', 'N'}, {'S', 'P'}, {'S', 'F'}, {'C', 'P'}, {'G', 'U'}, {'P', 'L'},
    {'S', 'T'}, {'<', '<'}, {'>', '>'}, {'M', 'U'}, {'V', '+'}, {'V', '-'},
    {'P', 'U'}, {'P', 'D'}, {'W', 'U'}, {'W', 'D'}, {'P', 'S'}, {'A', 'P'},
    {'C', 'L'}, {'H', 'M'}, {'E', 'D'}, {'A', 'L'}, {'C', 'T'}, {'L', 'W'},
    {'R', 'S'}, {'^', '<'}, {'^', '>'}, {'!', '4'}, {'R', 'G'}, {'B', '+'},
    {'B', '-'}, {'B', 'T'}, {'G', '.'}, {'D', 'N'},
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
        case '%': out[0] = 0x61; out[1] = 0x1C; out[2] = 0x43; break;
        case ':': out[0] = 0x00; out[1] = 0x14; out[2] = 0x00; break;
        case '#': out[0] = 0x14; out[1] = 0x7F; out[2] = 0x14; break;
        case '.': out[0] = 0x00; out[1] = 0x60; out[2] = 0x00; break;
        case ' ': out[0] = 0x00; out[1] = 0x00; out[2] = 0x00; break;
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
    char c = basic_to_ascii(kc, false, true);
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

static void format_keycode_label_ex(uint16_t kc, bool shift, bool caps, uint8_t audio_output, char out[4]) {
    (void)audio_output;
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

        if (has_alt && !has_ctl && !has_gui && !has_sft && base == KC_F4) {
            out[0] = CHAR_ICON_CLOSE;
            return;
        }
        if (has_ctl && !has_alt && !has_gui && !has_sft && base == KC_LEFT) {
            out[0] = CHAR_ICON_WORD_LEFT;
            return;
        }
        if (has_ctl && !has_alt && !has_gui && !has_sft && base == KC_RGHT) {
            out[0] = CHAR_ICON_WORD_RGHT;
            return;
        }
        if (has_gui && !has_ctl && !has_alt && !has_sft && base == KC_DOT) {
            out[0] = CHAR_ICON_EMOJI;
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
        out[0] = CHAR_ICON_LOWER;
        return;
    }
    if (kc == TL_UPPR) {
        out[0] = CHAR_ICON_RAISE;
        return;
    }
    if (IS_QK_MOMENTARY(kc)) {
        uint8_t l = kc & 0x1F;
        if (l == 1) {
            out[0] = CHAR_ICON_LOWER;
            return;
        }
        if (l == 2) {
            out[0] = CHAR_ICON_RAISE;
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
            out[0] = CHAR_ICON_SHIFT;
            return;
        }
        if (m & 0x01) {
            out[0] = CHAR_ICON_CTL;
            return;
        }
        if (m & 0x04) {
            out[0] = CHAR_ICON_ALT;
            return;
        }
        out[0] = CHAR_ICON_GUI;
        return;
    }
    if (IS_QK_MACRO(kc)) {
        uint8_t idx = (kc - QK_MACRO) & 0x0F;
        out[0]      = 'M';
        out[1]      = (idx < 10) ? (char)('0' + idx) : (char)('A' + (idx - 10));
        return;
    }

    switch (kc) {
        case KC_TAB:  out[0] = CHAR_ICON_TAB;   return;
        case KC_ESC:  out[0] = 'E'; out[1] = 'S'; return;
        case KC_BSPC: out[0] = CHAR_ICON_BSPC;  return;
        case KC_DEL:  out[0] = CHAR_ICON_DEL;   return;
        case KC_SPC:  out[0] = CHAR_ICON_SPACE; return;
        case KC_ENT:
        case KC_PENT: out[0] = CHAR_ICON_ENTER; return;
        case KC_LSFT:
        case KC_RSFT: out[0] = CHAR_ICON_SHIFT; return;
        case KC_LCTL:
        case KC_RCTL: out[0] = CHAR_ICON_CTL;   return;
        case KC_LALT:
        case KC_RALT: out[0] = CHAR_ICON_ALT;   return;
        case KC_LGUI:
        case KC_RGUI: out[0] = CHAR_ICON_GUI;   return;
        case KC_INS:  out[0] = 'I'; out[1] = 'N'; return;
        case KC_PSCR: out[0] = CHAR_ICON_PSCR;  return;
        case KC_SCRL: out[0] = 'S'; out[1] = 'L'; return;
        case KC_PAUS: out[0] = 'P'; out[1] = 'A'; return;
        case KC_APP:  out[0] = CHAR_ICON_MENU;  return;
        case KC_CAPS: out[0] = CHAR_ICON_CAPS;  return;
        case KC_CALC: out[0] = CHAR_ICON_CALC;  return;
        case KC_HOME: out[0] = CHAR_ICON_HOME;  return;
        case KC_END:  out[0] = CHAR_ICON_END;   return;
        case KC_PGUP: out[0] = CHAR_ICON_PGUP;  return;
        case KC_PGDN: out[0] = CHAR_ICON_PGDN;  return;
        case KC_UP:   out[0] = CHAR_ARROW_UP;   return;
        case KC_DOWN: out[0] = CHAR_ARROW_DOWN; return;
        case KC_LEFT: out[0] = CHAR_ARROW_LEFT; return;
        case KC_RGHT: out[0] = CHAR_ARROW_RGHT; return;
        case KC_MPLY: out[0] = CHAR_ICON_PLAY;  return;
        case KC_MSTP: out[0] = CHAR_ICON_STOP;  return;
        case KC_MUTE:
        case KC_KB_MUTE: out[0] = CHAR_ICON_MUTE; return;
        case KC_VOLU:
        case KC_KB_VOLUME_UP: out[0] = CHAR_ICON_VOLU; return;
        case KC_VOLD:
        case KC_KB_VOLUME_DOWN: out[0] = CHAR_ICON_VOLD; return;
        case KC_MNXT:
        case KC_MFFD: out[0] = CHAR_ICON_NEXT;  return;
        case KC_MPRV:
        case KC_MRWD: out[0] = CHAR_ICON_PREV;  return;
        case KC_BRIU: out[0] = CHAR_ICON_BRI_UP; return;
        case KC_BRID: out[0] = CHAR_ICON_BRI_DN; return;
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
        case KC_MENU: out[0] = CHAR_ICON_MENU;  return;
        case KC_PEQL: out[0] = '='; return;
        case KC_PCMM: out[0] = ','; return;
        case MS_WHLU: out[0] = CHAR_ICON_SCRL_UP; return;
        case MS_WHLD: out[0] = CHAR_ICON_SCRL_DN; return;
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
        case RM_TOGG: out[0] = CHAR_ICON_POWER;  return;
        case RM_NEXT: out[0] = 'R'; out[1] = '+'; return;
        case RM_PREV: out[0] = 'R'; out[1] = '-'; return;
        case RM_HUEU: out[0] = 'H'; out[1] = '+'; return;
        case RM_HUED: out[0] = 'H'; out[1] = '-'; return;
        case RM_SATU: out[0] = 'S'; out[1] = '+'; return;
        case RM_SATD: out[0] = 'S'; out[1] = '-'; return;
        case RM_VALU: out[0] = CHAR_ICON_BRI_UP; return;
        case RM_VALD: out[0] = CHAR_ICON_BRI_DN; return;
        case RM_SPDU: out[0] = 'P'; out[1] = '+'; return;
        case RM_SPDD: out[0] = 'P'; out[1] = '-'; return;
        case HA_HEX_TOGG: out[0] = CHAR_ICON_BULB; return;
        case WIN_AUD_SWP:
            out[0] = (audio_output == HOST_AUDIO_SPEAKER) ? CHAR_ICON_HEADPHONES : CHAR_ICON_SPEAKER;
            return;
        case HOST_SYNC_SETUP:
            out[0] = CHAR_ICON_DOWNLOAD;
            return;
        case QK_BOOT: out[0] = CHAR_ICON_BOOT;  return;
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

static void format_keycode_label(uint16_t kc, bool shift, bool caps, char out[4]) {
    format_keycode_label_ex(kc, shift, caps, HOST_AUDIO_UNKNOWN, out);
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

static void fb_draw_cols9(uint8_t *fb, uint8_t x, uint8_t y, const uint16_t *cols, uint8_t width, bool invert) {
    for (uint8_t dx = 0; dx < width; dx++) {
        uint16_t col_bits = cols[dx];
        for (uint8_t dy = 0; dy < 9; dy++) {
            bool bit = (col_bits & (1U << dy)) != 0;
            fb_set_pixel(fb, (uint8_t)(x + dx), (uint8_t)(y + dy), invert ? !bit : bit);
        }
    }
}

static void fb_fill_rect(uint8_t *fb, uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on) {
    for (uint8_t dy = 0; dy < h; dy++) {
        for (uint8_t dx = 0; dx < w; dx++) {
            fb_set_pixel(fb, (uint8_t)(x + dx), (uint8_t)(y + dy), on);
        }
    }
}

static void fb_draw_rect(uint8_t *fb, uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    if (w == 0 || h == 0) return;
    for (uint8_t dx = 0; dx < w; dx++) {
        fb_set_pixel(fb, (uint8_t)(x + dx), y, true);
        fb_set_pixel(fb, (uint8_t)(x + dx), (uint8_t)(y + h - 1), true);
    }
    for (uint8_t dy = 0; dy < h; dy++) {
        fb_set_pixel(fb, x, (uint8_t)(y + dy), true);
        fb_set_pixel(fb, (uint8_t)(x + w - 1), (uint8_t)(y + dy), true);
    }
}

static void fb_draw_hline(uint8_t *fb, uint8_t x, uint8_t y, uint8_t w, bool dotted) {
    uint8_t step = dotted ? 2 : 1;
    for (uint8_t dx = 0; dx < w; dx = (uint8_t)(dx + step)) {
        fb_set_pixel(fb, (uint8_t)(x + dx), y, true);
    }
}

static uint8_t fb_measure_text3x7(const char *str) {
    uint8_t w = 0;
    uint8_t g4[4];
    for (uint8_t i = 0; str[i] != '\0'; i++) {
        char ch = str[i];
        if (i > 0 && ch != ' ' && str[i - 1] != ' ') {
            w = (uint8_t)(w + 1);
        }
        if (ch == ' ') {
            w = (uint8_t)(w + 2);
        } else if (ch == ':') {
            w = (uint8_t)(w + 1);
        } else {
            if (ch >= 'a' && ch <= 'z') {
                ch = (char)(ch - 'a' + 'A');
            }
            w = (uint8_t)(w + (get_glyph4x7_wide(ch, g4) ? 4 : 3));
        }
    }
    return w;
}

static void fb_draw_text3x7(uint8_t *fb, uint8_t x, uint8_t y, const char *str, bool invert) {
    uint8_t cx = x;
    for (uint8_t i = 0; str[i] != '\0'; i++) {
        char ch = str[i];
        if (i > 0 && ch != ' ' && str[i - 1] != ' ') {
            if (invert) {
                for (uint8_t dy = 0; dy < 7; dy++) {
                    fb_set_pixel(fb, cx, (uint8_t)(y + dy), true);
                }
            }
            cx = (uint8_t)(cx + 1);
        }
        if (ch == ' ') {
            if (invert) {
                fb_fill_rect(fb, cx, y, 2, 7, true);
            }
            cx = (uint8_t)(cx + 2);
        } else if (ch == ':') {
            uint8_t colon = 0x14;
            fb_draw_cols7(fb, cx, y, &colon, 1, invert);
            cx = (uint8_t)(cx + 1);
        } else {
            if (ch >= 'a' && ch <= 'z') {
                ch = (char)(ch - 'a' + 'A');
            }
            uint8_t g[4];
            uint8_t gw = 3;
            if (get_glyph4x7_wide(ch, g)) {
                gw = 4;
            } else {
                get_glyph3x7(ch, g);
            }
            fb_draw_cols7(fb, cx, y, g, gw, invert);
            cx = (uint8_t)(cx + gw);
        }
    }
}

static void format_uint_str(uint8_t val, char *out, uint8_t min_digits) {
    char    tmp[4];
    uint8_t n = 0;
    do {
        tmp[n++] = (char)('0' + (val % 10U));
        val      = (uint8_t)(val / 10U);
    } while (val > 0 || n < min_digits);
    for (uint8_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
}

static void fb_draw_bar(uint8_t *fb, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t val, uint8_t max_val) {
    fb_draw_rect(fb, x, y, w, h);
    if (w > 4 && h > 4 && max_val > 0) {
        uint8_t clamped = (val > max_val) ? max_val : val;
        uint8_t inner_w = (uint8_t)(w - 4);
        uint8_t fill_w  = (uint8_t)(((uint16_t)clamped * inner_w + (max_val / 2U)) / max_val);
        if (clamped > 0 && fill_w == 0) {
            fill_w = 1;
        }
        if (fill_w > 0) {
            fb_fill_rect(fb, (uint8_t)(x + 2), (uint8_t)(y + 2), fill_w, (uint8_t)(h - 4), true);
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
        if (c >= (uint8_t)CHAR_ICON_FIRST && c <= (uint8_t)CHAR_ICON_LAST) {
            uint16_t icon[7];
            uint8_t  idx = (uint8_t)(c - (uint8_t)CHAR_ICON_FIRST);
            for (uint8_t i = 0; i < 7; i++) {
                icon[i] = pgm_read_word(&oled_icons7x9[idx][i]);
            }
            fb_draw_cols9(fb, (uint8_t)(cx + 1), (uint8_t)(cy + 2), icon, 7, active);
            return;
        }
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

static const uint8_t oled_col_x[6]     = {0, 11, 22, 33, 44, 55};
static const uint8_t oled_stagger_l[6] = {4, 4, 2, 0, 2, 3};
static const uint8_t oled_stagger_r[6] = {3, 2, 0, 2, 4, 4};

static void format_layer_conn_header(uint8_t top_layer, oled_conn_mode_t conn_mode, char hdr[7]) {
    static const char PROGMEM conn_names[4][5] = {
        "TRRS",
        "SYNC",
        "WAIT",
        "N/C",
    };
    const char *cname = (conn_mode <= OLED_CONN_DISC) ? conn_names[conn_mode] : "N/C";
    hdr[0]            = (top_layer < 10) ? (char)('0' + top_layer) : '?';
    hdr[1]            = ':';
    uint8_t hlen      = 2;
    for (uint8_t i = 0; i < 4; i++) {
        char ch = (conn_mode <= OLED_CONN_DISC) ? (char)pgm_read_byte(&cname[i]) : cname[i];
        if (ch == '\0') break;
        hdr[hlen++] = ch;
    }
    hdr[hlen] = '\0';
}

static void render_oled_layout(uint8_t *fb, bool is_left, uint8_t top_layer, oled_conn_mode_t conn_mode, uint8_t mods, bool caps, uint8_t audio_output, const uint8_t pressed_rows[5], const uint16_t keys[5][6], uint16_t enc_ccw, uint16_t enc_cw) {
    memset(fb, 0, OLED_MATRIX_SIZE);

    bool shift = (mods & MOD_MASK_SHIFT) != 0;
    bool ctrl  = (mods & MOD_MASK_CTRL) != 0;
    bool alt   = (mods & MOD_MASK_ALT) != 0;
    bool gui   = (mods & MOD_MASK_GUI) != 0;

    char hdr[7];
    format_layer_conn_header(top_layer, conn_mode, hdr);
    fb_draw_text3x7(fb, 0, 1, hdr, false);

    const char mod_chars[5]  = {'S', 'C', 'A', 'G', 'L'};
    const bool mod_active[5] = {shift, ctrl, alt, gui, caps};
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t bx = (uint8_t)(34 + i * 6);
        uint8_t g[3];
        get_glyph3x7(mod_chars[i], g);
        if (mod_active[i]) {
            fb_fill_rect(fb, bx, 0, 5, 9, true);
            fb_draw_cols7(fb, (uint8_t)(bx + 1), 1, g, 3, true);
        } else {
            fb_draw_cols7(fb, (uint8_t)(bx + 1), 1, g, 3, false);
        }
    }

    fb_draw_hline(fb, 0, 10, OLED_PORTRAIT_W, false);

    static const int8_t thumb_l[6] = {2, 1, 0, 1, 2, -2};
    static const int8_t thumb_r[6] = {-2, 2, 1, 0, 1, 2};

    for (uint8_t r = 0; r < 5; r++) {
        for (uint8_t disp_c = 0; disp_c < 6; disp_c++) {
            uint8_t  mat_c   = is_left ? disp_c : (uint8_t)(5 - disp_c);
            uint16_t kc      = keys[r][mat_c];
            char     lbl[4];
            format_keycode_label_ex(kc, shift, caps, audio_output, lbl);
            bool    pressed = (pressed_rows[r] & (1U << mat_c)) != 0;
            bool    act     = pressed || is_kc_active_pill(kc, top_layer, mods, caps);
            uint8_t cx      = oled_col_x[disp_c];
            bool    is_knob = (r == 4 && mat_c == 5);
            uint8_t cy;
            if (r < 4) {
                uint8_t stg = is_left ? oled_stagger_l[disp_c] : oled_stagger_r[disp_c];
                cy          = (uint8_t)(13 + r * 17 + stg);
            } else {
                int8_t arc = is_left ? thumb_l[disp_c] : thumb_r[disp_c];
                cy         = (uint8_t)(89 + arc);
            }
            fb_draw_cell(fb, cx, cy, lbl, act, is_knob, kc == KC_NO);
        }
    }

    fb_draw_hline(fb, 0, 109, OLED_PORTRAIT_W, true);

    char lbl_ccw[4], lbl_cw[4];
    format_keycode_label_ex(enc_ccw, shift, caps, audio_output, lbl_ccw);
    format_keycode_label_ex(enc_cw, shift, caps, audio_output, lbl_cw);

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
    oled_app_t       app;
    bool             is_left;
    uint8_t          top_layer;
    oled_conn_mode_t conn_mode;
    uint8_t          mods;
    bool             caps;
    uint8_t          host_audio_output;
    uint8_t          pressed_rows[5];
    uint16_t         keys[5][6];
    uint16_t         enc_ccw;
    uint16_t         enc_cw;
    uint16_t         peer_enc_ccw;
    uint16_t         peer_enc_cw;
    uint8_t          rgb_enable;
    uint8_t          rgb_mode;
    uint8_t          rgb_h;
    uint8_t          rgb_s;
    uint8_t          rgb_v;
    uint8_t          rgb_speed;
} oled_app_ctx_t;

typedef void (*oled_app_render_fn)(uint8_t *fb, const oled_app_ctx_t *ctx);

static void copy_short_str(char *dst, const char *src, uint8_t max_len) {
    uint8_t i = 0;
    for (; i + 1 < max_len && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void set_mode_lines(char line1[15], char line2[15], const char *l1, const char *l2) {
    copy_short_str(line1, l1, 15);
    copy_short_str(line2, l2, 15);
}

static void get_rgb_mode_name_lines(uint8_t mode, char line1[15], char line2[15]) {
#ifdef RGB_MATRIX_ENABLE
    switch (mode) {
        case RGB_MATRIX_SOLID_COLOR:
            set_mode_lines(line1, line2, "SOLID", "COLOR");
            return;
#    ifdef ENABLE_RGB_MATRIX_GRADIENT_UP_DOWN
        case RGB_MATRIX_GRADIENT_UP_DOWN:
            set_mode_lines(line1, line2, "GRADIENT", "UP/DOWN");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_GRADIENT_UP_DOWN_VAL
        case RGB_MATRIX_GRADIENT_UP_DOWN_VAL:
            set_mode_lines(line1, line2, "BRIGHTNESS", "GRADIENT");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_GRADIENT_LEFT_RIGHT
        case RGB_MATRIX_GRADIENT_LEFT_RIGHT:
            set_mode_lines(line1, line2, "GRADIENT", "L/R");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_RAINBOW_MOVING_CHEVRON
        case RGB_MATRIX_RAINBOW_MOVING_CHEVRON:
            set_mode_lines(line1, line2, "RAINBOW", "CHEVRON");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_BREATHING
        case RGB_MATRIX_BREATHING:
            set_mode_lines(line1, line2, "BREATHING", "");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_CYCLE_ALL
        case RGB_MATRIX_CYCLE_ALL:
            set_mode_lines(line1, line2, "CYCLE", "ALL");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_CYCLE_LEFT_RIGHT
        case RGB_MATRIX_CYCLE_LEFT_RIGHT:
            set_mode_lines(line1, line2, "CYCLE", "L/R");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_CYCLE_UP_DOWN
        case RGB_MATRIX_CYCLE_UP_DOWN:
            set_mode_lines(line1, line2, "CYCLE", "UP/DOWN");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_CYCLE_OUT_IN
        case RGB_MATRIX_CYCLE_OUT_IN:
            set_mode_lines(line1, line2, "CYCLE", "OUT/IN");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_CYCLE_PINWHEEL
        case RGB_MATRIX_CYCLE_PINWHEEL:
            set_mode_lines(line1, line2, "CYCLE", "PINWHEEL");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_CYCLE_SPIRAL
        case RGB_MATRIX_CYCLE_SPIRAL:
            set_mode_lines(line1, line2, "CYCLE", "SPIRAL");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_HUE_BREATHING
        case RGB_MATRIX_HUE_BREATHING:
            set_mode_lines(line1, line2, "HUE", "BREATH");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_HUE_PENDULUM
        case RGB_MATRIX_HUE_PENDULUM:
            set_mode_lines(line1, line2, "HUE", "PENDUL");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_HUE_WAVE
        case RGB_MATRIX_HUE_WAVE:
            set_mode_lines(line1, line2, "HUE", "WAVE");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_RAINDROPS
        case RGB_MATRIX_RAINDROPS:
            set_mode_lines(line1, line2, "RAINDROP", "");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_TYPING_HEATMAP
        case RGB_MATRIX_TYPING_HEATMAP:
            set_mode_lines(line1, line2, "TYPING", "HEATMAP");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_DIGITAL_RAIN
        case RGB_MATRIX_DIGITAL_RAIN:
            set_mode_lines(line1, line2, "DIGITAL", "RAIN");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_SOLID_REACTIVE_SIMPLE
        case RGB_MATRIX_SOLID_REACTIVE_SIMPLE:
            set_mode_lines(line1, line2, "REACTIVE", "SIMPLE");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_SOLID_REACTIVE
        case RGB_MATRIX_SOLID_REACTIVE:
            set_mode_lines(line1, line2, "SOLID", "REACTIVE");
            return;
#    endif
#    ifdef ENABLE_RGB_MATRIX_SPLASH
        case RGB_MATRIX_SPLASH:
            set_mode_lines(line1, line2, "SPLASH", "");
            return;
#    endif
        default:
            break;
    }
#endif
    copy_short_str(line1, "MODE", 15);
    line2[0] = '#';
    format_uint_str(mode, line2 + 1, 1);
}

static const char *get_hue_color_tag(uint8_t h) {
    if (h <= 14 || h >= 241) return "RED";
    if (h <= 35) return "ORG";
    if (h <= 56) return "YEL";
    if (h <= 105) return "GRN";
    if (h <= 148) return "CYN";
    if (h <= 190) return "BLU";
    if (h <= 220) return "PUR";
    return "PNK";
}

static void format_ratio_str(uint8_t a, uint8_t b, char out[10]) {
    format_uint_str(a, out, 1);
    uint8_t len = (uint8_t)strlen(out);
    out[len++]  = '/';
    format_uint_str(b, out + len, 1);
}

static void render_oled_rgb_left(uint8_t *fb, const oled_app_ctx_t *ctx) {
    memset(fb, 0, OLED_MATRIX_SIZE);

    fb_draw_text3x7(fb, 0, 1, "RGB", false);
    if (ctx->rgb_enable) {
        uint8_t on_w = (uint8_t)(fb_measure_text3x7("ON") + 4);
        fb_fill_rect(fb, 15, 0, on_w, 9, true);
        fb_draw_text3x7(fb, 17, 1, "ON", true);
    } else {
        uint8_t off_w = (uint8_t)(fb_measure_text3x7("OFF") + 4);
        fb_draw_rect(fb, 15, 0, off_w, 9);
        fb_draw_text3x7(fb, 17, 1, "OFF", false);
    }

    char page[10];
    format_ratio_str((uint8_t)(ctx->app + 1), OLED_APP_COUNT, page);
    uint8_t pw = fb_measure_text3x7(page);
    fb_draw_text3x7(fb, (uint8_t)(OLED_PORTRAIT_W - pw), 1, page, false);

    fb_draw_hline(fb, 0, 10, OLED_PORTRAIT_W, false);

    fb_draw_text3x7(fb, 0, 13, "MODE", false);
#ifdef RGB_MATRIX_ENABLE
    uint8_t total_modes = (RGB_MATRIX_EFFECT_MAX > 1) ? (uint8_t)(RGB_MATRIX_EFFECT_MAX - 1) : 1;
#else
    uint8_t total_modes = 1;
#endif
    char mode_ratio[10];
    format_ratio_str(ctx->rgb_mode, total_modes, mode_ratio);
    uint8_t mrw = fb_measure_text3x7(mode_ratio);
    fb_draw_text3x7(fb, (uint8_t)(OLED_PORTRAIT_W - mrw), 13, mode_ratio, false);

    fb_draw_rect(fb, 0, 21, OLED_PORTRAIT_W, 21);
    char l1[15], l2[15];
    get_rgb_mode_name_lines(ctx->rgb_mode, l1, l2);
    if (l2[0] != '\0') {
        uint8_t w1 = fb_measure_text3x7(l1);
        uint8_t w2 = fb_measure_text3x7(l2);
        fb_draw_text3x7(fb, (OLED_PORTRAIT_W > w1) ? (uint8_t)((OLED_PORTRAIT_W - w1) / 2) : 1, 23, l1, false);
        fb_draw_text3x7(fb, (OLED_PORTRAIT_W > w2) ? (uint8_t)((OLED_PORTRAIT_W - w2) / 2) : 1, 32, l2, false);
    } else {
        uint8_t w1 = fb_measure_text3x7(l1);
        fb_draw_text3x7(fb, (OLED_PORTRAIT_W > w1) ? (uint8_t)((OLED_PORTRAIT_W - w1) / 2) : 1, 28, l1, false);
    }

#ifdef RGB_MATRIX_ENABLE
    uint8_t bri_max = (RGB_MATRIX_MAXIMUM_BRIGHTNESS > 0) ? RGB_MATRIX_MAXIMUM_BRIGHTNESS : 255;
#else
    uint8_t bri_max = 255;
#endif
    const char *const labels[4] = {"HUE", "SAT", "BRI", "SPD"};
    const uint8_t     vals[4]   = {ctx->rgb_h, ctx->rgb_s, ctx->rgb_v, ctx->rgb_speed};
    const uint8_t     maxs[4]   = {255, 255, bri_max, 255};
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t ry = (uint8_t)(45 + i * 18);
        fb_draw_text3x7(fb, 0, ry, labels[i], false);

        char mid[6];
        if (i == 0) {
            copy_short_str(mid, get_hue_color_tag(vals[0]), sizeof(mid));
        } else {
            uint16_t clamped = (vals[i] > maxs[i]) ? maxs[i] : vals[i];
            uint8_t  pct     = (uint8_t)(((uint32_t)clamped * 100U + (maxs[i] / 2U)) / maxs[i]);
            format_uint_str(pct, mid, 1);
            uint8_t len  = (uint8_t)strlen(mid);
            mid[len]     = '%';
            mid[len + 1] = '\0';
        }
        uint8_t mw = fb_measure_text3x7(mid);
        fb_draw_text3x7(fb, (uint8_t)(47 - mw), ry, mid, false);

        char raw_s[5];
        format_uint_str(vals[i], raw_s, 1);
        uint8_t rw = fb_measure_text3x7(raw_s);
        fb_draw_text3x7(fb, (uint8_t)(OLED_PORTRAIT_W - rw), ry, raw_s, false);

        fb_draw_bar(fb, 0, (uint8_t)(ry + 8), OLED_PORTRAIT_W, 7, vals[i], maxs[i]);
    }

    fb_draw_hline(fb, 0, 117, OLED_PORTRAIT_W, true);
    char hdr[7];
    format_layer_conn_header(ctx->top_layer, ctx->conn_mode, hdr);
    fb_draw_text3x7(fb, 0, 120, hdr, false);
    uint8_t aw = fb_measure_text3x7("R:APP");
    fb_draw_text3x7(fb, (uint8_t)(OLED_PORTRAIT_W - aw), 120, "R:APP", false);
}

static void append_sanitized_lbl(char *dst, uint8_t *pos, uint8_t max_len, const char lbl[4]) {
    if (lbl[0] == '\0') {
        if (*pos + 2 < max_len) {
            dst[(*pos)++] = '-';
            dst[(*pos)++] = '-';
        }
        return;
    }
    for (uint8_t i = 0; i < 3 && lbl[i] != '\0' && (*pos + 1) < max_len; i++) {
        char ch = lbl[i];
        if (ch == CHAR_ARROW_UP) {
            ch = '^';
        } else if (ch == CHAR_ARROW_DOWN) {
            ch = 'V';
        } else if (ch == CHAR_ARROW_LEFT) {
            ch = '<';
        } else if (ch == CHAR_ARROW_RGHT) {
            ch = '>';
        } else if ((uint8_t)ch >= (uint8_t)CHAR_ICON_FIRST && (uint8_t)ch <= (uint8_t)CHAR_ICON_LAST) {
            uint8_t idx = (uint8_t)((uint8_t)ch - (uint8_t)CHAR_ICON_FIRST);
            for (uint8_t j = 0; j < 2 && (*pos + 1) < max_len; j++) {
                dst[(*pos)++] = (char)pgm_read_byte(&oled_icon_ascii_tags[idx][j]);
            }
            continue;
        }
        dst[(*pos)++] = ch;
    }
}

static void format_rgb_enc_desc(uint16_t ccw, uint16_t cw, char out[12]) {
    const char *preset = NULL;
    if (ccw == RM_VALD && cw == RM_VALU) {
        preset = "BRI -/+";
    } else if (ccw == RM_VALU && cw == RM_VALD) {
        preset = "BRI +/-";
    } else if (ccw == RM_HUED && cw == RM_HUEU) {
        preset = "HUE -/+";
    } else if (ccw == RM_HUEU && cw == RM_HUED) {
        preset = "HUE +/-";
    } else if (ccw == RM_SATD && cw == RM_SATU) {
        preset = "SAT -/+";
    } else if (ccw == RM_SATU && cw == RM_SATD) {
        preset = "SAT +/-";
    } else if (ccw == RM_SPDD && cw == RM_SPDU) {
        preset = "SPD -/+";
    } else if (ccw == RM_SPDU && cw == RM_SPDD) {
        preset = "SPD +/-";
    } else if (ccw == RM_PREV && cw == RM_NEXT) {
        preset = "MOD -/+";
    } else if (ccw == RM_NEXT && cw == RM_PREV) {
        preset = "MOD +/-";
    } else if ((ccw == KC_VOLD || ccw == KC_KB_VOLUME_DOWN) && (cw == KC_VOLU || cw == KC_KB_VOLUME_UP)) {
        preset = "VOL -/+";
    } else if ((ccw == KC_VOLU || ccw == KC_KB_VOLUME_UP) && (cw == KC_VOLD || cw == KC_KB_VOLUME_DOWN)) {
        preset = "VOL +/-";
    } else if ((ccw == MS_WHLU && cw == MS_WHLD) || (ccw == MS_WHLD && cw == MS_WHLU)) {
        preset = "SCROLL";
    } else if ((ccw == KC_NO || ccw == KC_TRNS) && (cw == KC_NO || cw == KC_TRNS)) {
        preset = "NONE";
    }

    if (preset != NULL) {
        copy_short_str(out, preset, 12);
        return;
    }

    char lbl_ccw[4], lbl_cw[4];
    format_keycode_label(ccw, false, false, lbl_ccw);
    format_keycode_label(cw, false, false, lbl_cw);

    uint8_t pos = 0;
    append_sanitized_lbl(out, &pos, 12, lbl_ccw);
    if (pos + 1 < 12) {
        out[pos++] = '/';
    }
    append_sanitized_lbl(out, &pos, 12, lbl_cw);
    out[pos] = '\0';
}

static void render_oled_rgb_right(uint8_t *fb, const oled_app_ctx_t *ctx) {
    memset(fb, 0, OLED_MATRIX_SIZE);

    if (ctx->top_layer == _ADJUST) {
        fb_fill_rect(fb, 0, 0, 13, 9, true);
        fb_draw_text3x7(fb, 1, 1, "ADJ", true);
    } else {
        fb_draw_text3x7(fb, 1, 1, "ADJ", false);
    }
    fb_draw_text3x7(fb, 15, 1, "LIVE", false);

    char page[10];
    format_ratio_str((uint8_t)(ctx->app + 1), OLED_APP_COUNT, page);
    uint8_t pw = fb_measure_text3x7(page);
    fb_draw_text3x7(fb, (uint8_t)(OLED_PORTRAIT_W - pw), 1, page, false);

    fb_draw_hline(fb, 0, 10, OLED_PORTRAIT_W, false);

    for (uint8_t r = 0; r < 4; r++) {
        for (uint8_t disp_c = 0; disp_c < 6; disp_c++) {
            uint8_t  mat_c   = (uint8_t)(5 - disp_c);
            uint16_t kc      = ctx->keys[r][mat_c];
            char     lbl[4];
            format_keycode_label_ex(kc, false, false, ctx->host_audio_output, lbl);
            bool    pressed = (ctx->pressed_rows[r] & (1U << mat_c)) != 0;
            bool    empty   = (kc == KC_NO || kc == KC_TRNS);
            uint8_t cx      = oled_col_x[disp_c];
            uint8_t cy      = (uint8_t)(12 + r * 15 + (oled_stagger_r[disp_c] >> 1));
            fb_draw_cell(fb, cx, cy, lbl, pressed, false, empty);
        }
    }

    fb_draw_hline(fb, 0, 75, OLED_PORTRAIT_W, true);

    static const char *const legend[2][2] = {
        {"H:HUE", "S:SAT"},
        {"P:SPD", "R:MOD"},
    };
    for (uint8_t i = 0; i < 2; i++) {
        uint8_t ly = (uint8_t)(78 + i * 9);
        fb_draw_text3x7(fb, 0, ly, legend[i][0], false);
        fb_draw_text3x7(fb, 33, ly, legend[i][1], false);
    }

    uint16_t icon_bri[7], icon_pwr[7];
    uint8_t  bri_idx = (uint8_t)((uint8_t)CHAR_ICON_BRI_UP - (uint8_t)CHAR_ICON_FIRST);
    uint8_t  pwr_idx = (uint8_t)((uint8_t)CHAR_ICON_POWER - (uint8_t)CHAR_ICON_FIRST);
    for (uint8_t i = 0; i < 7; i++) {
        icon_bri[i] = pgm_read_word(&oled_icons7x9[bri_idx][i]);
        icon_pwr[i] = pgm_read_word(&oled_icons7x9[pwr_idx][i]);
    }
    fb_draw_cols9(fb, 0, 95, icon_bri, 7, false);
    fb_draw_text3x7(fb, 8, 96, ":BRI", false);
    fb_draw_cols9(fb, 33, 95, icon_pwr, 7, false);
    fb_draw_text3x7(fb, 41, 96, ":ON", false);

    fb_draw_hline(fb, 0, 105, OLED_PORTRAIT_W, true);

    char desc_l[12], desc_r[12];
    format_rgb_enc_desc(ctx->peer_enc_ccw, ctx->peer_enc_cw, desc_l);
    format_rgb_enc_desc(ctx->enc_ccw, ctx->enc_cw, desc_r);

    char enc_text[2][16];
    copy_short_str(enc_text[0], "KNB:", sizeof(enc_text[0]));
    copy_short_str(enc_text[0] + 4, desc_l, (uint8_t)(sizeof(enc_text[0]) - 4));
    copy_short_str(enc_text[1], "KNB:", sizeof(enc_text[1]));
    copy_short_str(enc_text[1] + 4, desc_r, (uint8_t)(sizeof(enc_text[1]) - 4));

    uint8_t w0    = (uint8_t)(7 + fb_measure_text3x7(enc_text[0]));
    uint8_t w1    = (uint8_t)(7 + fb_measure_text3x7(enc_text[1]));
    uint8_t max_w = (w0 > w1) ? w0 : w1;
    uint8_t ex    = (OLED_PORTRAIT_W > max_w) ? (uint8_t)((OLED_PORTRAIT_W - max_w) / 2) : 0;

    static const char side_ch[2][2] = {"L", "R"};
    for (uint8_t i = 0; i < 2; i++) {
        uint8_t py = (uint8_t)(107 + i * 11);
        fb_fill_rect(fb, ex, py, 5, 9, true);
        fb_draw_text3x7(fb, (uint8_t)(ex + 1), (uint8_t)(py + 1), side_ch[i], true);
        fb_draw_text3x7(fb, (uint8_t)(ex + 7), (uint8_t)(py + 1), enc_text[i], false);
    }
}

static void render_oled_app_keymap(uint8_t *fb, const oled_app_ctx_t *ctx) {
    render_oled_layout(fb, ctx->is_left, ctx->top_layer, ctx->conn_mode, ctx->mods, ctx->caps, ctx->host_audio_output, ctx->pressed_rows, ctx->keys, ctx->enc_ccw, ctx->enc_cw);
}

static void render_oled_app_rgb_info(uint8_t *fb, const oled_app_ctx_t *ctx) {
    if (ctx->is_left) {
        render_oled_rgb_left(fb, ctx);
    } else {
        render_oled_rgb_right(fb, ctx);
    }
}

static const oled_app_render_fn oled_apps[OLED_APP_COUNT] = {
    [OLED_APP_KEYMAP]   = render_oled_app_keymap,
    [OLED_APP_RGB_INFO] = render_oled_app_rgb_info,
};

typedef struct {
    bool           valid;
    oled_app_ctx_t ctx;
} oled_render_cache_t;

static oled_render_cache_t oled_cache                   = {0};
static uint8_t             oled_frame[OLED_MATRIX_SIZE] = {0};

bool oled_task_user(void) {
    if (lights_disabled) {
        oled_off();
        return false;
    }

    oled_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app     = (current_oled_app < OLED_APP_COUNT) ? current_oled_app : OLED_APP_KEYMAP;
    ctx.is_left = is_keyboard_left();

    uint8_t row_offset = ctx.is_left ? 0 : 5;
    for (uint8_t r = 0; r < 5; r++) {
        ctx.pressed_rows[r] = (uint8_t)(matrix_get_row((uint8_t)(r + row_offset)) & 0x3F);
    }

    if (!is_keyboard_master() && slave_has_hdr && slave_has_keys) {
        ctx.top_layer         = slave_hdr_state.top_layer;
        ctx.conn_mode         = OLED_CONN_TRRS;
        ctx.mods              = slave_hdr_state.mods;
        ctx.caps              = (slave_hdr_state.flags & SLAVE_HDR_CAPS_LOCK) != 0;
        ctx.host_audio_output = (uint8_t)((slave_hdr_state.flags & SLAVE_HDR_AUDIO_MASK) >> SLAVE_HDR_AUDIO_SHIFT);
        memcpy(ctx.keys, slave_keys, sizeof(ctx.keys));
        ctx.enc_ccw      = slave_enc_ccw;
        ctx.enc_cw       = slave_enc_cw;
        ctx.peer_enc_ccw = slave_peer_enc_ccw;
        ctx.peer_enc_cw  = slave_peer_enc_cw;
    } else {
        ctx.host_audio_output = host_audio_output;
        layer_state_t active_mask = layer_state | default_layer_state;
        ctx.top_layer             = get_highest_layer(active_mask);
        ctx.mods                  = get_local_mods();
        if (is_transport_connected() || !is_keyboard_master()) {
            ctx.conn_mode = OLED_CONN_TRRS;
        } else {
            ctx.mods |= remote_mods;
            if (host_service_connected && host_peer_connected) {
                ctx.conn_mode = OLED_CONN_SYNC;
            } else if (host_service_connected) {
                ctx.conn_mode = OLED_CONN_WAIT;
            } else {
                ctx.conn_mode = OLED_CONN_DISC;
            }
        }
        ctx.caps = host_keyboard_led_state().caps_lock;
        if (ctx.app == OLED_APP_RGB_INFO && !ctx.is_left) {
            populate_rgb_right_panel(ctx.keys, &ctx.enc_ccw, &ctx.enc_cw, &ctx.peer_enc_ccw, &ctx.peer_enc_cw);
        } else {
            populate_half_keys(ctx.is_left, active_mask, ctx.top_layer, ctx.keys, &ctx.enc_ccw, &ctx.enc_cw);
            ctx.peer_enc_ccw = KC_NO;
            ctx.peer_enc_cw  = KC_NO;
        }
    }

#ifdef RGB_MATRIX_ENABLE
    {
        hsv_t hsv      = rgb_matrix_get_hsv();
        ctx.rgb_enable = rgb_matrix_is_enabled() ? 1 : 0;
        ctx.rgb_mode   = rgb_matrix_get_mode();
        ctx.rgb_h      = hsv.h;
        ctx.rgb_s      = hsv.s;
        ctx.rgb_v      = hsv.v;
        ctx.rgb_speed  = rgb_matrix_get_speed();
    }
#endif

    if (!oled_cache.valid || memcmp(&oled_cache.ctx, &ctx, sizeof(ctx)) != 0) {
        oled_apps[ctx.app](oled_frame, &ctx);
        oled_cache.valid = true;
        oled_cache.ctx   = ctx;
    }

    oled_set_cursor(0, 0);
    oled_write_raw((const char *)oled_frame, OLED_MATRIX_SIZE);
    return false;
}

#endif
