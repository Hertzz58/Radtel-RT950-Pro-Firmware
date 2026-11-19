/*
 * settings.h - Persistent system settings for RT-950 Pro
 *
 * Maps to the SYSCFG wear-leveled flash sector (WL_SYSCFG at 0x010000).
 * OEM record: 128 bytes per slot, 119 bytes payload.
 *
 * Settings are loaded from flash at boot and saved back when changed.
 * The struct layout is our own definition - the OEM byte map is not
 * fully verified, but the WL engine handles read/write transparently.
 *
 * CPS-accessible settings at 0x009000 are a SEPARATE copy that the CPS
 * tool reads/writes directly. On CPS session end, we reload from flash.
 */

#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdint.h>

/* Keep the legacy Chinese value (1) reserved in saved settings. */
enum {
    SETTINGS_LANGUAGE_ENGLISH = 0,
    SETTINGS_LANGUAGE_TURKISH = 2,
    SETTINGS_LANGUAGE_RUSSIAN = 3,
};

/*
 * System settings struct - fields from CPS S_3941 / V0.27 analysis.
 * Padded to SYSCFG record size (128 bytes) for direct WL read/write.
 */
typedef struct __attribute__((packed)) {
    /* Block 0: Core radio settings */
    uint8_t  sql_level;         /* 0-9 (default 3) */
    uint8_t  battery_save;      /* 0=OFF, 1=ON */
    uint8_t  vox_level;         /* 0=OFF, 1-9 */
    uint8_t  vox_switch;        /* 0=OFF, 1=ON (master enable) */
    uint8_t  vox_delay;         /* 0=0.5s, 1=1.0s, 2=1.5s, 3=2.0s, ... */
    uint8_t  backlight;         /* 0=OFF, 1=ON, 2=5s, 3=10s, 4=15s, 5=30s */
    uint8_t  auto_pwr_off;      /* 0=OFF, 1=30m, 2=1h, 3=2h */
    uint8_t  tot;               /* TX timeout: 0=OFF, 1=30s, 2=60s, ... 8=240s */
    uint8_t  beep;              /* 0=OFF, 1=ON */
    uint8_t  voice_prompt;      /* 0=OFF, 1=ON */
    uint8_t  language;          /* SETTINGS_LANGUAGE_* */
    uint8_t  dtmf_st;           /* 0=OFF, 1=ON */
    uint8_t  scan_mode;         /* 0=TO(timeout), 1=CO(carrier), 2=SE(search) */
    uint8_t  ptt_id;            /* 0=OFF, 1=BOT, 2=EOT, 3=BOTH */
    uint8_t  send_id_delay;     /* 0=100ms, 1=200ms, ..., 7=800ms */
    uint8_t  dual_watch;        /* 0=OFF, 1=ON */

    /* Block 1: Display and lock settings */
    uint8_t  display_mode_a;    /* 0=Frequency, 1=Channel, 2=Name */
    uint8_t  display_mode_b;    /* 0=Frequency, 1=Channel, 2=Name */
    uint8_t  display_mode_c;    /* 0=Frequency, 1=Channel, 2=Name */
    uint8_t  auto_keypad_lock;  /* 0=OFF, 1=ON */
    uint8_t  keypad_lock;       /* 0=OFF, 1=ON */
    uint8_t  sos_mode;          /* 0=ON_SITE, 1=SEND, 2=BOTH */
    uint8_t  alarm_sound;       /* 0=OFF, 1=ON */
    uint8_t  tail_elim;         /* STE: 0=OFF, 1=ON */
    uint8_t  rpste;             /* 0=OFF, 1=ON */
    uint8_t  rpt_rl;            /* 0=OFF, 1=ON */
    uint8_t  tx_tail_sound;     /* 0=OFF, 1=ON */
    uint8_t  fm_backlight;      /* 0=OFF, 1=ON */
    uint8_t  fm_radio;          /* 0=OFF, 1=ON */
    uint8_t  work_mode_a;       /* 0=VFO, 1=CH */
    uint8_t  work_mode_b;       /* 0=VFO, 1=CH */
    uint8_t  work_mode_c;       /* 0=VFO, 1=CH */

    /* Block 2: Keys and zones */
    uint8_t  power_msg;         /* 0=Preset_Icons, 1=Full_Screen, 2=Voltage */
    uint8_t  bt_write_switch;   /* 0=OFF, 1=ON */
    uint8_t  r_tone;            /* 0=OFF, 1=1000Hz, 2=1450Hz, 3=1750Hz, 4=2100Hz */
    uint8_t  menu_out_time;     /* 0=5s, 1=10s, 2=15s, 3=20s, 4=30s */
    uint8_t  breathing_light;   /* 0=OFF, 1=ON */
    uint8_t  noaa_alarm;        /* 0=OFF, 1=ON */
    uint8_t  subaudio_scansave; /* 0=ALL, 1=RX, 2=TX */
    uint8_t  pf1_short;         /* Function code for PF1 short press */
    uint8_t  pf1_long;          /* Function code for PF1 long press */
    uint8_t  pf2_short;         /* Function code for PF2 short press */
    uint8_t  pf2_long;          /* Function code for PF2 long press */
    uint8_t  cur_zone_a;        /* Current work zone A (1-10) */
    uint8_t  cur_zone_b;        /* Current work zone B (1-10) */
    uint8_t  cur_zone_c;        /* Current work zone C (1-10) */
    uint8_t  fm_rx_interrupted; /* 0=OFF, 1=ON */

    /* Block 3: Transfer and numkey mapping */
    uint8_t  ab_uv_transfer;    /* 0=OFF, 1=ON */
    uint8_t  sound_transfer;    /* 0=OFF, 1=ON */
    uint8_t  key_long[10];      /* Key [0]-[9] long-press function codes */

    /* Block 4: APRS basic (rest in separate APRS config) */
    uint8_t  aprs_enable;       /* 0=OFF, 1=ON */
    uint8_t  crossband_mode;    /* 0=OFF, 1=A->B, 2=B->A, 3=Duplex */

    /* Padding to 128 bytes for WL record alignment */
    uint8_t  _reserved[67];
} settings_t;

_Static_assert(sizeof(settings_t) == 128, "settings_t must be 128 bytes");

/* Default settings (factory reset values from CPS S_3941) */
extern const settings_t settings_defaults;

/* Initialize settings subsystem (call after WL init, before menu/radio) */
void settings_init(void);

/* Get pointer to the live settings struct (read-only outside settings.c) */
const settings_t *settings_get(void);

/* Save current settings to flash (WL_SYSCFG). Call after any modification. */
void settings_save(void);

/*
 * Modify a setting. Updates the in-RAM copy and saves to flash.
 * offset: offsetof(settings_t, field)
 * value:  new value (uint8_t)
 */
void settings_set_u8(uint16_t offset, uint8_t value);

/* Reload settings from flash (e.g., after CPS session) */
void settings_reload(void);

/* Reset all settings to factory defaults and save */
void settings_factory_reset(void);

#endif /* APP_SETTINGS_H */
