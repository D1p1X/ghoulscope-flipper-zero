#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include <lib/subghz/devices/cc1101_configs.h>

#define GHOUL_TAG "GhoulScope"
#define GHOUL_CALIBRATION_SAMPLES 20U
#define GHOUL_ALERT_MIN_INTERVAL_MS 120U
#define GHOUL_ALERT_MAX_INTERVAL_MS 800U
#define GHOUL_TOAST_MS 1300U
#define GHOUL_SETTINGS_PATH APP_DATA_PATH("settings.txt")

typedef enum {
    GhoulSettingLed,
    GhoulSettingBrightness,
    GhoulSettingBeep,
    GhoulSettingVibration,
    GhoulSettingTestAlert,
    GhoulSettingCount,
} GhoulSetting;

typedef enum {
    GhoulScreenMain,
    GhoulScreenMenu,
    GhoulScreenSettings,
    GhoulScreenHelp,
} GhoulScreen;

typedef enum {
    GhoulMenuReturn,
    GhoulMenuSettings,
    GhoulMenuHelp,
    GhoulMenuExit,
    GhoulMenuCount,
} GhoulMenu;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    NotificationApp* notification;
    Storage* storage;
    File* record_file;

    bool scanning;
    bool recording;
    bool calibrating;
    bool led_enabled;
    bool beep_enabled;
    bool vibration_enabled;
    bool last_anomaly;

    uint8_t frequency_index;
    uint8_t sensitivity_index;
    uint8_t rate_index;
    uint8_t led_brightness_index;
    uint8_t settings_index;
    uint8_t menu_index;
    uint8_t help_page;
    GhoulScreen screen;
    uint16_t calibration_count;
    uint32_t sample_period_ms;
    uint32_t last_sample_tick;
    uint32_t last_alert_tick;
    uint32_t anomaly_until_tick;
    uint32_t toast_until_tick;
    uint32_t event_count;
    uint32_t record_count;
    float calibration_sum;
    float baseline_rssi;
    float current_rssi;
    float peak_rssi;
    float delta_rssi;
    char toast[24];
    char recording_path[96];
} GhoulScopeApp;

static const uint32_t ghoul_frequencies[] = {
    433420000,
    433920000,
    434420000,
    868350000,
};

static const char* const ghoul_frequency_labels[] = {
    "433.42",
    "433.92",
    "434.42",
    "868.35",
};

/* Smaller change threshold means greater sensitivity. */
static const float ghoul_sensitivity_db[] = {2.0f, 3.5f, 5.0f, 8.0f, 12.0f};
static const uint16_t ghoul_rates_ms[] = {100U, 250U, 500U, 1000U};
static const uint8_t ghoul_led_brightness[] = {32U, 72U, 144U, 255U};
static const char* const ghoul_led_brightness_labels[] = {"12%", "28%", "56%", "100%"};

/* Bigger activity means a higher-pitched, longer alert and a shorter pause before next beep. */
static float ghoul_alert_intensity(float activity) {
    float intensity = (activity - 1.0f) / 2.0f;
    if(intensity < 0.0f) intensity = 0.0f;
    if(intensity > 1.0f) intensity = 1.0f;
    return intensity;
}

static uint32_t ghoul_alert_interval_ms(float activity) {
    const float intensity = ghoul_alert_intensity(activity);
    return (uint32_t)(GHOUL_ALERT_MAX_INTERVAL_MS -
                      (GHOUL_ALERT_MAX_INTERVAL_MS - GHOUL_ALERT_MIN_INTERVAL_MS) * intensity);
}

