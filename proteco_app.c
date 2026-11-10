// Proteco MOVER gate remote: read, decode RAW .sub files, add manually, save and send.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>
#include <gui/modules/validators.h>
#include <gui/modules/widget.h>
#include <dialogs/dialogs.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/subghz_worker.h>
#include <toolbox/path.h>

#include "proteco_codec.h"

#define TAG "Proteco"

#define PROTECO_FREQUENCY   433920000u
// SUBGHZ_DEVICE_CC1101_EXT_NAME, spelled out: its header is not part of every SDK
#define PROTECO_RADIO_EXT   "cc1101_ext"
#define PROTECO_PROBE_MS    5000u
#define PROTECO_PRESET      "FuriHalSubGhzPresetOok650Async"
#define PROTECO_FOLDER      EXT_PATH("subghz")
#define PROTECO_EXT         ".sub"
#define SUBGHZ_KEY_FILETYPE "Flipper SubGhz Key File"
#define SUBGHZ_RAW_FILETYPE "Flipper SubGhz RAW File"
#define RAW_END_GUARD_US    100000u
#define BIN_RAW_MAX_BITS    (512u * 8u) // BIN_RAW_BUF_DATA_SIZE in lib/subghz/protocols/bin_raw.c

typedef enum {
    ProtecoSceneStart,
    ProtecoSceneRead,
    ProtecoSceneOpen,
    ProtecoSceneManual,
    ProtecoSceneKey,
    ProtecoSceneSave,
    ProtecoSceneMessage,
    ProtecoSceneAbout,
    ProtecoSceneNum,
} ProtecoScene;

typedef enum {
    ProtecoViewSubmenu,
    ProtecoViewMain,
    ProtecoViewByteInput,
    ProtecoViewTextInput,
    ProtecoViewWidget,
    ProtecoViewAbout,
} ProtecoView;

typedef enum {
    // 0.. are submenu indexes
    ProtecoEventRxKey = 100,
    ProtecoEventOkPress,
    ProtecoEventOkRelease,
    ProtecoEventRight,
    ProtecoEventUp,
    ProtecoEventDown,
    ProtecoEventInputDone,
} ProtecoEvent;

typedef enum {
    ProtecoModeRead,
    ProtecoModeKey,
} ProtecoMode;

typedef struct {
    ProtecoMode mode;
    bool has_key;
    bool tx;
    uint32_t key;
    uint32_t frequency;
    uint16_t frames;
    float rssi;
    bool external;
    uint32_t te; // Read: measured Te of the last frame, 0 = none yet
    uint32_t gap; // Read: the guard before it, us
    char source[40];
} ProtecoModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    NotificationApp* notifications;
    DialogsApp* dialogs;
    Storage* storage;

    Submenu* submenu;
    View* main_view;
    ByteInput* byte_input;
    TextInput* text_input;
    Widget* widget;
    View* about_view;

    const SubGhzDevice* radio;
    bool radio_external;
    bool otg_by_app;
    uint32_t radio_probe_tick;
    SubGhzWorker* worker;
    ProtecoDecoder decoder;
    ProtecoEncoder encoder;
    bool rx_running;
    bool tx_running;
    bool tx_held;
    volatile uint32_t rx_key;
    volatile uint32_t rx_te;
    volatile uint32_t rx_gap;
    uint32_t frame_te; // of the last frame received in Read
    uint32_t frame_gap;

    // current key
    bool key_valid;
    uint32_t key;
    uint32_t frequency;
    uint16_t frames;
    FuriString* source;

    FuriString* path;
    FuriString* message;
    char name[48];
    uint8_t key_bytes[3];
} ProtecoApp;

/* ------------------------------- radio -------------------------------- */

static void proteco_rx_pair_callback(void* context, bool level, uint32_t duration) {
    ProtecoApp* app = context;
    uint32_t key;
    if(proteco_decoder_feed(&app->decoder, level, duration, &key)) {
        app->rx_te = app->decoder.frame_te;
        app->rx_gap = app->decoder.frame_guard;
        app->rx_key = key;
        view_dispatcher_send_custom_event(app->view_dispatcher, ProtecoEventRxKey);
    }
}

static void proteco_rx_overrun_callback(void* context) {
    ProtecoApp* app = context;
    proteco_decoder_reset(&app->decoder);
}

static void proteco_otg_on(ProtecoApp* app) {
    if(furi_hal_power_is_otg_enabled()) return;
    for(uint8_t i = 0; i < 5 && !furi_hal_power_is_otg_enabled(); i++) {
        furi_hal_power_enable_otg();
        furi_delay_ms(10); // CC1101 power-up time
    }
    app->otg_by_app = furi_hal_power_is_otg_enabled();
}

static void proteco_otg_off(ProtecoApp* app) {
    // 5V the user switched on for something else stays on
    if(app->otg_by_app) furi_hal_power_disable_otg();
    app->otg_by_app = false;
}

