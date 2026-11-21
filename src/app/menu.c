/*
 * menu.c - Hierarchical 12-category menu for the RT-950 Pro
 *
 * 4-state machine: CLOSED -> CATEGORY -> LIST -> EDIT
 * Get/set functions read from and write to real hardware modules.
 *
 * V0.27 OEM menu architecture:
 *   settings_menu_engine    @ 0x08011AC4 (17,740B, command dispatch on r1)
 *     CMP chain: r1=0x02(init), 0x03(nav), 0x05(scroll), 0x07(edit),
 *       0x10(exit), 0x11(focus), 0x12-0x18(ops), 0x1C,0x1F,0x24,0x25
 *     r1 >= 0xA0: channel/zone select mode
 *     Refs: g_config @ 0x2000A340, display_state @ 0x20000215
 *   display_menu_screen     @ 0x08010790 (menu overlay rendering)
 *   menu_list_renderer      @ 0x080108F4 (sub-item list)
 *   menu_item_draw          @ 0x08010BC8 (single item render)
 *   menu_settings_draw      @ 0x08010AD8 (settings value page)
 *   menu_scroll_handler     @ 0x080102DC (scroll position tracking)
 *   menu_handler_display    @ 0x0801145C (display settings handler, 296B)
 *   menu_handler_audio      @ 0x08011598 (audio settings handler, 272B)
 *   menu_handler_timer      @ 0x080116B8 (timer settings handler, 316B)
 *   menu_handler_scan       @ 0x08011808 (scan settings handler, 300B)
 *
 *   String table  @ 0x0805A440-0x0805BC00 (~7 KB)
 *     Entry format: 02 ID 20 "string" 00 (items), 07 20 "string" 00 (headers)
 *   Top-level    @ 0x0805B3D0-0x0805B442 (12 categories, IDs 0x12-0x1D)
 *
 * Our implementation: flat C arrays with get/set function pointers.
 * Settings persisted via flash wear-leveling (WL).
 * See flash_wearleveling.c for WL sector configs.
 */

#include "app/menu.h"
#include "app/display.h"
#include "app/keypad.h"
#include "app/vfo.h"
#include "app/vox.h"
#include "app/power.h"
#include "app/settings.h"
#include "app/text_input.h"
#include "app/aprs.h"
#include "app/crossband.h"
#include "drivers/bk4829.h"
#include "drivers/flash_layout.h"
#include "drivers/lcd.h"

#include <stddef.h>
#include <string.h>

#define COLOR_DKGRAY  COLOR_DARK_GRAY

#define MENU_X_PAD      8
#define MENU_AREA_H   280
#define MENU_VALUE_X  (LCD_WIDTH - 70)

enum {
    MENU_ENGLISH,
    MENU_TURKISH,
    MENU_RUSSIAN,
    MENU_LANGUAGE_COUNT,
};

typedef const char *menu_text_t[MENU_LANGUAGE_COUNT];

static const uint8_t language_values[] = {
    [MENU_ENGLISH] = SETTINGS_LANGUAGE_ENGLISH,
    [MENU_TURKISH] = SETTINGS_LANGUAGE_TURKISH,
    [MENU_RUSSIAN] = SETTINGS_LANGUAGE_RUSSIAN,
};

static uint8_t get_language(void)
{
    uint8_t stored = settings_get()->language;
    for (uint8_t i = 0; i < MENU_LANGUAGE_COUNT; i++) {
        if (stored == language_values[i])
            return i;
    }
    return MENU_ENGLISH;
}

static void set_language(uint8_t choice)
{
    if (choice < MENU_LANGUAGE_COUNT && settings_get()->language != language_values[choice])
        settings_set_u8(offsetof(settings_t, language), language_values[choice]);
}

static const char *localized(const char * const text[MENU_LANGUAGE_COUNT])
{
    return text[get_language()];
}

#define TEXT(en, tr, ru) localized((const char * const [MENU_LANGUAGE_COUNT]){en, tr, ru})

static const menu_text_t text_off = { "Off", "Kapalı", "Выкл." };
static const menu_text_t text_on = { "On", "Açık", "Вкл." };
static const menu_text_t text_save = { "OK=Save  #=Cancel", "OK=Kaydet  #=İptal", "OK=Сохранить  #=Отмена" };

/* Item descriptor */

typedef struct {
    menu_text_t name;
    uint8_t     min_val;
    uint8_t     max_val;
    uint8_t     (*get_val)(void);
    void        (*set_val)(uint8_t);
} menu_item_desc_t;

/* Category descriptor */

typedef struct {
    menu_text_t        name;
    const menu_item_t *items;
    uint8_t            count;
} menu_category_t;

/* Step-size lookup */

static const uint32_t step_table[8] = {
    2500, 5000, 6250, 10000, 12500, 25000, 50000, 100000
};

static uint8_t step_hz_to_index(uint32_t hz)
{
    for (uint8_t i = 0; i < 8; i++) {
        if (step_table[i] == hz)
            return i;
    }
    return 4;
}

static const uint16_t auto_off_minutes_table[4] = { 0, 30, 60, 120 };

/* Getter/setter pairs */

static uint8_t get_squelch(void) {
    return vfo_get_state(vfo_get_active())->squelch_level;
}
static void set_squelch(uint8_t v) {
    vfo_set_squelch(vfo_get_active(), v);
}

static uint8_t get_tx_power(void) {
    return vfo_get_state(vfo_get_active())->tx_power;
}
static void set_tx_power(uint8_t v) {
    vfo_set_power(vfo_get_active(), v);
}

static uint8_t get_bandwidth(void) {
    return vfo_get_state(vfo_get_active())->bandwidth;
}
static void set_bandwidth(uint8_t v) {
    vfo_set_bandwidth(vfo_get_active(), v);
    vfo_apply(vfo_get_active());
}

static uint8_t get_modulation(void) {
    return vfo_get_state(vfo_get_active())->modulation;
}
static void set_modulation(uint8_t v) {
    vfo_set_modulation(vfo_get_active(), v);
    vfo_apply(vfo_get_active());
}