static void ghoul_activity_alert(GhoulScopeApp* app, float activity) {
    if(!app->beep_enabled && !app->vibration_enabled) return;

    const float intensity = ghoul_alert_intensity(activity);
    const NotificationMessage force_vibro = {
        .type = NotificationMessageTypeForceVibroSetting,
        .data.forced_settings.vibro = app->vibration_enabled,
    };
    const NotificationMessage vibro_on = {
        .type = NotificationMessageTypeVibro,
        .data.vibro.on = app->vibration_enabled,
    };
    const NotificationMessage sound_on = {
        .type = NotificationMessageTypeSoundOn,
        .data.sound.frequency = 850.0f + 1350.0f * intensity,
        .data.sound.volume = app->beep_enabled ? (0.35f + 0.65f * intensity) : 0.0f,
    };
    /* The Settings test uses activity > 1.0, which is unreachable for live
       samples. Keep live pulses responsive, but make the manual hardware
       check long enough to be unmistakable. */
    const NotificationMessage delay = {
        .type = NotificationMessageTypeDelay,
        .data.delay.length = activity > 1.0f ? 280U : 35U + (uint32_t)(55.0f * intensity),
    };
    const NotificationMessage vibro_off = {
        .type = NotificationMessageTypeVibro,
        .data.vibro.on = false,
    };
    const NotificationSequence alert_sequence = {
        &message_force_speaker_volume_setting_1f,
        &force_vibro,
        &message_display_backlight_on,
        &vibro_on,
        &sound_on,
        &delay,
        &vibro_off,
        &message_sound_off,
        NULL,
    };
    notification_message_block(app->notification, &alert_sequence);
}

static void ghoul_led_off(GhoulScopeApp* app) {
    notification_message_block(app->notification, &sequence_reset_rgb);
}

/*
 * Stable LED levels: green -> yellow -> red -> magenta -> white.
 * Red means the configured limit was reached; magenta and white make a
 * much stronger change immediately distinguishable without any blinking.
 */
static void ghoul_led_update(GhoulScopeApp* app) {
    if(!app->led_enabled || !app->scanning) {
        ghoul_led_off(app);
        return;
    }

    float activity = 0.0f;
    if(!app->calibrating) {
        const float threshold = ghoul_sensitivity_db[app->sensitivity_index];
        if(threshold > 0.0f) activity = app->delta_rssi / threshold;
    }
    if(activity < 0.0f) activity = 0.0f;
    if(activity > 2.5f) activity = 2.5f;

    const uint8_t level = ghoul_led_brightness[app->led_brightness_index];
    uint8_t red = 0U;
    uint8_t green = 0U;
    uint8_t blue = 0U;
    if(activity < 0.5f) {
        red = (uint8_t)(level * activity * 2.0f);
        green = level;
    } else if(activity < 1.0f) {
        red = level;
        green = (uint8_t)(level * (1.0f - activity) * 2.0f);
    } else if(activity < 1.75f) {
        red = level;
        blue = (uint8_t)(level * (activity - 1.0f) / 0.75f);
    } else if(activity < 2.5f) {
        red = level;
        blue = level;
        green = (uint8_t)(level * (activity - 1.75f) / 0.75f);
    } else {
        red = level;
        green = level;
        blue = level;
    }

    const NotificationMessage red_message = {
        .type = NotificationMessageTypeLedRed,
        .data.led.value = red,
    };
    const NotificationMessage green_message = {
        .type = NotificationMessageTypeLedGreen,
        .data.led.value = green,
    };
    const NotificationMessage blue_message = {
        .type = NotificationMessageTypeLedBlue,
        .data.led.value = blue,
    };
    const NotificationSequence led_sequence = {
        &red_message,
        &green_message,
        &blue_message,
        &message_do_not_reset,
        NULL,
    };
    notification_message_block(app->notification, &led_sequence);
}

static void ghoul_settings_save(GhoulScopeApp* app) {
    File* file = storage_file_alloc(app->storage);
    char contents[20];
    const int length = snprintf(
        contents,
        sizeof(contents),
        "%u %u %u %u\n",
        app->led_enabled ? 1U : 0U,
        app->led_brightness_index,
        app->beep_enabled ? 1U : 0U,
        app->vibration_enabled ? 1U : 0U);

    if(length > 0 && (size_t)length < sizeof(contents) &&
       storage_file_open(file, GHOUL_SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, contents, (size_t)length);
        storage_file_close(file);
    }
    storage_file_free(file);
}