/** Use an external CC1101 when one answers, the internal one otherwise. Radio must be idle. */
static void proteco_radio_probe(ProtecoApp* app) {
    app->radio_probe_tick = furi_get_tick();
    // begin() is the external module's self test, so re-running it is how it gets noticed
    if(app->radio_external) subghz_devices_end(app->radio);

    const SubGhzDevice* internal = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    const SubGhzDevice* external = subghz_devices_get_by_name(PROTECO_RADIO_EXT);
    if(external) {
        proteco_otg_on(app);
        if(subghz_devices_begin(external)) {
            if(!app->radio_external) subghz_devices_sleep(internal);
            app->radio = external;
            app->radio_external = true;
            return;
        }
        subghz_devices_end(external); // begin() allocates before it self-tests
    }
    proteco_otg_off(app);
    app->radio = internal;
    app->radio_external = false;
}

/** Before RX/TX: notice a module pulled since the last look, or one plugged in. */
static void proteco_radio_check(ProtecoApp* app) {
    if(app->radio_external) {
        // a status read, cheap
        if(!subghz_devices_is_connect(app->radio)) proteco_radio_probe(app);
    } else if(furi_get_tick() - app->radio_probe_tick >= furi_ms_to_ticks(PROTECO_PROBE_MS)) {
        // searching costs an OTG power cycle and the driver timeout, so rate-limit it
        proteco_radio_probe(app);
    }
}

static bool proteco_radio_prepare(ProtecoApp* app, uint32_t frequency) {
    if(!subghz_devices_is_frequency_valid(app->radio, frequency)) return false;
    subghz_devices_reset(app->radio);
    subghz_devices_idle(app->radio);
    subghz_devices_load_preset(app->radio, FuriHalSubGhzPresetOok650Async, NULL);
    subghz_devices_set_frequency(app->radio, frequency);
    return true;
}

static void proteco_rx_start(ProtecoApp* app) {
    if(app->rx_running) return;
    proteco_radio_check(app);
    if(!proteco_radio_prepare(app, PROTECO_FREQUENCY)) return;
    proteco_decoder_reset(&app->decoder);
    subghz_devices_flush_rx(app->radio);
    subghz_devices_start_async_rx(app->radio, subghz_worker_rx_callback, app->worker);
    subghz_worker_start(app->worker);
    app->rx_running = true;
}

static void proteco_rx_stop(ProtecoApp* app) {
    if(!app->rx_running) return;
    subghz_worker_stop(app->worker);
    subghz_devices_stop_async_rx(app->radio);
    subghz_devices_idle(app->radio);
    app->rx_running = false;
}

static bool proteco_tx_start(ProtecoApp* app) {
    if(app->tx_running) return true;
    if(!proteco_radio_prepare(app, app->frequency)) return false;
    proteco_encoder_load(&app->encoder, app->key, PROTECO_TX_REPEAT);
    // set_tx refuses frequencies the region settings do not allow
    if(!subghz_devices_set_tx(app->radio) ||
       !subghz_devices_start_async_tx(app->radio, proteco_encoder_yield, &app->encoder)) {
        subghz_devices_idle(app->radio);
        return false;
    }
    app->tx_running = true;
    notification_message(app->notifications, &sequence_blink_start_magenta);
    return true;
}

static void proteco_tx_stop(ProtecoApp* app) {
    if(!app->tx_running) return;
    subghz_devices_stop_async_tx(app->radio);
    subghz_devices_idle(app->radio);
    app->tx_running = false;
    notification_message(app->notifications, &sequence_blink_stop);
}

/* -------------------------------- files ------------------------------- */

static bool proteco_save_key(ProtecoApp* app, const char* path) {
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    uint32_t bits = PROTECO_BITS;
    uint8_t key[8] = {0, 0, 0, 0, 0, app->key >> 16, app->key >> 8, app->key};
    bool ok = flipper_format_file_open_always(ff, path) &&
              flipper_format_write_header_cstr(ff, SUBGHZ_KEY_FILETYPE, 1) &&
              flipper_format_write_uint32(ff, "Frequency", &app->frequency, 1) &&
              flipper_format_write_string_cstr(ff, "Preset", PROTECO_PRESET) &&
              flipper_format_write_string_cstr(ff, "Protocol", PROTECO_PROTOCOL) &&
              flipper_format_write_uint32(ff, "Bit", &bits, 1) &&
              flipper_format_write_hex(ff, "Key", key, sizeof(key));
    flipper_format_free(ff);
    return ok;
}

/** Decode one sample of a file. RX is never running then, so app->decoder is free. */
static void proteco_decode_raw_sample(void* context, bool level, uint32_t duration) {
    ProtecoApp* app = context;
    uint32_t key;
    if(proteco_decoder_feed(&app->decoder, level, duration, &key)) {
        // keep the first key found, count its frames
        if(!app->frames) app->key = key;
        if(key == app->key) app->frames++;
    }
}