static uint8_t get_step(void) {
    return step_hz_to_index(vfo_get_state(vfo_get_active())->step_hz);
}
static void set_step(uint8_t v) {
    if (v < 8)
        vfo_set_step(vfo_get_active(), step_table[v]);
}

static uint8_t get_ctcss_tx(void) {
    uint8_t idx = vfo_get_state(vfo_get_active())->ctcss_tx_idx;
    return (idx == 0xFF) ? 0 : (uint8_t)(idx + 1);
}
static void set_ctcss_tx(uint8_t v) {
    vfo_set_ctcss_tx(vfo_get_active(), (v == 0) ? 0xFF : (uint8_t)(v - 1));
}

static uint8_t get_ctcss_rx(void) {
    uint8_t idx = vfo_get_state(vfo_get_active())->ctcss_rx_idx;
    return (idx == 0xFF) ? 0 : (uint8_t)(idx + 1);
}
static void set_ctcss_rx(uint8_t v) {
    vfo_set_ctcss_rx(vfo_get_active(), (v == 0) ? 0xFF : (uint8_t)(v - 1));
}

static uint8_t get_dcs_tx(void) {
    uint8_t idx = vfo_get_state(vfo_get_active())->dcs_code_idx;
    return (idx == 0xFF) ? 0 : (uint8_t)(idx + 1);
}
static void set_dcs_tx(uint8_t v) {
    const vfo_state_t *s = vfo_get_state(vfo_get_active());
    vfo_set_dcs(vfo_get_active(), (v == 0) ? 0xFF : (uint8_t)(v - 1),
                s->dcs_polarity);
}

static uint8_t get_dcs_rx(void) {
    uint8_t idx = vfo_get_state(vfo_get_active())->dcs_code_idx;
    return (idx == 0xFF) ? 0 : (uint8_t)(idx + 1);
}
static void set_dcs_rx(uint8_t v) {
    const vfo_state_t *s = vfo_get_state(vfo_get_active());
    vfo_set_dcs(vfo_get_active(), (v == 0) ? 0xFF : (uint8_t)(v - 1),
                s->dcs_polarity);
}

static uint8_t get_scrambler(void) {
    return vfo_get_state(vfo_get_active())->scrambler;
}
static void set_scrambler(uint8_t v) {
    vfo_set_scrambler(vfo_get_active(), v);
}

static uint8_t get_vox_switch(void) { return settings_get()->vox_switch; }
static void    set_vox_switch(uint8_t v) {
    settings_set_u8(offsetof(settings_t, vox_switch), v);
}

static uint8_t get_vox_level(void) { return settings_get()->vox_level; }
static void    set_vox_level(uint8_t v) {
    settings_set_u8(offsetof(settings_t, vox_level), v);
    vox_set_level(v);
}

static uint8_t get_vox_delay(void) { return settings_get()->vox_delay; }
static void    set_vox_delay(uint8_t v) {
    settings_set_u8(offsetof(settings_t, vox_delay), v);
}

static uint8_t get_scan_add(void)      { return 0; }
static void    set_scan_add(uint8_t v) { (void)v; }

static uint8_t get_backlight(void) { return settings_get()->backlight; }
static void    set_backlight(uint8_t v) {
    settings_set_u8(offsetof(settings_t, backlight), v);
    switch (v) {
    case 0: power_backlight_off(); break;
    case 1: power_backlight_on();  break;
    default: break;
    }
}

static uint8_t get_auto_pwr_off(void) { return settings_get()->auto_pwr_off; }
static void    set_auto_pwr_off(uint8_t v) {
    settings_set_u8(offsetof(settings_t, auto_pwr_off), v);
    if (v < 4)
        power_set_auto_off(auto_off_minutes_table[v]);
}

static uint8_t get_dual_watch(void)      { return settings_get()->dual_watch; }
static void    set_dual_watch(uint8_t v) { settings_set_u8(offsetof(settings_t, dual_watch), v); }

static uint8_t get_beep(void)      { return settings_get()->beep; }
static void    set_beep(uint8_t v) { settings_set_u8(offsetof(settings_t, beep), v); }

static uint8_t get_key_lock(void)      { return settings_get()->keypad_lock; }
static void    set_key_lock(uint8_t v) { settings_set_u8(offsetof(settings_t, keypad_lock), v); }

static uint8_t get_aprs_enable(void)      { return settings_get()->aprs_enable; }
static void    set_aprs_enable(uint8_t v) { settings_set_u8(offsetof(settings_t, aprs_enable), v); }

static uint8_t get_aprs_symbol(void)
{
    const aprs_config_t *ac = aprs_config_get();
    return (ac->symbol_idx < 5) ? ac->symbol_idx : 0;
}
static void set_aprs_symbol(uint8_t v)
{
    aprs_config_t cfg;
    memcpy(&cfg, aprs_config_get(), sizeof(cfg));
    cfg.symbol_idx = v;
    static const char sym_codes[] = { '[', 'b', '>', 'R', 'K' };
    if (v < 5) cfg.symbol_code = sym_codes[v];
    cfg.symbol_table = '/';
    aprs_config_save(&cfg);
}

static uint8_t get_tot(void)      { return settings_get()->tot; }
static void    set_tot(uint8_t v) { settings_set_u8(offsetof(settings_t, tot), v); }

static uint8_t get_ste(void)      { return settings_get()->tail_elim; }
static void    set_ste(uint8_t v) { settings_set_u8(offsetof(settings_t, tail_elim), v); }

static uint8_t get_offset_dir(void) { return vfo_get_state(vfo_get_active())->offset_dir; }
static void    set_offset_dir(uint8_t v) { vfo_set_offset_dir(vfo_get_active(), v); }

static uint8_t get_offset_freq(void)
{
    return (uint8_t)(vfo_get_state(vfo_get_active())->offset_freq_hz / 100000U);
}
static void set_offset_freq(uint8_t v)
{
    vfo_set_offset_freq(vfo_get_active(), (uint32_t)v * 100000U);
}