static void ghoul_settings_load(GhoulScopeApp* app) {
    File* file = storage_file_alloc(app->storage);
    char contents[20] = {0};
    unsigned int led_enabled = 1U;
    unsigned int brightness_index = app->led_brightness_index;
    unsigned int beep_enabled = 1U;
    unsigned int vibration_enabled = 1U;

    if(storage_file_open(file, GHOUL_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const size_t read = storage_file_read(file, contents, sizeof(contents) - 1U);
        storage_file_close(file);
        const int parsed = sscanf(
            contents,
            "%u %u %u %u",
            &led_enabled,
            &brightness_index,
            &beep_enabled,
            &vibration_enabled);
        if(read > 0U && parsed >= 3) {
            app->led_enabled = led_enabled != 0U;
            if(brightness_index < COUNT_OF(ghoul_led_brightness)) {
                app->led_brightness_index = (uint8_t)brightness_index;
            }
            app->beep_enabled = beep_enabled != 0U;
            /* Older releases stored one combined alerts switch as the third value. */
            app->vibration_enabled = parsed >= 4 ? vibration_enabled != 0U : app->beep_enabled;
        }
    }
    storage_file_free(file);
}

static void ghoul_toast(GhoulScopeApp* app, const char* message) {
    strlcpy(app->toast, message, sizeof(app->toast));
    app->toast_until_tick = furi_get_tick() + furi_ms_to_ticks(GHOUL_TOAST_MS);
}

static void ghoul_reset_baseline(GhoulScopeApp* app) {
    app->calibrating = true;
    app->calibration_count = 0;
    app->calibration_sum = 0.0f;
    app->baseline_rssi = -127.0f;
    app->delta_rssi = 0.0f;
    app->last_anomaly = false;
}

static void ghoul_radio_stop(void) {
    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
    furi_hal_subghz_sleep();
}

static bool ghoul_radio_start(GhoulScopeApp* app) {
    const uint32_t requested_frequency = ghoul_frequencies[app->frequency_index];

    if(!furi_hal_subghz_is_frequency_valid(requested_frequency)) {
        ghoul_toast(app, "Frequency blocked");
        return false;
    }

    furi_hal_subghz_reset();
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);
    furi_hal_subghz_set_frequency_and_path(requested_frequency);
    furi_hal_subghz_rx();
    app->scanning = true;
    app->last_sample_tick = furi_get_tick();
    ghoul_reset_baseline(app);
    return true;
}

static void ghoul_radio_pause(GhoulScopeApp* app) {
    ghoul_radio_stop();
    app->scanning = false;
    app->last_anomaly = false;
    ghoul_led_off(app);
}

static void ghoul_record_close(GhoulScopeApp* app) {
    if(app->record_file) {
        storage_file_close(app->record_file);
        storage_file_free(app->record_file);
        app->record_file = NULL;
    }
    app->recording = false;
}

static bool ghoul_record_open(GhoulScopeApp* app) {
    DateTime now;
    furi_hal_rtc_get_datetime(&now);

    char leaf_name[48];
    for(uint8_t suffix = 0; suffix < 100U; suffix++) {
        if(suffix == 0U) {
            snprintf(
                leaf_name,
                sizeof(leaf_name),
                "scan_%04u%02u%02u_%02u%02u%02u.csv",
                now.year,
                now.month,
                now.day,
                now.hour,
                now.minute,
                now.second);
        } else {
            snprintf(
                leaf_name,
                sizeof(leaf_name),
                "scan_%04u%02u%02u_%02u%02u%02u_%u.csv",
                now.year,
                now.month,
                now.day,
                now.hour,
                now.minute,
                now.second,
                suffix);
        }

        snprintf(app->recording_path, sizeof(app->recording_path), "%s%s", APP_DATA_PATH(""), leaf_name);
        if(!storage_file_exists(app->storage, app->recording_path)) break;
    }

    app->record_file = storage_file_alloc(app->storage);
    if(!storage_file_open(app->record_file, app->recording_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->record_file);
        app->record_file = NULL;
        ghoul_toast(app, "SD write failed");
        return false;
    }

    const char* header =
        "timestamp,uptime_ms,frequency_hz,rssi_dbm,baseline_dbm,delta_db,anomaly\n";
    if(storage_file_write(app->record_file, header, strlen(header)) != strlen(header)) {
        ghoul_record_close(app);
        ghoul_toast(app, "SD write failed");
        return false;
    }

    app->recording = true;
    app->record_count = 0;
    ghoul_toast(app, "Recording on");
    return true;
}