/** Feed every RAW_Data sample of an opened RAW file through the decoder. */
static bool proteco_decode_raw(ProtecoApp* app, FlipperFormat* ff) {
    uint32_t count = 0, capacity = 0;
    int32_t* samples = NULL;
    proteco_decoder_reset(&app->decoder);
    app->frames = 0;

    while(flipper_format_get_value_count(ff, "RAW_Data", &count) && count) {
        if(count > capacity) {
            free(samples);
            capacity = count;
            samples = malloc(capacity * sizeof(int32_t));
        }
        if(!flipper_format_read_int32(ff, "RAW_Data", samples, count)) break;
        for(uint32_t i = 0; i < count; i++) {
            proteco_decode_raw_sample(app, samples[i] > 0, (uint32_t)abs(samples[i]));
        }
    }
    // closing guard, so a frame that ends the file is counted too
    proteco_decode_raw_sample(app, false, RAW_END_GUARD_US);
    free(samples);
    return app->frames > 0;
}

/** Decode the Bit_RAW/Data_RAW chunks of an opened BinRAW key file. */
static bool proteco_decode_bin_raw(ProtecoApp* app, FlipperFormat* ff) {
    ProtecoBinRaw bin = {.callback = proteco_decode_raw_sample, .context = app};
    uint32_t te = 0, bits = 0;
    proteco_decoder_reset(&app->decoder);
    app->frames = 0;

    if(!flipper_format_read_uint32(ff, "TE", &te, 1) || !te) return false;
    // bin_raw.c keeps at most this much data
    while(flipper_format_read_uint32(ff, "Bit_RAW", &bits, 1) && bits &&
          bits <= BIN_RAW_MAX_BITS) {
        uint8_t* data = malloc((bits + 7) / 8);
        bool read = flipper_format_read_hex(ff, "Data_RAW", data, (bits + 7) / 8);
        if(read) proteco_bin_raw_feed(&bin, data, bits, te);
        free(data);
        if(!read) break;
    }
    proteco_bin_raw_flush(&bin);
    proteco_decode_raw_sample(app, false, RAW_END_GUARD_US);
    return app->frames > 0;
}

/** Load a Proteco key file, or decode a RAW capture. Sets app->message on failure. */
static bool proteco_load_file(ProtecoApp* app, const char* path) {
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    FuriString* filetype = furi_string_alloc();
    FuriString* protocol = furi_string_alloc();
    uint32_t version = 0, bits = 0;
    uint8_t key[8] = {0};
    bool ok = false;

    do {
        if(!flipper_format_file_open_existing(ff, path) ||
           !flipper_format_read_header(ff, filetype, &version)) {
            furi_string_set(app->message, "Cannot read file");
            break;
        }
        if(!flipper_format_read_uint32(ff, "Frequency", &app->frequency, 1)) {
            app->frequency = PROTECO_FREQUENCY;
        }
        if(!flipper_format_read_string(ff, "Protocol", protocol)) {
            furi_string_set(app->message, "No Protocol in file");
            break;
        }

        if(furi_string_equal(filetype, SUBGHZ_RAW_FILETYPE) &&
           furi_string_equal(protocol, "RAW")) {
            ok = proteco_decode_raw(app, ff);
            if(!ok) furi_string_set(app->message, "No Proteco signal\nfound in this RAW file");
        } else if(furi_string_equal(protocol, "BinRAW")) {
            ok = proteco_decode_bin_raw(app, ff);
            if(!ok) furi_string_set(app->message, "No Proteco signal\nfound in this BinRAW file");
        } else if(furi_string_equal(protocol, PROTECO_PROTOCOL)) {
            if(!flipper_format_read_uint32(ff, "Bit", &bits, 1) || bits != PROTECO_BITS ||
               !flipper_format_read_hex(ff, "Key", key, sizeof(key))) {
                furi_string_set(app->message, "Broken Proteco key file");
                break;
            }
            app->key = (key[5] << 16) | (key[6] << 8) | key[7];
            app->frames = 0;
            ok = true;
        } else {
            furi_string_printf(
                app->message,
                "Protocol %s\nis not Proteco, RAW\nor BinRAW",
                furi_string_get_cstr(protocol));
        }
    } while(false);

    if(ok) {
        app->key_valid = true;
        path_extract_filename_no_ext(path, app->source);
    }
    furi_string_free(protocol);
    furi_string_free(filetype);
    flipper_format_free(ff);
    return ok;
}

/* --------------------------------- view ------------------------------- */

static void proteco_view_update(ProtecoApp* app, ProtecoMode mode, float rssi) {
    with_view_model(
        app->main_view,
        ProtecoModel * model,
        {
            model->mode = mode;
            model->has_key = app->key_valid;
            model->tx = app->tx_running;
            model->key = app->key;
            model->frequency = (mode == ProtecoModeRead) ? PROTECO_FREQUENCY : app->frequency;
            model->frames = app->frames;
            model->te = (mode == ProtecoModeRead) ? app->frame_te : 0;
            model->gap = app->frame_gap;
            model->rssi = rssi;
            model->external = app->radio_external;
            snprintf(
                model->source, sizeof(model->source), "%s", furi_string_get_cstr(app->source));
        },
        true);
}