static uint8_t get_bcl(void) { return vfo_get_state(vfo_get_active())->busy_lockout; }
static void    set_bcl(uint8_t v) { vfo_set_busy_lockout(vfo_get_active(), v); }

static uint8_t get_roger(void)      { return settings_get()->r_tone; }
static void    set_roger(uint8_t v) { settings_set_u8(offsetof(settings_t, r_tone), v); }

static uint8_t get_scan_mode(void)      { return settings_get()->scan_mode; }
static void    set_scan_mode(uint8_t v) { settings_set_u8(offsetof(settings_t, scan_mode), v); }

static uint8_t get_rpste(void)      { return settings_get()->rpste; }
static void    set_rpste(uint8_t v) { settings_set_u8(offsetof(settings_t, rpste), v); }

static uint8_t get_rpt_rl(void)      { return settings_get()->rpt_rl; }
static void    set_rpt_rl(uint8_t v) { settings_set_u8(offsetof(settings_t, rpt_rl), v); }

static uint8_t get_battery_save(void)      { return settings_get()->battery_save; }
static void    set_battery_save(uint8_t v) { settings_set_u8(offsetof(settings_t, battery_save), v); }

static uint8_t get_bt_switch(void)      { return settings_get()->bt_write_switch; }
static void    set_bt_switch(uint8_t v) { settings_set_u8(offsetof(settings_t, bt_write_switch), v); }

static uint8_t get_voice_prompt(void)      { return settings_get()->voice_prompt; }
static void    set_voice_prompt(uint8_t v) { settings_set_u8(offsetof(settings_t, voice_prompt), v); }

static uint8_t get_menu_timeout(void)      { return settings_get()->menu_out_time; }
static void    set_menu_timeout(uint8_t v) { settings_set_u8(offsetof(settings_t, menu_out_time), v); }

static uint8_t get_power_msg(void)      { return settings_get()->power_msg; }
static void    set_power_msg(uint8_t v) { settings_set_u8(offsetof(settings_t, power_msg), v); }

static uint8_t get_breath_led(void)      { return settings_get()->breathing_light; }
static void    set_breath_led(uint8_t v) { settings_set_u8(offsetof(settings_t, breathing_light), v); }

static uint8_t get_noaa_alarm(void)      { return settings_get()->noaa_alarm; }
static void    set_noaa_alarm(uint8_t v) { settings_set_u8(offsetof(settings_t, noaa_alarm), v); }

static uint8_t get_crossband(void)      { return settings_get()->crossband_mode; }
static void    set_crossband(uint8_t v) {
    settings_set_u8(offsetof(settings_t, crossband_mode), v);
    crossband_set_mode((xband_mode_t)v);
}

static uint8_t get_ptt_id(void)      { return settings_get()->ptt_id; }
static void    set_ptt_id(uint8_t v) { settings_set_u8(offsetof(settings_t, ptt_id), v); }

static uint8_t get_dtmf_st(void)      { return settings_get()->dtmf_st; }
static void    set_dtmf_st(uint8_t v) { settings_set_u8(offsetof(settings_t, dtmf_st), v); }

static uint8_t get_ch_name(void)      { return 0; }
static void    set_ch_name(uint8_t v) { (void)v; }

static uint8_t get_factory_reset(void)      { return 0; }
static void    set_factory_reset(uint8_t v) { if (v) settings_factory_reset(); }

static uint8_t get_about_version(void)      { return 0; }
static void    set_about_version(uint8_t v) { (void)v; }

/* Item descriptor table */

