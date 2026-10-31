// Copyright 2024 Ryan Neff (@JellyTitan)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* Use full duplex communication (TRRS) */
#define SERIAL_USART_FULL_DUPLEX
#define SERIAL_USART_TX_PIN GP16
#define SERIAL_USART_RX_PIN GP17
/* Swap TX and RX pins if keyboard is master half. Only available on some MCU's. This _is_ available on the RP2040 */
#define SERIAL_USART_PIN_SWAP

/* Set handedness via EEPROM so either side can be plugged into USB */
#define EE_HANDS
// Uncomment one of the following to set the default handedness for the keyboard
// #define INIT_EE_HANDS_RIGHT
// #define INIT_EE_HANDS_LEFT

/* I2C for OLEDs */
#define I2C_DRIVER I2CD1
#define I2C1_SDA_PIN GP6
#define I2C1_SCL_PIN GP7

#define OLED_DISPLAY_128X64
#define OLED_TIMEOUT 30000

#define ENCODER_MAP_KEY_DELAY 10

#define RGB_MATRIX_TYPING_HEATMAP_DECREASE_DELAY_MS 50

/* Custom split transaction ID to sync GP24 user button and OLED/RGB toggle state */
#define SPLIT_TRANSACTION_IDS_USER USR_BTN_SYNC