/** Draw the first of the candidate lines that fits the screen width. */
static void
    proteco_draw_fitting(Canvas* canvas, int32_t y, const char* const* lines, size_t count) {
    size_t i = 0;
    while(i + 1 < count && canvas_string_width(canvas, lines[i]) > 124)
        i++;
    canvas_draw_str(canvas, 2, y, lines[i]);
}

static void proteco_view_draw(Canvas* canvas, void* context) {
    ProtecoModel* model = context;
    bool read = model->mode == ProtecoModeRead;
    char line[64];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, read ? "Proteco Read" : "Proteco");

    canvas_set_font(canvas, FontSecondary);
    snprintf(
        line,
        sizeof(line),
        "%lu.%02lu MHz",
        model->frequency / 1000000,
        (model->frequency % 1000000) / 10000);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, line);

    if(model->has_key) {
        canvas_set_font(canvas, FontPrimary);
        snprintf(line, sizeof(line), "Key: 0x%06lX", model->key);
        canvas_draw_str(canvas, 2, 26, line);

        canvas_set_font(canvas, FontSecondary);
        snprintf(line, sizeof(line), "Btn %c", proteco_button(model->key));
        canvas_draw_str_aligned(canvas, 126, 26, AlignRight, AlignBottom, line);
        if(!read && !model->tx) {
            // Up/Down change the button
            int32_t x = 126 - canvas_string_width(canvas, line) - 5;
            canvas_draw_triangle(canvas, x, 21, 5, 3, CanvasDirectionBottomToTop);
            canvas_draw_triangle(canvas, x, 23, 5, 3, CanvasDirectionTopToBottom);
        }

        if(read && model->te) {
            // the frame's own clock and the gap before it, in its Te
            uint32_t gap10 = (model->gap * 10 + model->te / 2) / model->te;
            char full[64], mid[64], brief[64];
            snprintf(
                full,
                sizeof(full),
                "%u frames, Te %luus, gap %lu.%lu Te",
                model->frames,
                model->te,
                gap10 / 10,
                gap10 % 10);
            snprintf(
                mid,
                sizeof(mid),
                "x%u Te %luus gap %lu.%luTe",
                model->frames,
                model->te,
                gap10 / 10,
                gap10 % 10);
            snprintf(
                brief, sizeof(brief), "Te %luus gap %lu.%luTe", model->te, gap10 / 10, gap10 % 10);
            const char* const lines[] = {full, mid, brief};
            proteco_draw_fitting(canvas, 38, lines, 3);
        } else if(model->frames) {
            snprintf(line, sizeof(line), "%s, %u frames", model->source, model->frames);
            canvas_draw_str(canvas, 2, 38, line);
        } else {
            canvas_draw_str(canvas, 2, 38, model->source);
        }
        if(!read) {
            snprintf(line, sizeof(line), "Sn 0x%06lX", (model->key >> 2) & 0x3FFFFF);
            canvas_draw_str(canvas, 2, 49, line);
        }
    } else {
        canvas_draw_str(canvas, 2, 28, "Waiting for signal...");
        canvas_draw_str(canvas, 2, 38, "Press the remote button");
    }

    canvas_draw_str_aligned(
        canvas, 126, 49, AlignRight, AlignBottom, model->external ? "Ext radio" : "Int radio");

    if(read) {
        // RSSI bar, -100..-40 dBm
        float level = (model->rssi + 100.0f) / 60.0f;
        level = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
        canvas_draw_frame(canvas, 2, 43, 62, 6);
        canvas_draw_box(canvas, 3, 44, (uint8_t)(60 * level), 4);
        if(model->has_key) elements_button_center(canvas, "Use");
    } else if(model->tx) {
        elements_button_center(canvas, "Sending...");
    } else {
        elements_button_center(canvas, "Send");
        elements_button_right(canvas, "Save");
    }
}

static bool proteco_view_input(InputEvent* event, void* context) {
    ProtecoApp* app = context;
    if(event->key == InputKeyOk) {
        if(event->type == InputTypePress) {
            view_dispatcher_send_custom_event(app->view_dispatcher, ProtecoEventOkPress);
        } else if(event->type == InputTypeRelease) {
            view_dispatcher_send_custom_event(app->view_dispatcher, ProtecoEventOkRelease);
        }
        return true;
    }
    if(event->key == InputKeyRight && event->type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->view_dispatcher, ProtecoEventRight);
        return true;
    }
    if((event->key == InputKeyUp || event->key == InputKeyDown) &&
       (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, event->key == InputKeyUp ? ProtecoEventUp : ProtecoEventDown);
        return true;
    }
    return false;
}

/* -------------------------------- scenes ------------------------------ */