static const menu_item_desc_t menu_items[MENU_ITEM_COUNT] = {
    [MENU_VOX_SWITCH]     = { { "VOX Switch", "VOX Aç/Kapat", "Включение VOX" }, 0,   1, get_vox_switch,    set_vox_switch   },
    [MENU_VOX_LEVEL]      = { { "VOX Level", "VOX Seviyesi", "Уровень VOX" }, 0,   9, get_vox_level,     set_vox_level    },
    [MENU_VOX_DELAY]      = { { "VOX Delay", "VOX Gecikmesi", "Задержка VOX" }, 0,   7, get_vox_delay,     set_vox_delay    },
    [MENU_STEP]           = { { "Step", "Adım", "Шаг частоты" }, 0,   7, get_step,          set_step         },
    [MENU_OFFSET_DIR]     = { { "Direction", "Ofset Yönü", "Знак сдвига" }, 0,   2, get_offset_dir,    set_offset_dir   },
    [MENU_OFFSET_FREQ]    = { { "Offset", "Frekans Ofseti", "Сдвиг частоты" }, 0, 255, get_offset_freq,   set_offset_freq  },
    [MENU_MODULATION]     = { { "Modulation", "Modülasyon", "Модуляция" }, 0,   3, get_modulation,    set_modulation   },
    [MENU_CH_NAME]        = { { "Ch Name", "Kanal Adı", "Имя канала" }, 0,   0, get_ch_name,       set_ch_name      },
    [MENU_CTCSS_TX]       = { { "TX CTCSS", "TX CTCSS", "CTCSS передачи" }, 0,  50, get_ctcss_tx,      set_ctcss_tx     },
    [MENU_CTCSS_RX]       = { { "RX CTCSS", "RX CTCSS", "CTCSS приёма" }, 0,  50, get_ctcss_rx,      set_ctcss_rx     },
    [MENU_DCS_TX]         = { { "TX DCS", "TX DCS", "DCS передачи" }, 0, 104, get_dcs_tx,        set_dcs_tx       },
    [MENU_DCS_RX]         = { { "RX DCS", "RX DCS", "DCS приёма" }, 0, 104, get_dcs_rx,        set_dcs_rx       },
    [MENU_SCRAMBLER]      = { { "Encryption", "Karıştırıcı", "Скремблер" }, 0,   1, get_scrambler,     set_scrambler    },
    [MENU_SCAN_ADD]       = { { "Scan Add", "Taramaya Ekle", "В список сканирования" }, 0,   1, get_scan_add,      set_scan_add     },
    [MENU_SQUELCH]        = { { "Squelch", "Gürültü Susturma", "Шумоподавитель" }, 0,   9, get_squelch,       set_squelch      },
    [MENU_TX_POWER]       = { { "TX Power", "Gönderim Gücü", "Мощность передачи" }, 0,   2, get_tx_power,      set_tx_power     },
    [MENU_BANDWIDTH]      = { { "Bandwidth", "Bant Genişliği", "Ширина полосы" }, 0,   1, get_bandwidth,     set_bandwidth    },
    [MENU_BCL]            = { { "Busy Lock", "Meşgul Kanal Kilidi", "Блок. занятого канала" }, 0,   1, get_bcl,           set_bcl          },
    [MENU_TOT]            = { { "TOT", "Gönderim Süre Sınırı", "Таймер передачи" }, 0,   8, get_tot,           set_tot          },
    [MENU_STE]            = { { "Tail Elim", "Kuyruk Sesi Kesme", "Подавл. хвоста" }, 0,   1, get_ste,           set_ste          },
    [MENU_ROGER]          = { { "R-Tone", "Bitiş Tonu", "Тон конца передачи" }, 0,   4, get_roger,         set_roger        },
    [MENU_SCAN_MODE]      = { { "Scan Mode", "Tarama Modu", "Режим сканирования" }, 0,   2, get_scan_mode,     set_scan_mode    },
    [MENU_RPSTE]          = { { "RP-STE", "Röle Kuyruk Kesme", "Подавл. хвоста реп." }, 0,   1, get_rpste,         set_rpste        },
    [MENU_RPT_RL]         = { { "RPT-RL", "RPT-RL", "RPT-RL" }, 0,   1, get_rpt_rl,       set_rpt_rl       },
    [MENU_DUAL_WATCH]     = { { "Dual Watch", "İkili Dinleme", "Двойное наблюдение" }, 0,   1, get_dual_watch,    set_dual_watch   },
    [MENU_BATTERY_SAVE]   = { { "Batt Save", "Pil Tasarrufu", "Экономия батареи" }, 0,   1, get_battery_save,  set_battery_save },
    [MENU_CROSSBAND]      = { { "X-Band Rpt", "Çapraz Bant Rölesi", "Междиап. ретранслятор" }, 0,   3, get_crossband,     set_crossband    },
    [MENU_APRS_ENABLE]    = { { "APRS", "APRS", "APRS" }, 0,   1, get_aprs_enable,   set_aprs_enable  },
    [MENU_APRS_SYMBOL]    = { { "APRS Symbol", "APRS Simgesi", "Значок APRS" }, 0,   4, get_aprs_symbol,   set_aprs_symbol  },
    [MENU_BT_SWITCH]      = { { "Bluetooth", "Bluetooth", "Bluetooth" }, 0,   1, get_bt_switch,     set_bt_switch    },
    [MENU_PTT_ID]         = { { "PTT-ID", "PTT-ID", "PTT-ID" }, 0,   3, get_ptt_id,        set_ptt_id       },
    [MENU_DTMF_ST]        = { { "DTMF-ST", "DTMF Yan Tonu", "Подтон DTMF" }, 0,   1, get_dtmf_st,       set_dtmf_st      },
    [MENU_BEEP]           = { { "Beep", "Tuş Sesi", "Звук клавиш" }, 0,   1, get_beep,          set_beep         },
    [MENU_VOICE_PROMPT]   = { { "Voice", "Sesli Uyarı", "Голосовые подсказки" }, 0,   1, get_voice_prompt,  set_voice_prompt },
    [MENU_KEY_LOCK]       = { { "Key Lock", "Tuş Kilidi", "Блокировка клавиш" }, 0,   1, get_key_lock,      set_key_lock     },
    [MENU_MENU_TIMEOUT]   = { { "Menu Time", "Menü Süresi", "Время меню" }, 0,   4, get_menu_timeout,  set_menu_timeout },
    [MENU_POWER_MSG]      = { { "Power Msg", "Açılış Ekranı", "Экран запуска" }, 0,   2, get_power_msg,     set_power_msg    },
    [MENU_BREATH_LED]     = { { "Breath LED", "Yanıp Sönen LED", "Пульсирующий LED" }, 0,   1, get_breath_led,    set_breath_led   },
    [MENU_NOAA_ALARM]     = { { "NOAA Alert", "NOAA Uyarısı", "Уведомление NOAA" }, 0,   1, get_noaa_alarm,    set_noaa_alarm   },
    [MENU_BACKLIGHT]      = { { "Backlight", "Ekran Aydınlatması", "Подсветка" }, 0,   2, get_backlight,     set_backlight    },
    [MENU_AUTO_POWER_OFF] = { { "Auto PwrOff", "Otomatik Kapanma", "Автовыключение" }, 0,   3, get_auto_pwr_off,  set_auto_pwr_off },
    [MENU_FACTORY_RESET]  = { { "Reset All", "Her Şeyi Sıfırla", "Сбросить всё" }, 0,   1, get_factory_reset, set_factory_reset },
    [MENU_ABOUT_VERSION]  = { { "Version", "Sürüm", "Версия" }, 0,   0, get_about_version, set_about_version },
    [MENU_LANGUAGE]      = { { "Language", "Dil", "Язык" }, MENU_ENGLISH, MENU_RUSSIAN, get_language, set_language },
};

/* Per-category item arrays */

static const menu_item_t cat_vox[] = {
    MENU_VOX_SWITCH, MENU_VOX_LEVEL, MENU_VOX_DELAY
};

static const menu_item_t cat_vfo_ch[] = {
    MENU_STEP, MENU_OFFSET_DIR, MENU_OFFSET_FREQ, MENU_MODULATION, MENU_CH_NAME
};

static const menu_item_t cat_ctcss_dcs[] = {
    MENU_CTCSS_TX, MENU_CTCSS_RX, MENU_DCS_TX, MENU_DCS_RX,
    MENU_SCRAMBLER, MENU_SCAN_ADD
};