static void ghoul_toggle_recording(GhoulScopeApp* app) {
    if(app->recording) {
        ghoul_record_close(app);
        ghoul_toast(app, "Recording saved");
        return;
    }

    if(!app->scanning && !ghoul_radio_start(app)) return;
    ghoul_record_open(app);
}

static void ghoul_write_sample(GhoulScopeApp* app, bool anomaly) {
    if(!app->recording || !app->record_file) return;

    DateTime now;
    char line[128];
    furi_hal_rtc_get_datetime(&now);
    const int len = snprintf(
        line,
        sizeof(line),
        "%04u-%02u-%02uT%02u:%02u:%02u,%lu,%lu,%.1f,%.1f,%.1f,%u\n",
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second,
        (unsigned long)furi_get_tick(),
        (unsigned long)ghoul_frequencies[app->frequency_index],
        (double)app->current_rssi,
        (double)app->baseline_rssi,
        (double)app->delta_rssi,
        anomaly ? 1U : 0U);

    if((len < 0) || ((size_t)len >= sizeof(line)) ||
       (storage_file_write(app->record_file, line, (size_t)len) != (size_t)len)) {
        ghoul_record_close(app);
        ghoul_toast(app, "Recording stopped");
        return;
    }

    app->record_count++;
}

static void ghoul_sample(GhoulScopeApp* app) {
    const uint32_t now = furi_get_tick();
    app->current_rssi = furi_hal_subghz_get_rssi();
    if(app->current_rssi > app->peak_rssi) app->peak_rssi = app->current_rssi;
    app->last_anomaly = false;

    if(app->calibrating) {
        app->calibration_sum += app->current_rssi;
        app->calibration_count++;
        app->baseline_rssi = app->calibration_sum / app->calibration_count;
        app->delta_rssi = 0.0f;
        if(app->calibration_count >= GHOUL_CALIBRATION_SAMPLES) {
            app->calibrating = false;
            ghoul_toast(app, "Baseline ready");
        }
        ghoul_write_sample(app, false);
        ghoul_led_update(app);
        return;
    }

    app->delta_rssi = app->current_rssi - app->baseline_rssi;
    const float limit = ghoul_sensitivity_db[app->sensitivity_index];
    const float activity = app->delta_rssi / limit;
    const bool anomaly = app->delta_rssi >= limit;
    app->last_anomaly = anomaly;

    if(anomaly) {
        app->anomaly_until_tick = now + furi_ms_to_ticks(GHOUL_ALERT_MAX_INTERVAL_MS);
        if((now - app->last_alert_tick) >= furi_ms_to_ticks(ghoul_alert_interval_ms(activity))) {
            app->event_count++;
            app->last_alert_tick = now;
            ghoul_activity_alert(app, activity);
        }
    } else {
        /* Slowly follow a stable background so long recordings do not drift indefinitely. */
        app->baseline_rssi = (app->baseline_rssi * 0.99f) + (app->current_rssi * 0.01f);
    }

    ghoul_write_sample(app, anomaly);
    ghoul_led_update(app);
}