static void proteco_show_message(ProtecoApp* app, const char* text) {
    furi_string_set(app->message, text);
    scene_manager_next_scene(app->scene_manager, ProtecoSceneMessage);
}

static void proteco_submenu_callback(void* context, uint32_t index) {
    ProtecoApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void proteco_input_done_callback(void* context) {
    ProtecoApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, ProtecoEventInputDone);
}

// Start
static void proteco_scene_start_on_enter(void* context) {
    ProtecoApp* app = context;
    submenu_reset(app->submenu);
    submenu_add_item(app->submenu, "Read", ProtecoSceneRead, proteco_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Open / Decode .sub", ProtecoSceneOpen, proteco_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Add Manually", ProtecoSceneManual, proteco_submenu_callback, app);
    submenu_add_item(app->submenu, "About", ProtecoSceneAbout, proteco_submenu_callback, app);
    submenu_set_selected_item(
        app->submenu, scene_manager_get_scene_state(app->scene_manager, ProtecoSceneStart));
    view_dispatcher_switch_to_view(app->view_dispatcher, ProtecoViewSubmenu);
}

static bool proteco_scene_start_on_event(void* context, SceneManagerEvent event) {
    ProtecoApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    scene_manager_set_scene_state(app->scene_manager, ProtecoSceneStart, event.event);
    scene_manager_next_scene(app->scene_manager, event.event);
    return true;
}

static void proteco_scene_start_on_exit(void* context) {
    ProtecoApp* app = context;
    submenu_reset(app->submenu);
}

// Read
static void proteco_scene_read_on_enter(void* context) {
    ProtecoApp* app = context;
    app->key_valid = false;
    app->frames = 0;
    app->frequency = PROTECO_FREQUENCY;
    furi_string_set(app->source, "Received");
    app->frame_te = 0;
    proteco_rx_start(app);
    proteco_view_update(app, ProtecoModeRead, -100.0f);
    view_dispatcher_switch_to_view(app->view_dispatcher, ProtecoViewMain);
}

static bool proteco_scene_read_on_event(void* context, SceneManagerEvent event) {
    ProtecoApp* app = context;
    if(event.type == SceneManagerEventTypeTick) {
        // an external module pulled mid-RX keeps reading a steady RSSI, so ask it directly
        if(app->rx_running && app->radio_external && !subghz_devices_is_connect(app->radio)) {
            proteco_rx_stop(app);
            proteco_rx_start(app); // falls back to the internal radio
        }
        proteco_view_update(
            app, ProtecoModeRead, app->rx_running ? subghz_devices_get_rssi(app->radio) : -100.0f);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == ProtecoEventRxKey) {
        uint32_t key = app->rx_key;
        if(!app->key_valid || key != app->key) {
            app->key = key;
            app->frames = 0;
            notification_message(app->notifications, &sequence_success);
        }
        app->key_valid = true;
        app->frames++;
        app->frame_te = app->rx_te;
        app->frame_gap = app->rx_gap;
        return true;
    }
    if(event.event == ProtecoEventOkPress && app->key_valid) {
        scene_manager_next_scene(app->scene_manager, ProtecoSceneKey);
        return true;
    }
    return false;
}

static void proteco_scene_read_on_exit(void* context) {
    proteco_rx_stop(context);
}

// Open: pick a .sub, then load it or decode it
static void proteco_scene_open_on_enter(void* context) {
    ProtecoApp* app = context;
    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, PROTECO_EXT, NULL);
    options.base_path = PROTECO_FOLDER;

    if(!dialog_file_browser_show(app->dialogs, app->path, app->path, &options)) {
        scene_manager_previous_scene(app->scene_manager);
    } else if(proteco_load_file(app, furi_string_get_cstr(app->path))) {
        scene_manager_next_scene(app->scene_manager, ProtecoSceneKey);
    } else {
        notification_message(app->notifications, &sequence_error);
        scene_manager_next_scene(app->scene_manager, ProtecoSceneMessage);
    }
}

static bool proteco_scene_open_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

static void proteco_scene_open_on_exit(void* context) {
    UNUSED(context);
}

// Manual: type a 24 bit key
static void proteco_scene_manual_on_enter(void* context) {
    ProtecoApp* app = context;
    if(app->key_valid) {
        app->key_bytes[0] = app->key >> 16;
        app->key_bytes[1] = app->key >> 8;
        app->key_bytes[2] = app->key;
    }
    byte_input_set_header_text(app->byte_input, "Enter 24 bit key");
    byte_input_set_result_callback(
        app->byte_input,
        proteco_input_done_callback,
        NULL,
        app,
        app->key_bytes,
        sizeof(app->key_bytes));
    view_dispatcher_switch_to_view(app->view_dispatcher, ProtecoViewByteInput);
}