static const menu_item_t cat_radio_set[] = {
    MENU_SQUELCH, MENU_TX_POWER, MENU_BANDWIDTH, MENU_BCL,
    MENU_TOT, MENU_STE, MENU_ROGER, MENU_SCAN_MODE,
    MENU_RPSTE, MENU_RPT_RL, MENU_DUAL_WATCH, MENU_BATTERY_SAVE,
    MENU_CROSSBAND
};

static const menu_item_t cat_aprs[] = {
    MENU_APRS_ENABLE, MENU_APRS_SYMBOL
};

static const menu_item_t cat_bluetooth[] = {
    MENU_BT_SWITCH
};

static const menu_item_t cat_signaling[] = {
    MENU_PTT_ID, MENU_DTMF_ST
};

static const menu_item_t cat_setting[] = {
    MENU_BEEP, MENU_VOICE_PROMPT, MENU_KEY_LOCK, MENU_MENU_TIMEOUT,
    MENU_POWER_MSG, MENU_BREATH_LED, MENU_NOAA_ALARM,
    MENU_BACKLIGHT, MENU_AUTO_POWER_OFF, MENU_LANGUAGE
};

static const menu_item_t cat_reset[] = {
    MENU_FACTORY_RESET
};

static const menu_item_t cat_about[] = {
    MENU_ABOUT_VERSION
};

/*
 * Category table (OEM order @ 0x0805B3D0).
 * Zone (0x13) and User Key (0x18) are stub categories in both
 * OEM V0.27 and our code - minimal implementation.
 */

static const menu_category_t categories[MENU_CATEGORY_COUNT] = {
    { { "VOX", "VOX", "VOX" }, cat_vox,       3 },
    { { "Zone", "Bölge", "Зона" }, NULL,          0 },   /* OEM ID 0x13 - stub */
    { { "VFO & CH", "VFO ve Kanal", "VFO и канал" }, cat_vfo_ch,    5 },
    { { "CTCSS DCS", "CTCSS DCS", "CTCSS DCS" }, cat_ctcss_dcs, 6 },
    { { "Radio Set", "Telsiz Ayarları", "Радио" }, cat_radio_set, 13 },
    { { "APRS Set", "APRS Ayarları", "Настройки APRS" }, cat_aprs,      2 },
    { { "User Key", "İşlev Tuşu", "Функц. клавиша" }, NULL,          0 },   /* OEM ID 0x18 - stub */
    { { "Bluetooth", "Bluetooth", "Bluetooth" }, cat_bluetooth, 1 },
    { { "Signaling", "Sinyalleşme", "Сигнализация" }, cat_signaling, 2 },
    { { "Setting", "Ayarlar", "Настройки" }, cat_setting, sizeof cat_setting / sizeof cat_setting[0] },
    { { "Reset", "Sıfırlama", "Сброс" }, cat_reset,     1 },
    { { "About", "Hakkında", "Об устройстве" }, cat_about,     1 },
};

/* State */

static menu_state_t state;
static uint8_t      cur_category;
static uint8_t      cur_item_idx;
static uint8_t      edit_value;
static uint8_t      list_scroll;

static char     ch_name_buf[13];
static uint16_t ch_name_ch_num;

/* Helpers */

static const menu_category_t *cur_cat(void)
{
    return &categories[cur_category];
}

static menu_item_t cur_menu_item(void)
{
    const menu_category_t *c = cur_cat();
    if (c->items && cur_item_idx < c->count)
        return c->items[cur_item_idx];
    return (menu_item_t)0;
}

