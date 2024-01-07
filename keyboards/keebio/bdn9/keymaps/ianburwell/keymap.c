/* Copyright 2019 Danny Nguyen <danny@keeb.io>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include QMK_KEYBOARD_H
#include "virtser.h"

// Any key code >= SAFE_RANGE will be 
#define CDC_CHAR(character) (QK_USER + character%0xFF)

enum encoder_names {
  _LEFT,
  _RIGHT,
  _MIDDLE,
};

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    [0] = LAYOUT(
        KC_MPLY,       MO(1),         CDC_CHAR('0'),
        CDC_CHAR('1'), CDC_CHAR('2'), CDC_CHAR('3'),
        CDC_CHAR('4'), CDC_CHAR('5'), CDC_CHAR('6')
    ),

    [1] = LAYOUT(
        QK_BOOT,       _______,       RGB_MOD,
        CDC_CHAR('7'), CDC_CHAR('8'), CDC_CHAR('9'),
        CDC_CHAR('A'), CDC_CHAR('B'), CDC_CHAR('C')
    ),

};

bool encoder_update_user(uint8_t index, bool clockwise) {
    if (index == _LEFT) {
        if (clockwise) {
            if(layer_state_is(1))
                rgb_matrix_increase_hue();
            else
                tap_code(KC_VOLU);
        } else {
            if(layer_state_is(1))
                rgb_matrix_decrease_hue();
            else
                tap_code(KC_VOLD);
        }
    }
    // else if (index == _MIDDLE) {
    //     if (clockwise) {
    //         tap_code(KC_DOWN);
    //     } else {
    //         tap_code(KC_UP);
    //     }
    // }
    else if (index == _RIGHT) {
        if (clockwise) {

            if(layer_state_is(1))
                rgb_matrix_increase_val();
            else
                virtser_send('+');

        } else {

            if(layer_state_is(1))
                rgb_matrix_decrease_val();
            else
                virtser_send('-');

        }
    }
    return false;
}

bool process_record_user(uint16_t keycode, keyrecord_t *record){
    // Allow normal processing of non-CDC keys
    if (!IS_QK_USER(keycode))
        return true;

    // We will handle all CDC keys however
    if(record->event.pressed)
        virtser_send((const char)(keycode-CDC_CHAR(0)));
    return false;
}