static bool proteco_scene_manual_on_event(void* context, SceneManagerEvent event) {
    ProtecoApp* app = context;
    if(event.type != SceneManagerEventTypeCustom || event.event != ProtecoEventInputDone) {
        return false;
    }
    app->key = (app->key_bytes[0] << 16) | (app->key_bytes[1] << 8) | app->key_bytes[2];
    app->key_valid = true;
    app->frames = 0;
    app->frequency = PROTECO_FREQUENCY;
    furi_string_set(app->source, "Manual");
    scene_manager_next_scene(app->scene_manager, ProtecoSceneKey);
    return true;
}

static void proteco_scene_manual_on_exit(void* context) {
    UNUSED(context);
}

// Key: show, send while OK is held, save
static void proteco_scene_key_on_enter(void* context) {
    ProtecoApp* app = context;
    proteco_view_update(app, ProtecoModeKey, 0);
    view_dispatcher_switch_to_view(app->view_dispatcher, ProtecoViewMain);
}

static bool proteco_scene_key_on_event(void* context, SceneManagerEvent event) {
    ProtecoApp* app = context;
    if(event.type == SceneManagerEventTypeTick) {
        // one burst is PROTECO_TX_REPEAT frames, start another while OK is still held
        if(app->tx_running && subghz_devices_is_async_complete_tx(app->radio)) {
            proteco_tx_stop(app);
            if(app->tx_held) proteco_tx_start(app);
            proteco_view_update(app, ProtecoModeKey, 0);
        }
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case ProtecoEventOkPress:
        if(!app->tx_running) proteco_radio_check(app);
        app->tx_held = true;
        if(!proteco_tx_start(app)) {
            app->tx_held = false;
            notification_message(app->notifications, &sequence_error);
            proteco_show_message(
                app, "Transmission refused\non this frequency.\nCheck region settings.");
        }
        proteco_view_update(app, ProtecoModeKey, 0);
        return true;
    case ProtecoEventOkRelease:
        app->tx_held = false;
        return true;
    case ProtecoEventRight:
        if(!app->tx_running) scene_manager_next_scene(app->scene_manager, ProtecoSceneSave);
        return true;
    case ProtecoEventUp:
    case ProtecoEventDown:
        // same key, another button of the remote: A, B, C, D (D is unverified)
        if(!app->tx_running) {
            app->key = proteco_button_step(app->key, event.event == ProtecoEventUp ? 1 : -1);
            app->frames = 0;
            proteco_view_update(app, ProtecoModeKey, 0);
        }
        return true;
    default:
        return false;
    }
}

static void proteco_scene_key_on_exit(void* context) {
    ProtecoApp* app = context;
    app->tx_held = false;
    proteco_tx_stop(app);
}

// Save: name the key file
static void proteco_scene_save_on_enter(void* context) {
    ProtecoApp* app = context;
    snprintf(app->name, sizeof(app->name), "Proteco_%06lX", app->key);
    text_input_set_header_text(app->text_input, "Name the key");
    text_input_set_result_callback(
        app->text_input, proteco_input_done_callback, app, app->name, sizeof(app->name), false);
    text_input_set_validator(
        app->text_input,
        validator_is_file_callback,
        validator_is_file_alloc_init(PROTECO_FOLDER, PROTECO_EXT, ""));
    view_dispatcher_switch_to_view(app->view_dispatcher, ProtecoViewTextInput);
}

static bool proteco_scene_save_on_event(void* context, SceneManagerEvent event) {
    ProtecoApp* app = context;
    if(event.type != SceneManagerEventTypeCustom || event.event != ProtecoEventInputDone) {
        return false;
    }
    storage_simply_mkdir(app->storage, PROTECO_FOLDER);
    FuriString* path = furi_string_alloc_printf("%s/%s%s", PROTECO_FOLDER, app->name, PROTECO_EXT);
    if(proteco_save_key(app, furi_string_get_cstr(path))) {
        furi_string_printf(app->source, "Saved %s", app->name);
        furi_string_set(app->path, path);
        notification_message(app->notifications, &sequence_success);
        scene_manager_previous_scene(app->scene_manager);
    } else {
        notification_message(app->notifications, &sequence_error);
        proteco_show_message(app, "Cannot save the key");
    }
    furi_string_free(path);
    return true;
}

static void proteco_scene_save_on_exit(void* context) {
    ProtecoApp* app = context;
    void* validator = text_input_get_validator_callback_context(app->text_input);
    text_input_set_validator(app->text_input, NULL, NULL);
    validator_is_file_free(validator);
    text_input_reset(app->text_input);
}

// Message: about text and errors
static void proteco_scene_message_on_enter(void* context) {
    ProtecoApp* app = context;
    widget_reset(app->widget);
    widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, furi_string_get_cstr(app->message));
    view_dispatcher_switch_to_view(app->view_dispatcher, ProtecoViewWidget);
}

static bool proteco_scene_message_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

static void proteco_scene_message_on_exit(void* context) {
    ProtecoApp* app = context;
    widget_reset(app->widget);
}