static uint8_t uint_to_buf(char *buf, unsigned int num)
{
    char tmp[10];
    uint8_t ti = 0;
    if (num == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (num > 0) { tmp[ti++] = (char)('0' + (num % 10)); num /= 10; }
    for (uint8_t j = 0; j < ti; j++) buf[j] = tmp[ti - 1 - j];
    buf[ti] = '\0';
    return ti;
}

static char fmt_buf[16];

static const char *format_value(menu_item_t item, uint8_t val)
{
    switch (item) {
    case MENU_LANGUAGE: {
        static const char * const names[] = { "English", "Türkçe", "Русский" };
        return (val < MENU_LANGUAGE_COUNT) ? names[val] : "?";
    }
    case MENU_SQUELCH:
        if (val == 0) return TEXT("Open", "Açık", "Открыт");
        fmt_buf[0] = (char)('0' + val); fmt_buf[1] = '\0';
        return fmt_buf;
    case MENU_TX_POWER: {
        static const menu_text_t pwr[] = {
            { "Low", "Düşük", "Низкая" },
            { "Mid", "Orta", "Средняя" },
            { "High", "Yüksek", "Высокая" },
        };
        return (val <= 2) ? localized(pwr[val]) : "?";
    }
    case MENU_BANDWIDTH:
        return val ? TEXT("Wide", "Geniş", "Широкая") : TEXT("Narrow", "Dar", "Узкая");
    case MENU_MODULATION: {
        static const char * const mod[] = { "FM", "AM", "USB", "LSB" };
        return (val <= 3) ? mod[val] : "?";
    }
    case MENU_STEP: {
        static const char * const stp[] = {
            "2.5k", "5.0k", "6.25k", "10k", "12.5k", "25k", "50k", "100k"
        };
        return (val <= 7) ? stp[val] : "?";
    }
    case MENU_CTCSS_TX:
    case MENU_CTCSS_RX:
        if (val == 0) return localized(text_off);
        if (val <= CTCSS_TONE_COUNT) {
            uint16_t tone = ctcss_tone_table[val - 1];
            uint8_t p = uint_to_buf(fmt_buf, tone / 10);
            fmt_buf[p] = '.';
            fmt_buf[p + 1] = (char)('0' + (tone % 10));
            fmt_buf[p + 2] = '\0';
            return fmt_buf;
        }
        return "?";
    case MENU_DCS_TX:
    case MENU_DCS_RX:
        if (val == 0) return localized(text_off);
        if (val <= DCS_CODE_COUNT) {
            uint16_t code = dcs_code_table[val - 1];
            fmt_buf[0] = (char)('0' + ((code / 100) % 10));
            fmt_buf[1] = (char)('0' + ((code / 10) % 10));
            fmt_buf[2] = (char)('0' + (code % 10));
            fmt_buf[3] = '\0';
            return fmt_buf;
        }
        return "?";
    case MENU_SCRAMBLER:
    case MENU_VOX_SWITCH:
    case MENU_DUAL_WATCH:
    case MENU_BEEP:
    case MENU_KEY_LOCK:
    case MENU_STE:
    case MENU_BCL:
    case MENU_APRS_ENABLE:
    case MENU_RPSTE:
    case MENU_RPT_RL:
    case MENU_BATTERY_SAVE:
    case MENU_BT_SWITCH:
    case MENU_VOICE_PROMPT:
    case MENU_BREATH_LED:
    case MENU_NOAA_ALARM:
    case MENU_SCAN_ADD:
    case MENU_DTMF_ST:
        return localized(val ? text_on : text_off);
    case MENU_VOX_LEVEL:
        if (val == 0) return localized(text_off);
        fmt_buf[0] = (char)('0' + val); fmt_buf[1] = '\0';
        return fmt_buf;
    case MENU_VOX_DELAY: {
        static const char * const vdly[] = {
            "0.5s", "1.0s", "1.5s", "2.0s", "2.5s", "3.0s", "3.5s", "4.0s"
        };
        return (val <= 7) ? vdly[val] : "?";
    }
    case MENU_TOT: {
        static const char * const tot_str[] = { "30s", "60s", "90s", "120s", "150s", "180s", "210s", "240s" };
        if (val == 0) return localized(text_off);
        return (val <= 8) ? tot_str[val - 1] : "?";
    }
    case MENU_OFFSET_DIR: {
        if (val == 0) return TEXT("None", "Yok", "Нет");
        return (val == 1) ? "+" : (val == 2) ? "-" : "?";
    }
    case MENU_OFFSET_FREQ: {
        uint8_t mhz = val / 10, frac = val % 10, p = 0;
        if (mhz >= 10) fmt_buf[p++] = (char)('0' + mhz / 10);
        fmt_buf[p++] = (char)('0' + mhz % 10);
        fmt_buf[p++] = '.'; fmt_buf[p++] = (char)('0' + frac);
        fmt_buf[p++] = 'M'; fmt_buf[p] = '\0';
        return fmt_buf;
    }
    case MENU_ROGER: {
        static const char * const rtone[] = { "1000Hz", "1450Hz", "1750Hz", "2100Hz" };
        if (val == 0) return localized(text_off);
        return (val <= 4) ? rtone[val - 1] : "?";
    }
    case MENU_SCAN_MODE: {
        static const char * const sm[] = { "TO", "CO", "SE" };
        return (val <= 2) ? sm[val] : "?";
    }
    case MENU_BACKLIGHT: {
        if (val <= 1) return localized(val ? text_on : text_off);
        return (val == 2) ? TEXT("Auto", "Otomatik", "Авто") : "?";
    }
    case MENU_AUTO_POWER_OFF: {
        static const menu_text_t apo[] = {
            { "30min", "30 dk", "30 мин" },
            { "1hr", "1 saat", "1 ч" },
            { "2hr", "2 saat", "2 ч" },
        };
        if (val == 0) return localized(text_off);
        return (val <= 3) ? localized(apo[val - 1]) : "?";
    }
    case MENU_MENU_TIMEOUT: {
        static const char * const mt[] = { "5s", "10s", "15s", "20s", "30s" };
        return (val <= 4) ? mt[val] : "?";
    }
    case MENU_POWER_MSG: {
        static const menu_text_t pm[] = {
            { "Icons", "Simgeler", "Значки" },
            { "Full", "Tam", "Полный" },
            { "Voltage", "Gerilim", "Напряжение" },
        };
        return (val <= 2) ? localized(pm[val]) : "?";
    }
    case MENU_PTT_ID: {
        if (val == 0) return localized(text_off);
        if (val == 1) return "BOT";
        if (val == 2) return "EOT";
        return (val == 3) ? TEXT("Both", "İkisi", "Оба") : "?";
    }
    case MENU_CROSSBAND: {
        if (val == 0) return localized(text_off);
        if (val == 1) return "A->B";
        if (val == 2) return "B->A";
        return (val == 3) ? TEXT("Duplex", "Dupleks", "Дуплекс") : "?";
    }
    case MENU_FACTORY_RESET:
        return val ? TEXT("Yes!", "Evet!", "Да!") : TEXT("No", "Hayır", "Нет");
    case MENU_CH_NAME:
        return TEXT("Edit...", "Düzenle...", "Изменить...");
    case MENU_APRS_SYMBOL: {
        static const menu_text_t sym[] = {
            { "Jogger", "Koşucu", "Бегун" },
            { "Bicycle", "Bisiklet", "Велосипед" },
            { "Car", "Otomobil", "Автомобиль" },
            { "RV", "Karavan", "Автодом" },
            { "SUV", "SUV", "SUV" },
        };
        return (val < 5) ? localized(sym[val]) : "?";
    }
    case MENU_ABOUT_VERSION:
        return "1.0";
    case MENU_ITEM_COUNT:
    default:
        return "?";
    }
}

static int8_t key_to_digit(uint8_t key)
{
    switch (key) {
    case KEY_0: return 0; case KEY_1: return 1; case KEY_2: return 2;
    case KEY_3: return 3; case KEY_4: return 4; case KEY_5: return 5;
    case KEY_6: return 6; case KEY_7: return 7; case KEY_8: return 8;
    case KEY_9: return 9; default: return -1;
    }
}

static int is_vfo_item(menu_item_t item)
{
    switch (item) {
    case MENU_STEP: case MENU_OFFSET_DIR: case MENU_OFFSET_FREQ:
    case MENU_MODULATION: case MENU_BANDWIDTH:
    case MENU_CTCSS_TX: case MENU_CTCSS_RX:
    case MENU_DCS_TX: case MENU_DCS_RX:
    case MENU_SCRAMBLER: case MENU_SQUELCH:
    case MENU_TX_POWER: case MENU_BCL:
        return 1;
    default:
        return 0;
    }
}

/* Public API */

void menu_init(void)
{
    state = MENU_STATE_CLOSED;
    cur_category = 0;
    cur_item_idx = 0;
    edit_value = 0;
    list_scroll = 0;
}

void menu_open(void)
{
    state = MENU_STATE_CATEGORY;
    cur_category = 0;
}

void menu_close(void) { state = MENU_STATE_CLOSED; }

menu_state_t menu_get_state(void) { return state; }

void menu_handle_encoder(int8_t direction)
{
    switch (state) {
    case MENU_STATE_CATEGORY:
        if (direction > 0 && cur_category < MENU_CATEGORY_COUNT - 1)
            cur_category++;
        else if (direction < 0 && cur_category > 0)
            cur_category--;
        break;
    case MENU_STATE_LIST: {
        const menu_category_t *c = cur_cat();
        if (direction > 0 && cur_item_idx < c->count - 1)
            cur_item_idx++;
        else if (direction < 0 && cur_item_idx > 0)
            cur_item_idx--;
        if (cur_item_idx < list_scroll)
            list_scroll = cur_item_idx;
        if (cur_item_idx >= list_scroll + 6)
            list_scroll = cur_item_idx - 5;
        break;
    }
    case MENU_STATE_EDIT: {
        menu_item_t mi = cur_menu_item();
        if (mi == MENU_CH_NAME && text_input_is_active()) {
            text_input_handle_encoder(direction);
            break;
        }
        if (direction > 0 && edit_value < menu_items[mi].max_val)
            edit_value++;
        else if (direction < 0 && edit_value > menu_items[mi].min_val)
            edit_value--;
        break;
    }
    default: break;
    }
}

void menu_handle_key(uint8_t key)
{
    switch (state) {
    case MENU_STATE_CLOSED:
        if (key == KEY_C_MENU) menu_open();
        break;
    case MENU_STATE_CATEGORY:
        if (key == KEY_C_MENU) {
            const menu_category_t *c = cur_cat();
            if (c->items && c->count > 0) {
                cur_item_idx = 0;
                list_scroll = 0;
                state = MENU_STATE_LIST;
            }
        } else if (key == KEY_HASH) {
            menu_close();
        }
        break;
    case MENU_STATE_LIST: {
        menu_item_t mi = cur_menu_item();
        if (key == KEY_C_MENU) {
            if (mi == MENU_CH_NAME) {
                ch_name_ch_num = 0;
                channel_record_t rec;
                if (flash_read_channel(ch_name_ch_num, &rec) == 0) {
                    for (uint8_t i = 0; i < 12; i++)
                        ch_name_buf[i] = (char)rec.name[i];
                    ch_name_buf[12] = '\0';
                } else {
                    for (uint8_t i = 0; i < 12; i++)
                        ch_name_buf[i] = ' ';
                    ch_name_buf[12] = '\0';
                }
                text_input_start(ch_name_buf, 12);
                state = MENU_STATE_EDIT;
                break;
            }
            if (mi == MENU_ABOUT_VERSION) break;
            edit_value = menu_items[mi].get_val();
            state = MENU_STATE_EDIT;
        } else if (key == KEY_HASH) {
            state = MENU_STATE_CATEGORY;
        }
        break;
    }
    case MENU_STATE_EDIT: {
        menu_item_t mi = cur_menu_item();
        if (mi == MENU_CH_NAME && text_input_is_active()) {
            text_input_handle_key(key);
            if (!text_input_is_active()) {
                if (text_input_confirmed()) {
                    channel_record_t rec;
                    if (flash_read_channel(ch_name_ch_num, &rec) == 0) {
                        for (uint8_t i = 0; i < 12; i++)
                            rec.name[i] = (uint8_t)ch_name_buf[i];
                        flash_write_channel(ch_name_ch_num, &rec);
                    }
                }
                state = MENU_STATE_LIST;
            }
            break;
        }
        if (key == KEY_C_MENU) {
            menu_items[mi].set_val(edit_value);
            if (is_vfo_item(mi)) vfo_save();
            state = MENU_STATE_LIST;
        } else if (key == KEY_HASH) {
            state = MENU_STATE_LIST;
        } else {
            int8_t digit = key_to_digit(key);
            if (digit >= 0) {
                uint8_t d = (uint8_t)digit;
                if (d >= menu_items[mi].min_val && d <= menu_items[mi].max_val)
                    edit_value = d;
            }
        }
        break;
    }
    default: break;
    }
}

/* Drawing */

#define LIST_VISIBLE  6
#define LIST_Y_START 30
#define LIST_LINE_H  20

static void draw_category_view(void)
{
    display_draw_text(MENU_X_PAD, 5, TEXT("MENU", "MENÜ", "МЕНЮ"), COLOR_WHITE, COLOR_BLACK);
    display_draw_hline(0, 18, LCD_WIDTH, COLOR_DKGRAY);
    if (cur_category > 0)
        display_draw_text(MENU_X_PAD + 16, 35, localized(categories[cur_category - 1].name),
                          COLOR_GRAY, COLOR_BLACK);
    display_draw_text(MENU_X_PAD, 65, ">", COLOR_WHITE, COLOR_BLACK);
    display_draw_text(MENU_X_PAD + 16, 65, localized(cur_cat()->name), COLOR_WHITE, COLOR_BLACK);
    if (categories[cur_category].count > 0)
        display_printf(MENU_X_PAD + 16, 85, COLOR_CYAN, COLOR_BLACK,
                       TEXT("%u items", "%u öğe", "Пунктов: %u"), (unsigned)cur_cat()->count);
    else
        display_draw_text(MENU_X_PAD + 16, 85, TEXT("(not available)", "(kullanılamıyor)", "(недоступно)"),
                          COLOR_DKGRAY, COLOR_BLACK);
    if (cur_category < MENU_CATEGORY_COUNT - 1)
        display_draw_text(MENU_X_PAD + 16, 115, localized(categories[cur_category + 1].name),
                          COLOR_GRAY, COLOR_BLACK);
    display_printf(MENU_X_PAD, 145, COLOR_DKGRAY, COLOR_BLACK,
                   "%u/%u", (unsigned)(cur_category + 1), (unsigned)MENU_CATEGORY_COUNT);
    display_draw_hline(0, 160, LCD_WIDTH, COLOR_DKGRAY);
    display_draw_text(MENU_X_PAD, 168, TEXT("OK=Enter  #=Exit", "OK=Aç  #=Çık", "OK=Открыть  #=Выход"),
                      COLOR_GRAY, COLOR_BLACK);
}

static void draw_list_view(void)
{
    const menu_category_t *c = cur_cat();
    display_draw_text(MENU_X_PAD, 5, localized(c->name), COLOR_WHITE, COLOR_BLACK);
    display_draw_hline(0, 18, LCD_WIDTH, COLOR_DKGRAY);
    for (uint8_t vi = 0; vi < LIST_VISIBLE && (list_scroll + vi) < c->count; vi++) {
        uint8_t idx = list_scroll + vi;
        menu_item_t mi = c->items[idx];
        uint16_t y = LIST_Y_START + vi * LIST_LINE_H;
        uint16_t nc = (idx == cur_item_idx) ? COLOR_WHITE : COLOR_GRAY;
        uint16_t vc = (idx == cur_item_idx) ? COLOR_CYAN : COLOR_DKGRAY;
        if (idx == cur_item_idx)
            display_draw_text(MENU_X_PAD, y, ">", COLOR_WHITE, COLOR_BLACK);
        display_draw_text(MENU_X_PAD + 12, y, localized(menu_items[mi].name), nc, COLOR_BLACK);
        uint8_t val = menu_items[mi].get_val();
        const char *vstr = format_value(mi, val);
        display_draw_text(MENU_VALUE_X, y, vstr, vc, COLOR_BLACK);
    }
    if (list_scroll > 0)
        display_draw_text(LCD_WIDTH - 16, LIST_Y_START - 10, "^", COLOR_DKGRAY, COLOR_BLACK);
    if (list_scroll + LIST_VISIBLE < c->count)
        display_draw_text(LCD_WIDTH - 16, LIST_Y_START + LIST_VISIBLE * LIST_LINE_H,
                          "v", COLOR_DKGRAY, COLOR_BLACK);
    display_printf(MENU_X_PAD, LIST_Y_START + LIST_VISIBLE * LIST_LINE_H + 10,
                   COLOR_DKGRAY, COLOR_BLACK, "%u/%u",
                   (unsigned)(cur_item_idx + 1), (unsigned)c->count);
    display_draw_hline(0, 180, LCD_WIDTH, COLOR_DKGRAY);
    display_draw_text(MENU_X_PAD, 188, TEXT("OK=Edit  #=Back", "OK=Düzenle  #=Geri", "OK=Изменить  #=Назад"),
                      COLOR_GRAY, COLOR_BLACK);
}

static void draw_edit_view(void)
{
    menu_item_t mi = cur_menu_item();
    if (mi == MENU_CH_NAME && text_input_is_active()) {
        display_draw_text(MENU_X_PAD, 5, TEXT("EDIT NAME", "ADI DÜZENLE", "ИЗМЕНИТЬ ИМЯ"),
                          COLOR_YELLOW, COLOR_BLACK);
        display_draw_hline(0, 18, LCD_WIDTH, COLOR_DKGRAY);
        text_input_draw(TEXT("Edit Name:", "Adı düzenle:", "Изменить имя:"));
        display_draw_hline(0, 120, LCD_WIDTH, COLOR_DKGRAY);
        display_draw_text(MENU_X_PAD, 130, localized(text_save), COLOR_GRAY, COLOR_BLACK);
        display_draw_text(MENU_X_PAD, 150, TEXT("2-9=Letters *=Del", "2-9=Harf  *=Sil", "2-9=Буквы  *=Удалить"),
                          COLOR_DKGRAY, COLOR_BLACK);
        return;
    }
    const char *name = localized(menu_items[mi].name);
    display_printf(MENU_X_PAD, 5, COLOR_WHITE, COLOR_BLACK,
                   "%s > %s", localized(cur_cat()->name), name);
    display_draw_hline(0, 18, LCD_WIDTH, COLOR_DKGRAY);
    display_draw_text(MENU_X_PAD, 50, name, COLOR_WHITE, COLOR_BLACK);
    {
        const char *vstr = format_value(mi, edit_value);
        display_printf(MENU_X_PAD, 80, COLOR_YELLOW, COLOR_BLACK, "< %s >", vstr);
    }
    if (mi != MENU_LANGUAGE)
        display_printf(MENU_X_PAD, 110, COLOR_GRAY, COLOR_BLACK,
                       "%u..%u", (unsigned)menu_items[mi].min_val,
                       (unsigned)menu_items[mi].max_val);
    display_draw_hline(0, 135, LCD_WIDTH, COLOR_DKGRAY);
    display_draw_text(MENU_X_PAD, 145, localized(text_save), COLOR_GRAY, COLOR_BLACK);
    display_draw_text(MENU_X_PAD, 165,
                      TEXT("Encoder to adjust", "Ayarlamak için düğmeyi çevir", "Вращайте ручку для настройки"),
                      COLOR_DKGRAY, COLOR_BLACK);
}

void menu_draw(void)
{
    if (state == MENU_STATE_CLOSED) return;
    lcd_fill_rect(0, 0, LCD_WIDTH, MENU_AREA_H, COLOR_BLACK);
    switch (state) {
    case MENU_STATE_CATEGORY: draw_category_view(); break;
    case MENU_STATE_LIST:     draw_list_view();     break;
    case MENU_STATE_EDIT:     draw_edit_view();     break;
    default: break;
    }
}
