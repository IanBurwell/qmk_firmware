#ifdef ENABLE_RGB_MATRIX_GRADIENT_UP_DOWN_VAL
RGB_MATRIX_EFFECT(GRADIENT_UP_DOWN_VAL)
#    ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS

bool GRADIENT_UP_DOWN_VAL(effect_params_t* params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);

    hsv_t hsv = rgb_matrix_config.hsv;
    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();
        // g_led_config.point[i].y ranges from 0 (top) to 64 (bottom).
        // Bottom row (y = 64) is brightest; top row (y = 0..7) is darkest.
        // rgb_matrix_config.speed controls the gradient strength/steepness.
        uint8_t  y                = g_led_config.point[i].y > 64 ? 64 : g_led_config.point[i].y;
        uint8_t  dist_from_bottom = 64 - y;
        uint16_t drop             = ((uint16_t)dist_from_bottom * rgb_matrix_config.speed) / 30;
        uint8_t  v_scale          = (drop >= 255) ? 0 : (uint8_t)(255 - drop);
        hsv.v                     = scale8(rgb_matrix_config.hsv.v, v_scale);
        rgb_t rgb                 = rgb_matrix_hsv_to_rgb(hsv);
        rgb_matrix_set_color(i, rgb.r, rgb.g, rgb.b);
    }
    return rgb_matrix_check_finished_leds(led_max);
}

#    endif // RGB_MATRIX_CUSTOM_EFFECT_IMPLS
#endif     // ENABLE_RGB_MATRIX_GRADIENT_UP_DOWN_VAL
