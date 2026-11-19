/*
 * settings.c - Persistent system settings for RT-950 Pro
 *
 * Reads/writes system settings through the WL_SYSCFG wear-leveled
 * flash sector. The in-RAM settings_t struct is the authoritative
 * copy; all getters read from it, all setters write through to flash.
 */

#include "app/settings.h"
#include "drivers/flash_wearleveling.h"

#include <string.h>
#include <stddef.h>

/* Live settings in RAM ------------------------------------------------ */

static settings_t settings;

/* Factory defaults (from CPS S_3941 capture) -------------------------- */

const settings_t settings_defaults = {
    .sql_level          = 3,
    .battery_save       = 0,        /* OFF */
    .vox_level          = 1,
    .vox_switch         = 0,        /* OFF */
    .vox_delay          = 1,        /* 1.0s */
    .backlight          = 4,        /* 15sec */
    .auto_pwr_off       = 0,        /* OFF */
    .tot                = 8,        /* 240s */
    .beep               = 1,        /* ON */
    .voice_prompt       = 0,        /* OFF */
    .language           = SETTINGS_LANGUAGE_ENGLISH,
    .dtmf_st            = 0,        /* OFF */
    .scan_mode          = 1,        /* CO (Carrier) */
    .ptt_id             = 0,        /* OFF */
    .send_id_delay      = 7,        /* 800ms */
    .dual_watch         = 0,        /* OFF */

    .display_mode_a     = 0,        /* Frequency */
    .display_mode_b     = 0,        /* Frequency */
    .display_mode_c     = 0,        /* Frequency */
    .auto_keypad_lock   = 0,        /* OFF */
    .keypad_lock        = 0,        /* OFF */
    .sos_mode           = 0,        /* On Site */
    .alarm_sound        = 1,        /* ON */
    .tail_elim          = 1,        /* ON (Tail) */
    .rpste              = 0,        /* OFF */
    .rpt_rl             = 0,        /* OFF */
    .tx_tail_sound      = 0,        /* OFF */
    .fm_backlight       = 1,        /* ON */
    .fm_radio           = 1,        /* ON */
    .work_mode_a        = 0,        /* VFO Mode */
    .work_mode_b        = 1,        /* CH Mode */
    .work_mode_c        = 0,        /* VFO Mode */

    .power_msg          = 0,        /* Preset Icons */
    .bt_write_switch    = 1,        /* ON */
    .r_tone             = 3,        /* 1750Hz (index 3) */
    .menu_out_time      = 3,        /* 20sec */
    .breathing_light    = 0,        /* OFF (default during development) */
    .noaa_alarm         = 1,        /* ON */
    .subaudio_scansave  = 0,        /* ALL */
    .pf1_short          = 0,        /* PTTC */
    .pf1_long           = 0,        /* Spectrum */
    .pf2_short          = 0,        /* Radio */
    .pf2_long           = 0,        /* Moni */
    .cur_zone_a         = 1,
    .cur_zone_b         = 1,
    .cur_zone_c         = 1,
    .fm_rx_interrupted  = 0,        /* OFF */

    .ab_uv_transfer     = 0,        /* OFF */
    .sound_transfer     = 0,        /* OFF */
    .key_long           = {0},      /* All default to first function */
    .aprs_enable        = 0,        /* OFF */
    ._reserved          = {0},
};

/* Public API ---------------------------------------------------------- */

void settings_init(void)
{
    if (wl_read(&WL_SYSCFG, &settings) != 0) {
        /* No valid record in flash - use defaults */
        settings = settings_defaults;
        wl_write(&WL_SYSCFG, &settings);
        return;
    }

    /* Sanity check: if SPI flash is dead or erased, all bytes read as 0xFF.
     * Detect this by checking a few fields for impossible values. */
    if (settings.sql_level == 0xFF && settings.beep == 0xFF) {
        settings = settings_defaults;
    }
}

const settings_t *settings_get(void)
{
    return &settings;
}

void settings_save(void)
{
    wl_write(&WL_SYSCFG, &settings);
}

void settings_set_u8(uint16_t offset, uint8_t value)
{
    if (offset >= sizeof(settings_t))
        return;

    uint8_t *base = (uint8_t *)&settings;
    base[offset] = value;
    settings_save();
}

void settings_reload(void)
{
    if (wl_read(&WL_SYSCFG, &settings) != 0)
        settings = settings_defaults;
}

void settings_factory_reset(void)
{
    settings = settings_defaults;
    settings_save();
}