// About: animated credits page, Right for the help text
#define ABOUT_FRAMES 46 // one open/close cycle, 100 ms ticks

/** How far the garage door is rolled up, 0..20 px. */
static uint8_t proteco_about_door(uint8_t frame) {
    if(frame < 10) return 0;
    if(frame < 20) return (frame - 10) * 2;
    if(frame < 30) return 20;
    if(frame < 40) return (40 - frame) * 2;
    return 0;
}

static void proteco_about_draw(Canvas* canvas, void* context) {
    uint8_t frame = *(uint8_t*)context;
    uint8_t open = proteco_about_door(frame);

    canvas_clear(canvas);

    // garage: roof, walls, ground
    canvas_draw_line(canvas, 0, 12, 15, 1);
    canvas_draw_line(canvas, 15, 1, 30, 12);
    canvas_draw_line(canvas, 1, 12, 15, 2);
    canvas_draw_line(canvas, 15, 2, 29, 12);
    canvas_draw_line(canvas, 2, 12, 2, 37);
    canvas_draw_line(canvas, 28, 12, 28, 37);
    canvas_draw_line(canvas, 0, 38, 42, 38);

    // car parked inside
    canvas_draw_frame(canvas, 11, 26, 9, 5);
    canvas_draw_box(canvas, 8, 30, 15, 5);
    canvas_draw_disc(canvas, 11, 35, 1);
    canvas_draw_disc(canvas, 19, 35, 1);

    // sectional door over the 19x22 opening, rolled up by `open`
    uint8_t door = 22 - open;
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 6, 16, 19, door);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 6, 16, 19, 22);
    for(uint8_t y = 16; y < 16 + door; y += 3) {
        canvas_draw_line(canvas, 6, y, 24, y);
    }
    canvas_draw_line(canvas, 6, 15 + door, 24, 15 + door);

    // flashing warning lamp while the door moves
    if(open != 0 && open != 20 && (frame & 1)) canvas_draw_box(canvas, 13, 13, 5, 2);

    // remote signal arriving from the right, far arc first
    if(frame < 12) {
        uint8_t arcs = frame / 3 + 1;
        for(uint8_t i = 3 - (arcs > 3 ? 3 : arcs); i < 3; i++) {
            uint8_t x = 32 + i * 3, r = 2 + i * 2;
            canvas_draw_line(canvas, x + r / 2, 24 - r, x, 24 - r / 2);
            canvas_draw_line(canvas, x, 24 - r / 2, x, 24 + r / 2);
            canvas_draw_line(canvas, x, 24 + r / 2, x + r / 2, 24 + r);
        }
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 86, 10, AlignCenter, AlignBottom, "Proteco MOVER");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 86, 22, AlignCenter, AlignBottom, "Gate remote");
    canvas_draw_str_aligned(canvas, 86, 34, AlignCenter, AlignBottom, "by Dinko Korunic");
    canvas_draw_str_aligned(canvas, 64, 49, AlignCenter, AlignBottom, "dinko.korunic@gmail.com");
    canvas_draw_str(canvas, 2, 62, "433.92 MHz");
    elements_button_right(canvas, "Next");
}

static bool proteco_about_input(InputEvent* event, void* context) {
    ProtecoApp* app = context;
    if(event->key != InputKeyRight || event->type != InputTypeShort) return false;
    view_dispatcher_send_custom_event(app->view_dispatcher, ProtecoEventRight);
    return true;
}

static void proteco_scene_about_on_enter(void* context) {
    ProtecoApp* app = context;
    with_view_model(app->about_view, uint8_t * frame, { *frame = 0; }, true);
    view_dispatcher_switch_to_view(app->view_dispatcher, ProtecoViewAbout);
}

static bool proteco_scene_about_on_event(void* context, SceneManagerEvent event) {
    ProtecoApp* app = context;
    if(event.type == SceneManagerEventTypeTick) {
        with_view_model(
            app->about_view, uint8_t * frame, { *frame = (*frame + 1) % ABOUT_FRAMES; }, true);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom || event.event != ProtecoEventRight) return false;
    proteco_show_message(
        app,
        "Proteco MOVER gate remotes\n"
        "433.92 MHz AM650, 24 bit\n"
        "fixed code (CAME coding\n"
        "at Te 440us).\n\n"
        "Read: listen for a remote.\n"
        "Open: load a Proteco key\n"
        "or decode a RAW or\n"
        "BinRAW .sub.\n"
        "Add Manually: type a key.\n\n"
        "Key screen: hold OK to\n"
        "send, Up/Down to pick the\n"
        "button (A gate, B pedes-\n"
        "trian, C programming, D\n"
        "unverified), Right to save.\n"
        "Keys go to /ext/subghz.\n\n"
        "Created by Dinko Korunic\n"
        "dinko.korunic@gmail.com");
    return true;
}

static void proteco_scene_about_on_exit(void* context) {
    UNUSED(context);
}