static void ghoul_draw_meter(Canvas* canvas, GhoulScopeApp* app) {
    const float min_rssi = -110.0f;
    const float max_rssi = -25.0f;
    float normalized = (app->current_rssi - min_rssi) / (max_rssi - min_rssi);
    if(normalized < 0.0f) normalized = 0.0f;
    if(normalized > 1.0f) normalized = 1.0f;
    const uint8_t width = (uint8_t)(normalized * 122.0f);

    canvas_draw_frame(canvas, 2, 43, 124, 10);
    if(width > 0U) canvas_draw_box(canvas, 3, 44, width, 8);

    /* Small top tick: strongest signal seen since this app was launched. */
    float peak = (app->peak_rssi - min_rssi) / (max_rssi - min_rssi);
    if(peak < 0.0f) peak = 0.0f;
    if(peak > 1.0f) peak = 1.0f;
    const uint8_t peak_x = 3U + (uint8_t)(peak * 122.0f);
    canvas_draw_line(canvas, peak_x, 40, peak_x, 42);

    if(!app->calibrating) {
        float threshold_rssi = app->baseline_rssi + ghoul_sensitivity_db[app->sensitivity_index];
        float threshold = (threshold_rssi - min_rssi) / (max_rssi - min_rssi);
        if(threshold < 0.0f) threshold = 0.0f;
        if(threshold > 1.0f) threshold = 1.0f;
        const uint8_t x = 3U + (uint8_t)(threshold * 122.0f);
        canvas_draw_line(canvas, x, 42, x, 53);
    }
}

static void ghoul_draw_callback(Canvas* canvas, void* context) {
    GhoulScopeApp* app = context;
    char line[40];
    const uint32_t now = furi_get_tick();

    canvas_clear(canvas);
    if(app->screen == GhoulScreenMenu) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "GHOULSCOPE MENU");
        canvas_draw_line(canvas, 0, 13, 128, 13);
        canvas_set_font(canvas, FontSecondary);

        snprintf(line, sizeof(line), "%c Resume scan", app->menu_index == GhoulMenuReturn ? '>' : ' ');
        canvas_draw_str(canvas, 2, 24, line);
        snprintf(
            line,
            sizeof(line),
            "%c Settings",
            app->menu_index == GhoulMenuSettings ? '>' : ' ');
        canvas_draw_str(canvas, 2, 34, line);
        snprintf(line, sizeof(line), "%c Quick help", app->menu_index == GhoulMenuHelp ? '>' : ' ');
        canvas_draw_str(canvas, 2, 44, line);
        snprintf(line, sizeof(line), "%c Exit app", app->menu_index == GhoulMenuExit ? '>' : ' ');
        canvas_draw_str(canvas, 2, 54, line);
        canvas_draw_str(canvas, 2, 63, "UP/DN move   OK select");
        return;
    }

    if(app->screen == GhoulScreenSettings) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "SETTINGS");
        canvas_draw_str(canvas, 98, 10, "BACK");
        canvas_draw_line(canvas, 0, 13, 128, 13);
        canvas_set_font(canvas, FontSecondary);

        snprintf(
            line,
            sizeof(line),
            "%c LED meter: %s",
            app->settings_index == GhoulSettingLed ? '>' : ' ',
            app->led_enabled ? "ON" : "OFF");
        canvas_draw_str(canvas, 2, 23, line);
        snprintf(
            line,
            sizeof(line),
            "%c Brightness: %s",
            app->settings_index == GhoulSettingBrightness ? '>' : ' ',
            ghoul_led_brightness_labels[app->led_brightness_index]);
        canvas_draw_str(canvas, 2, 32, line);
        snprintf(
            line,
            sizeof(line),
            "%c Beep: %s",
            app->settings_index == GhoulSettingBeep ? '>' : ' ',
            app->beep_enabled ? "DYNAMIC" : "OFF");
        canvas_draw_str(canvas, 2, 41, line);
        snprintf(
            line,
            sizeof(line),
            "%c Vibrate: %s",
            app->settings_index == GhoulSettingVibration ? '>' : ' ',
            app->vibration_enabled ? "ON" : "OFF");
        canvas_draw_str(canvas, 2, 50, line);
        snprintf(line, sizeof(line), "%c Test alert", app->settings_index == GhoulSettingTestAlert ? '>' : ' ');
        canvas_draw_str(canvas, 2, 59, line);
        return;
    }

    if(app->screen == GhoulScreenHelp) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(
            canvas,
            2,
            10,
            app->help_page == 0U ? "QUICK HELP 1/3" :
                                  (app->help_page == 1U ? "QUICK HELP 2/3" : "QUICK HELP 3/3"));
        canvas_draw_line(canvas, 0, 13, 128, 13);
        canvas_set_font(canvas, FontSecondary);
        if(app->help_page == 0U) {
            canvas_draw_str(canvas, 2, 26, "OK pause | hold OK record");
            canvas_draw_str(canvas, 2, 38, "LEFT/RIGHT: frequency");
            canvas_draw_str(canvas, 2, 50, "UP/DOWN: sensitivity");
            canvas_draw_str(canvas, 2, 63, "RIGHT next   BACK menu");
        } else if(app->help_page == 1U) {
            canvas_draw_str(canvas, 2, 26, "Hold LEFT/RIGHT: rate");
            canvas_draw_str(canvas, 2, 38, "Hold DOWN: clear events");
            canvas_draw_str(canvas, 2, 50, "Hold UP: recalibrate");
            canvas_draw_str(canvas, 2, 63, "RIGHT colors BACK menu");
        } else {
            canvas_draw_str(canvas, 2, 26, "G low / Y medium");
            canvas_draw_str(canvas, 2, 38, "R=limit / M=strong");
            canvas_draw_str(canvas, 2, 50, "W=extreme activity");
            canvas_draw_str(canvas, 2, 63, "Top tick: max signal");
        }
        return;
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "GHOULSCOPE");
    if(app->recording) canvas_draw_str(canvas, 102, 10, "REC");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);
    snprintf(
        line,
        sizeof(line),
        "%s MHz  %s  %lums",
        ghoul_frequency_labels[app->frequency_index],
        app->scanning ? "LIVE" : "PAUSE",
        (unsigned long)app->sample_period_ms);
    canvas_draw_str(canvas, 2, 23, line);

    snprintf(line, sizeof(line), "Signal %6.1f dBm", (double)app->current_rssi);
    canvas_draw_str(canvas, 2, 33, line);
    if(app->calibrating) {
        snprintf(line, sizeof(line), "Learning room %u/%u", app->calibration_count, GHOUL_CALIBRATION_SAMPLES);
    } else {
        snprintf(
            line,
            sizeof(line),
            "Change %+4.1f  limit %.1f",
            (double)app->delta_rssi,
            (double)ghoul_sensitivity_db[app->sensitivity_index]);
    }
    canvas_draw_str(canvas, 2, 41, line);
    ghoul_draw_meter(canvas, app);

    if(!app->calibrating && ((int32_t)(app->anomaly_until_tick - now) > 0)) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 63, "HIGH ACTIVITY");
        canvas_set_font(canvas, FontSecondary);
        snprintf(line, sizeof(line), "E:%lu", (unsigned long)app->event_count);
        canvas_draw_str(canvas, 94, 62, line);
    } else if((int32_t)(app->toast_until_tick - now) > 0) {
        canvas_draw_str(canvas, 2, 63, app->toast);
    } else {
        snprintf(line, sizeof(line), "BACK menu   Events: %lu", (unsigned long)app->event_count);
        canvas_draw_str(canvas, 2, 63, line);
    }
}

