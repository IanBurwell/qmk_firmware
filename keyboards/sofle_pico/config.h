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
#define SPLIT_CONNECTION_CHECK_TIMEOUT 3000
#define RPC_M2S_BUFFER_SIZE 72
#define VIA_EEPROM_CUSTOM_CONFIG_SIZE 4

/* Per-mode RGB Matrix preset storage (4B magic + 4B valid mask + 32 x 4B HSV/speed slots) */
#define EECONFIG_USER_DATA_SIZE 136
#define EECONFIG_USER_DATA_VERSION 0x52503031UL

/* Stabilize USB hub/host resume transitions and recover if a half boots during PC sleep */
#define USB_SUSPEND_WAKEUP_DELAY 200
#define SPLIT_WATCHDOG_ENABLE
#define SPLIT_WATCHDOG_TIMEOUT 3000