static void (*const proteco_on_enter[])(void*) = {
    proteco_scene_start_on_enter,
    proteco_scene_read_on_enter,
    proteco_scene_open_on_enter,
    proteco_scene_manual_on_enter,
    proteco_scene_key_on_enter,
    proteco_scene_save_on_enter,
    proteco_scene_message_on_enter,
    proteco_scene_about_on_enter,
};

static bool (*const proteco_on_event[])(void*, SceneManagerEvent) = {
    proteco_scene_start_on_event,
    proteco_scene_read_on_event,
    proteco_scene_open_on_event,
    proteco_scene_manual_on_event,
    proteco_scene_key_on_event,
    proteco_scene_save_on_event,
    proteco_scene_message_on_event,
    proteco_scene_about_on_event,
};

static void (*const proteco_on_exit[])(void*) = {
    proteco_scene_start_on_exit,
    proteco_scene_read_on_exit,
    proteco_scene_open_on_exit,
    proteco_scene_manual_on_exit,
    proteco_scene_key_on_exit,
    proteco_scene_save_on_exit,
    proteco_scene_message_on_exit,
    proteco_scene_about_on_exit,
};

static const SceneManagerHandlers proteco_scene_handlers = {
    .on_enter_handlers = proteco_on_enter,
    .on_event_handlers = proteco_on_event,
    .on_exit_handlers = proteco_on_exit,
    .scene_num = ProtecoSceneNum,
};

/* ---------------------------------- app ------------------------------- */

static bool proteco_custom_event_callback(void* context, uint32_t event) {
    ProtecoApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool proteco_back_event_callback(void* context) {
    ProtecoApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void proteco_tick_event_callback(void* context) {
    ProtecoApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static ProtecoApp* proteco_app_alloc(void) {
    ProtecoApp* app = malloc(sizeof(ProtecoApp));
    memset(app, 0, sizeof(ProtecoApp));
    app->frequency = PROTECO_FREQUENCY;
    app->source = furi_string_alloc();
    app->path = furi_string_alloc_set(PROTECO_FOLDER);
    app->message = furi_string_alloc();

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->storage = furi_record_open(RECORD_STORAGE);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&proteco_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, proteco_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, proteco_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, proteco_tick_event_callback, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ProtecoViewSubmenu, submenu_get_view(app->submenu));

    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLocking, sizeof(ProtecoModel));
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, proteco_view_draw);
    view_set_input_callback(app->main_view, proteco_view_input);
    view_dispatcher_add_view(app->view_dispatcher, ProtecoViewMain, app->main_view);

    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ProtecoViewByteInput, byte_input_get_view(app->byte_input));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ProtecoViewTextInput, text_input_get_view(app->text_input));

    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ProtecoViewWidget, widget_get_view(app->widget));

    app->about_view = view_alloc();
    view_allocate_model(app->about_view, ViewModelTypeLocking, sizeof(uint8_t));
    view_set_context(app->about_view, app);
    view_set_draw_callback(app->about_view, proteco_about_draw);
    view_set_input_callback(app->about_view, proteco_about_input);
    view_dispatcher_add_view(app->view_dispatcher, ProtecoViewAbout, app->about_view);

    // external CC1101 when one is attached, like the stock Sub-GHz app
    subghz_devices_init();
    proteco_radio_probe(app);

    app->worker = subghz_worker_alloc();
    subghz_worker_set_overrun_callback(app->worker, proteco_rx_overrun_callback);
    subghz_worker_set_pair_callback(app->worker, proteco_rx_pair_callback);
    subghz_worker_set_context(app->worker, app);

    return app;
}

static void proteco_app_free(ProtecoApp* app) {
    proteco_rx_stop(app);
    proteco_tx_stop(app);
    subghz_worker_free(app->worker);
    // the internal radio's end() is a full shutdown, the stock app only puts it to sleep
    if(app->radio_external) {
        subghz_devices_end(app->radio);
    } else {
        subghz_devices_sleep(app->radio);
    }
    proteco_otg_off(app);
    subghz_devices_deinit();

    view_dispatcher_remove_view(app->view_dispatcher, ProtecoViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, ProtecoViewMain);
    view_dispatcher_remove_view(app->view_dispatcher, ProtecoViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, ProtecoViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, ProtecoViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, ProtecoViewAbout);
    submenu_free(app->submenu);
    view_free(app->main_view);
    byte_input_free(app->byte_input);
    text_input_free(app->text_input);
    widget_free(app->widget);
    view_free(app->about_view);
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    furi_string_free(app->message);
    furi_string_free(app->path);
    furi_string_free(app->source);
    free(app);
}

int32_t proteco_app(void* p) {
    UNUSED(p);
    ProtecoApp* app = proteco_app_alloc();
    scene_manager_next_scene(app->scene_manager, ProtecoSceneStart);
    view_dispatcher_run(app->view_dispatcher);
    proteco_app_free(app);
    return 0;
}