static void ghoul_input_callback(InputEvent* event, void* context) {
    GhoulScopeApp* app = context;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

static void ghoul_change_frequency(GhoulScopeApp* app, int8_t direction) {
    const uint8_t old_index = app->frequency_index;
    const uint8_t count = COUNT_OF(ghoul_frequencies);
    app->frequency_index = (uint8_t)((app->frequency_index + count + direction) % count);

    if(app->scanning) {
        ghoul_radio_stop();
        if(!ghoul_radio_start(app)) {
            app->frequency_index = old_index;
            ghoul_radio_start(app);
            return;
        }
    }

    char toast[24];
    snprintf(toast, sizeof(toast), "%s MHz", ghoul_frequency_labels[app->frequency_index]);
    ghoul_toast(app, toast);
}

static void ghoul_change_sensitivity(GhoulScopeApp* app, int8_t direction) {
    const int16_t next = (int16_t)app->sensitivity_index + direction;
    if(next < 0 || next >= (int16_t)COUNT_OF(ghoul_sensitivity_db)) return;
    app->sensitivity_index = (uint8_t)next;
    char toast[24];
    snprintf(toast, sizeof(toast), "Sensitivity %.1fdB", (double)ghoul_sensitivity_db[next]);
    ghoul_toast(app, toast);
}

static void ghoul_change_rate(GhoulScopeApp* app, int8_t direction) {
    const int16_t next = (int16_t)app->rate_index + direction;
    if(next < 0 || next >= (int16_t)COUNT_OF(ghoul_rates_ms)) return;
    app->rate_index = (uint8_t)next;
    app->sample_period_ms = ghoul_rates_ms[next];
    char toast[24];
    snprintf(toast, sizeof(toast), "Rate %lums", (unsigned long)app->sample_period_ms);
    ghoul_toast(app, toast);
}

static void ghoul_settings_change(GhoulScopeApp* app, int8_t direction) {
    switch((GhoulSetting)app->settings_index) {
    case GhoulSettingLed:
        app->led_enabled = !app->led_enabled;
        ghoul_toast(app, app->led_enabled ? "LED meter on" : "LED meter off");
        break;
    case GhoulSettingBrightness: {
        const int16_t next = (int16_t)app->led_brightness_index + direction;
        if(next < 0 || next >= (int16_t)COUNT_OF(ghoul_led_brightness)) return;
        app->led_brightness_index = (uint8_t)next;
        break;
    }
    case GhoulSettingBeep:
        app->beep_enabled = !app->beep_enabled;
        ghoul_toast(app, app->beep_enabled ? "Dynamic beep on" : "Beep off");
        break;
    case GhoulSettingVibration:
        app->vibration_enabled = !app->vibration_enabled;
        ghoul_toast(app, app->vibration_enabled ? "Vibration on" : "Vibration off");
        break;
    case GhoulSettingTestAlert:
        ghoul_activity_alert(app, 2.5f);
        ghoul_toast(app, "Test alert");
        break;
    default:
        return;
    }

    ghoul_settings_save(app);
    ghoul_led_update(app);
}

static bool ghoul_handle_settings_input(GhoulScopeApp* app, const InputEvent* event) {
    if(event->type == InputTypeShort) {
        switch(event->key) {
        case InputKeyBack:
            app->screen = GhoulScreenMenu;
            ghoul_toast(app, "Settings saved");
            return true;
        case InputKeyUp:
            if(app->settings_index > 0U) app->settings_index--;
            break;
        case InputKeyDown:
            if(app->settings_index + 1U < GhoulSettingCount) app->settings_index++;
            break;
        case InputKeyLeft:
            ghoul_settings_change(app, -1);
            break;
        case InputKeyRight:
        case InputKeyOk:
            ghoul_settings_change(app, 1);
            break;
        default:
            break;
        }
    }
    return true;
}

static bool ghoul_handle_menu_input(GhoulScopeApp* app, const InputEvent* event) {
    if(event->type != InputTypeShort) return true;

    switch(event->key) {
    case InputKeyBack:
        app->screen = GhoulScreenMain;
        break;
    case InputKeyUp:
        if(app->menu_index > 0U) app->menu_index--;
        break;
    case InputKeyDown:
        if(app->menu_index + 1U < GhoulMenuCount) app->menu_index++;
        break;
    case InputKeyOk:
    case InputKeyRight:
        switch((GhoulMenu)app->menu_index) {
        case GhoulMenuReturn:
            app->screen = GhoulScreenMain;
            break;
        case GhoulMenuSettings:
            app->screen = GhoulScreenSettings;
            app->settings_index = GhoulSettingLed;
            break;
        case GhoulMenuHelp:
            app->screen = GhoulScreenHelp;
            app->help_page = 0U;
            break;
        case GhoulMenuExit:
            return false;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return true;
}

static bool ghoul_handle_help_input(GhoulScopeApp* app, const InputEvent* event) {
    if(event->type != InputTypeShort) return true;

    switch(event->key) {
    case InputKeyBack:
        app->screen = GhoulScreenMenu;
        break;
    case InputKeyLeft:
        if(app->help_page > 0U) app->help_page--;
        break;
    case InputKeyRight:
        if(app->help_page < 2U) app->help_page++;
        break;
    default:
        break;
    }
    return true;
}

static bool ghoul_handle_input(GhoulScopeApp* app, const InputEvent* event) {
    if(app->screen == GhoulScreenSettings) return ghoul_handle_settings_input(app, event);
    if(app->screen == GhoulScreenMenu) return ghoul_handle_menu_input(app, event);
    if(app->screen == GhoulScreenHelp) return ghoul_handle_help_input(app, event);

    if(event->type == InputTypeShort) {
        switch(event->key) {
        case InputKeyBack:
            app->screen = GhoulScreenMenu;
            app->menu_index = GhoulMenuReturn;
            break;
        case InputKeyOk:
            if(app->scanning) {
                ghoul_radio_pause(app);
                ghoul_toast(app, "Scan paused");
            } else if(ghoul_radio_start(app)) {
                ghoul_toast(app, "Scan started");
            }
            break;
        case InputKeyLeft:
            ghoul_change_frequency(app, -1);
            break;
        case InputKeyRight:
            ghoul_change_frequency(app, 1);
            break;
        case InputKeyUp:
            ghoul_change_sensitivity(app, -1);
            break;
        case InputKeyDown:
            ghoul_change_sensitivity(app, 1);
            break;
        default:
            break;
        }
    } else if(event->type == InputTypeLong) {
        switch(event->key) {
        case InputKeyOk:
            ghoul_toggle_recording(app);
            break;
        case InputKeyUp:
            ghoul_reset_baseline(app);
            ghoul_toast(app, "Recalibrating");
            break;
        case InputKeyDown:
            app->event_count = 0;
            ghoul_toast(app, "Events cleared");
            break;
        case InputKeyLeft:
            ghoul_change_rate(app, -1);
            break;
        case InputKeyRight:
            ghoul_change_rate(app, 1);
            break;
        case InputKeyBack:
            return false;
        default:
            break;
        }
    }
    return true;
}

int32_t ghoul_scope_app(void* p) {
    UNUSED(p);
    GhoulScopeApp app = {
        .scanning = false,
        .recording = false,
        .calibrating = false,
        .led_enabled = true,
        .beep_enabled = true,
        .vibration_enabled = true,
        .frequency_index = 1U,
        .sensitivity_index = 2U,
        .rate_index = 1U,
        .led_brightness_index = 2U,
        .screen = GhoulScreenMain,
        .sample_period_ms = ghoul_rates_ms[1],
        .baseline_rssi = -127.0f,
        .current_rssi = -127.0f,
        .peak_rssi = -127.0f,
        .delta_rssi = 0.0f,
    };

    app.input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app.view_port = view_port_alloc();
    view_port_draw_callback_set(app.view_port, ghoul_draw_callback, &app);
    view_port_input_callback_set(app.view_port, ghoul_input_callback, &app);
    app.gui = furi_record_open(RECORD_GUI);
    app.notification = furi_record_open(RECORD_NOTIFICATION);
    app.storage = furi_record_open(RECORD_STORAGE);
    ghoul_settings_load(&app);
    gui_add_view_port(app.gui, app.view_port, GuiLayerFullscreen);

    ghoul_radio_start(&app);
    ghoul_led_update(&app);
    ghoul_toast(&app, "Press BACK for menu");
    view_port_update(app.view_port);

    bool running = true;
    while(running) {
        InputEvent event;
        const FuriStatus status =
            furi_message_queue_get(app.input_queue, &event, furi_ms_to_ticks(25U));
        if(status == FuriStatusOk) running = ghoul_handle_input(&app, &event);

        const uint32_t now = furi_get_tick();
        if(app.scanning && (now - app.last_sample_tick >= furi_ms_to_ticks(app.sample_period_ms))) {
            app.last_sample_tick = now;
            ghoul_sample(&app);
        }

        view_port_update(app.view_port);
    }

    ghoul_record_close(&app);
    ghoul_radio_stop();
    notification_message(app.notification, &sequence_reset_vibro);
    notification_message(app.notification, &sequence_reset_sound);
    notification_message(app.notification, &sequence_reset_rgb);

    gui_remove_view_port(app.gui, app.view_port);
    view_port_free(app.view_port);
    furi_message_queue_free(app.input_queue);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    return 0;
}
