// Philosophy:
//
// NVS provides values for UI settings like timezone, WiFi SSID, 12 vs
// 24hr time format, etc. When NVS values are used to set these in the
// UI, it's the `LV_EVENT_VALUE_CHANGED` callbacks that actually make
// the clock or WiFi or whatever change. This makes it possible for
// there to be a single place where each value's setting is
// implemented and takes into account the first time and every time it
// changes afterward "for free".
//
// TODO:
// 

#include <stdio.h>
#include <time.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"

#include "lvgl.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"


static const char *TAG = "wallclock";

// THIS COULD BE HELPFUL:
// https://components.espressif.com/components/espressif/esp_lvgl_port
// https://github.com/pjaos/mgos_esp32_littlevgl_wifi_setup/tree/master


// XXX remove this after debugging
static const char *evToName[] = {
  "EVENT_ALL",

  /** Input device events*/
  "EVENT_PRESSED",
  "EVENT_PRESSING",
  "EVENT_PRESS_LOST",
  "EVENT_SHORT_CLICKED",
  "EVENT_LONG_PRESSED",
  "EVENT_LONG_PRESSED_REPEAT",
  "EVENT_CLICKED",
  "EVENT_RELEASED",
  "EVENT_SCROLL_BEGIN",
  "EVENT_SCROLL_END",
  "EVENT_SCROLL",
  "EVENT_GESTURE",
  "EVENT_KEY",
  "EVENT_FOCUSED",
  "EVENT_DEFOCUSED",
  "EVENT_LEAVE",
  "EVENT_HIT_TEST",

  /** Drawing events*/
  "EVENT_COVER_CHECK",
  "EVENT_REFR_EXT_DRAW_SIZE",
  "EVENT_DRAW_MAIN_BEGIN",
  "EVENT_DRAW_MAIN",
  "EVENT_DRAW_MAIN_END",
  "EVENT_DRAW_POST_BEGIN",
  "EVENT_DRAW_POST",
  "EVENT_DRAW_POST_END",
  "EVENT_DRAW_PART_BEGIN",
  "EVENT_DRAW_PART_END",

  /** Special events*/
  "EVENT_VALUE_CHANGED",
  "EVENT_INSERT",
  "EVENT_REFRESH",
  "EVENT_READY",
  "EVENT_CANCEL",

  /** Other events*/
  "LV_EVENT_DELETE",
  "EVENT_CHILD_CHANGED",
  "EVENT_CHILD_CREATED",
  "EVENT_CHILD_DELETED",
  "EVENT_SCREEN_UNLOAD_START",
  "EVENT_SCREEN_LOAD_START",
  "EVENT_SCREEN_LOADED",
  "EVENT_SCREEN_UNLOADED",
  "EVENT_SIZE_CHANGED",
  "EVENT_STYLE_CHANGED",
  "EVENT_LAYOUT_CHANGED",
  "EVENT_GET_SELF_SIZE",
};



#define HRESOLUTION 800
#define VRESOLUTION 480

#define LVGL_TICK_PERIOD_MS 10

static lv_disp_draw_buf_t dispBuf;
static lv_disp_drv_t dispDrv;
static esp_lcd_panel_handle_t panelH;
static esp_lcd_touch_handle_t touchH;
static lv_indev_t *touchIndevP;

static int hours = 0;
static int minutes = 0;
static int seconds = 0;

// Our icons created with https://lvgl.io/tools/imageconverter
extern const lv_img_dsc_t visible;
extern const lv_img_dsc_t invisible;
extern const lv_img_dsc_t cog;


static struct {
  lv_style_t base;
  lv_style_t icon;
  lv_style_t widget;
  lv_style_t time;
  lv_style_t selectedItem;
  lv_style_t pageHeader;
} styles;


static lv_disp_t *disp;
static lv_obj_t *keyboard;
extern lv_font_t LoraBold240;
extern lv_font_t RobotoMedium40;


// The display has one LVGL "screen" loaded at a time, and this is
// used to set the UI mode for wallclock (`timeUI`), settings
// (`settingsUI`), WiFi SSID scanner (`wifiUI`), WiFi SSID password
// entry (`passwordUI`), etc. The default at startup time is to load
// the `timeUI` screen.
//
// Each UI has a `screen` that contains its UI elements. The struct
// for each UI contains only the things I have to dink around with
// after creation time.  I don't bother putting things like static
// headings in this since they never need to be accessed after they're
// initially configured properly.
static struct {
  lv_obj_t *screen;
  lv_obj_t *settingsButton;
  lv_obj_t *time;
  lv_obj_t *seconds;
  lv_obj_t *ampm;
  lv_obj_t *tz;
  lv_obj_t *date;
  lv_obj_t *apName;
  lv_obj_t *ntpName;
} timeUI;


#define N_NTP_SERVERS	3

static struct {
  lv_obj_t *screen;
  lv_obj_t *twelveHr;		/* Checkbox */
  lv_obj_t *showSeconds;	/* Checkbox */
  lv_obj_t *showDayDate;	/* Checkbox */
  lv_obj_t *ntp[N_NTP_SERVERS];	/* Each is a textarea */
  lv_obj_t *tzString;		/* Timezone in TZ envar format (e.g., PST-08 for US Pacific) */
  lv_obj_t *ok;			/* Button */
  lv_obj_t *cancel;		/* Button */
} settingsUI;


static struct {
  lv_obj_t *screen;
  lv_obj_t *list;
  lv_obj_t *ok;
  lv_obj_t *cancel;
  lv_obj_t *wifiList;		/* button matrix */
} wifiUI;


static struct {
  lv_obj_t *screen;
  lv_obj_t *pass;		/* textarea userdata=keyboard when focused */
  lv_obj_t *visible;
  lv_obj_t *ok;
  lv_obj_t *cancel;
} passwordUI;


// The settings as they are currently set. These come from NVS
// initially, are modified by the settingsUI, wifiUI, and passwordUI,
// and are saved back to NVS when changed.
static struct {
  uint8_t twelveHr;	  /* Set if 12hr is needed (24hr otherwise) */
  uint8_t showSeconds;	  /* Display and update seconds */
  uint8_t showDayDate;	  /* Display day of week and date */
  char *tz;		  /* String for TZ envar */
  char *ntp1;		  /* String for primary NTP site */
  char *ntp2;		  /* String for secondary NTP site */
  char *ntp3;		  /* String for tertiary NTP site */
  char *ssid;		  /* String for current selected WiFi SSID */
  char *password;	  /* String for current selected Wifi SSID's password */
} settings = {
  .twelveHr = 0,
  .showSeconds = 1,
  .showDayDate = 1,
  .tz = "PST",
  .ntp1 = "pool.ntp.org",
  .ntp2 = "time.google.com",
  .ntp3 = "clock.uregina.ca",
  .ssid = "My Sooper Seekyure WiFi",
  .password = "s00per-skeQret",
};


// Use two semaphores to sync the VSYNC event and the LVGL task, to
// avoid potential tearing effect.
static SemaphoreHandle_t semVsyncEnd;
static SemaphoreHandle_t semGuiReady;


static void lvglTickCB(void *) {
  /* Tell LVGL how many milliseconds has elapsed */
  lv_tick_inc(LVGL_TICK_PERIOD_MS);
}


// Timer callback for updating the displayed time and date every period.
//
// XXX Does this get called with lvgl_port_lock already held or do I
// need to protect here or will that hang or what?
//
// https://components.espressif.com/components/espressif/esp_lvgl_port
//
// lvgl_port_lock(0);
// ...
// lvgl_port_unlock();
static void secondsCB(lv_timer_t *timerP) {

  if (++seconds > 59) {
    seconds = 0;

    if (++minutes > 59) {
      minutes = 0;

      if (++hours > 23) hours = 0;
    }
  }

#if 0
  time_t now;
  struct tm tm;
  time(&now);
  tzset();
  localtime_r(&now, &tm);
  
  char buf[64];
  int st;
  char *timeFmtP = settings.twelveHr ? "%I:%M%p" : "%R";

  st = strftime(buf, sizeof(buf)-1, timeFmtP, &tod);
  lv_label_set_text(timeUI.time, buf);
  lv_label_set_text_fmt(timeUI.seconds, "%02d", seconds);

  int st = strftime(buf, sizeof(buf)-1, "%A %B %d, %Y");
  lv_label_set_text(timeUI.date, date);
#endif
}


static bool vsyncCB(esp_lcd_panel_handle_t panel,
		    const esp_lcd_rgb_panel_event_data_t *event_data,
		    void *user_data)
{
  BaseType_t high_task_awoken = pdFALSE;

  if (xSemaphoreTakeFromISR(semGuiReady, &high_task_awoken) == pdTRUE) {
    xSemaphoreGiveFromISR(semVsyncEnd, &high_task_awoken);
  }

  return high_task_awoken == pdTRUE;
}


static void flushCB(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colorMap) {
  int ox1 = area->x1;
  int ox2 = area->x2;
  int oy1 = area->y1;
  int oy2 = area->y2;

  xSemaphoreGive(semGuiReady);
  xSemaphoreTake(semVsyncEnd, portMAX_DELAY);

  // pass the draw buffer to the driver
  esp_lcd_panel_draw_bitmap(panelH, ox1, oy1, ox2 + 1, oy2 + 1, colorMap);
  lv_disp_flush_ready(drv);
}


static void cogClickedCB(lv_event_t *ev) {
  ESP_LOGI(TAG, "cog clicked event code=%d (%s)", ev->code, evToName[ev->code]);
  if (ev->code == LV_EVENT_CLICKED) lv_scr_load(settingsUI.screen);
}


static void setupClockUI(void) {
  // For each block that configures an LVGL object, use this pointer
  // to avoid copypasta errors.
  lv_obj_t *p;

  ESP_LOGI(TAG, "[set up clock UI]");

  p = timeUI.screen = lv_obj_create(NULL);
  lv_obj_add_style(p, &styles.base, LV_PART_MAIN);

  // Set up the default style for the large-ish text. Most of the
  // objects on this screen use this.
  lv_obj_set_style_text_font(p, &RobotoMedium40, LV_PART_MAIN);

  // Left edge of hh:mm
  static const int timeL = 32;
  // Top edge of hh:mm
  static const int timeT = 32;

  // Padding below hh:mm:ss before top fo timezone
  static const int timeBottomPad = 10;

  // The hours:minutes
  p = timeUI.time = lv_label_create(timeUI.screen);
  lv_label_set_text_static(p, "00:00");
  lv_obj_set_style_text_font(p, &LoraBold240, LV_PART_MAIN);
  lv_obj_set_pos(p, timeL, timeT);

  // The AM/PM indicator
  if (settings.twelveHr || 1) {
    p = timeUI.ampm = lv_label_create(timeUI.screen);
    lv_label_set_text_static(p, "AM");
    lv_obj_set_align(p, LV_ALIGN_TOP_LEFT);
    lv_obj_align_to(p, timeUI.time, LV_ALIGN_TOP_RIGHT, 48, 36);
  }

  // The seconds
  p = timeUI.seconds = lv_label_create(timeUI.screen);
  lv_label_set_text_static(p, "00");
  lv_obj_set_align(p, LV_ALIGN_BOTTOM_LEFT);
  lv_obj_align_to(p, timeUI.time, LV_ALIGN_BOTTOM_RIGHT, 48, -50);

  // The timezone
  p = timeUI.tz = lv_label_create(timeUI.screen);
  lv_label_set_text_static(p, "PST");
  lv_obj_set_align(p, LV_ALIGN_TOP_RIGHT);
  lv_obj_align_to(p, timeUI.seconds, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, timeBottomPad);

  // The day/date
  p = timeUI.date = lv_label_create(timeUI.screen);
  lv_label_set_text_static(p, "Blurday Franuary 33, 1999");
  lv_obj_set_align(p, LV_ALIGN_TOP_RIGHT);
  lv_obj_align_to(p, timeUI.tz, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 0);

  // The WiFi SSID in status bar
  p = timeUI.apName = lv_label_create(timeUI.screen);
  lv_obj_set_align(p, LV_ALIGN_BOTTOM_LEFT);
  lv_label_set_text_static(p, settings.ssid);
  lv_obj_set_width(p, LV_SIZE_CONTENT);

  // The NTP source site in status bar
  p = timeUI.ntpName = lv_label_create(timeUI.screen);
  lv_obj_set_align(p, LV_ALIGN_BOTTOM_RIGHT);
  lv_label_set_text_static(p, settings.ntp1);
  lv_obj_set_width(p, LV_SIZE_CONTENT);

  // The settings (cog) button
  p = timeUI.settingsButton = lv_btn_create(timeUI.screen);
  lv_obj_align_to(p, timeUI.screen, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_size(p, 32, 32);
  lv_obj_add_event_cb(p, cogClickedCB, LV_EVENT_CLICKED, NULL);

  // The settings (cog) button's image
  lv_obj_t *cogImg = lv_img_create(p);
  lv_img_set_src(cogImg, &cog);
  lv_obj_add_style(cogImg, &styles.icon, LV_PART_MAIN);
  lv_obj_center(cogImg);

  lv_obj_update_layout(timeUI.screen);

  lv_scr_load(timeUI.screen);
  ESP_LOGI(TAG, "after update layout/scr load cog pos=(%d %d)  size=(%d %d)",
	   (int) lv_obj_get_x(p), (int) lv_obj_get_y(p),
	   (int) lv_obj_get_width(p), (int) lv_obj_get_height(p));

  lv_timer_create(secondsCB, 1000, NULL);
}


static void setupPasswordUI(void) {
  (void) &passwordUI;		/* Don't warn me */
}


static void setupWifiUI(void) {
  (void) &wifiUI;		/* Don't warn me */
}


#if 0
static void networkScanner(void) {
}
#endif


// Define each style by initializing its static variable as a new
// style and calling each of the SETTINGS style setting functions with
// their respective parameters.
static void setupStyles(void) {

#define INIT(STYLE)			lv_style_init(&styles.STYLE)
#define SET(STYLE,ATTR,PARMS...)	lv_style_set_##ATTR(&styles.STYLE, PARMS)

  INIT(base);
  SET(base, bg_color, lv_color_hex(0x222244));
  SET(base, text_color, lv_color_hex(0xAAAAAA));

  INIT(icon);
  SET(icon, border_color, lv_color_hex(0x5555AA));
  SET(icon, border_width, 2);
  SET(icon, pad_all, 16);

  INIT(widget);
  SET(widget, text_font, &lv_font_montserrat_24);

  INIT(pageHeader);
  SET(pageHeader, bg_color, lv_color_hex(0xFCFCAA));
  SET(pageHeader, bg_opa, LV_OPA_COVER);
  SET(pageHeader, text_color, lv_color_black());
  SET(pageHeader, text_font, &RobotoMedium40);

#define STYLES								\
  DO1(statusStyle,							\
      SET1(text_font, &lv_font_montserrat_10))				\
  DO1(buttonStyle,							\
      SET1(border_color, lv_palette_main(LV_PALETTE_GREY)))		\
  DO1(okButtonStyle,							\
      SET1(border_color, lv_palette_main(LV_PALETTE_GREEN)))		\
  DO1(cancelButtonStyle,						\
      SET1(border_color, lv_palette_main(LV_PALETTE_ORANGE)))		\
  DO1(popupStyle,							\
      SET1(bg_color, lv_palette_main(LV_PALETTE_ORANGE)))

// First, declare the static variables for our styles
#define DO1(S, SETTINGS...)	static lv_style_t S;
    STYLES
#undef DO1


#define SET1(F, ARGS...)	lv_style_set_ ## F(s, ARGS);

#define DO1(S, SETTINGS...)			\
  {						\
    lv_style_t *s = &S;				\
    lv_style_init(s);				\
    SETTINGS					\
  }

  STYLES
#undef SET1
#undef DO1

#undef SET
#undef INIT
}


static void setupKeyboard(void) {
  keyboard = lv_keyboard_create(lv_scr_act());
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}


static lv_obj_t *makeTextBox(lv_obj_t *parent, const char *prompt, int boxWidth) {
  lv_obj_t *group = lv_obj_create(parent);
  lv_obj_t *p = lv_label_create(group);
  lv_label_set_text(p, prompt);
  lv_obj_t *box = lv_textarea_create(group);
  lv_textarea_set_text(box, "");
  lv_textarea_set_one_line(box, true);
  lv_obj_set_width(box, boxWidth);
  return group;
}


static void setupSettingsUI(void) {
  lv_obj_t *p;

  settingsUI.screen = lv_obj_create(NULL);
  lv_obj_add_style(settingsUI.screen, &styles.base, LV_PART_MAIN);

  p = lv_label_create(settingsUI.screen);
  lv_label_set_text(p, "Settings");
  lv_obj_add_style(p, &styles.pageHeader, LV_PART_MAIN);
  lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 25);
  lv_obj_set_width(p, lv_pct(80));

  p = settingsUI.twelveHr = lv_checkbox_create(settingsUI.screen);
  lv_checkbox_set_text(p, "12-hour time format");
  lv_obj_align(p, LV_ALIGN_TOP_LEFT, 20, 175);

  p = settingsUI.showSeconds = lv_checkbox_create(settingsUI.screen);
  lv_checkbox_set_text(p, "Show seconds");
  lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, 0, 10);
  lv_obj_align_to(p, settingsUI.twelveHr, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

  p = settingsUI.showDayDate = lv_checkbox_create(settingsUI.screen);
  lv_checkbox_set_text(p, "Show day/date");
  lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, 0, 10);
  lv_obj_align_to(p, settingsUI.showDayDate, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

  p = settingsUI.ntp[0] = makeTextBox(settingsUI.screen, "NTP #1:", 220);
  lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_align_to(p, settingsUI.twelveHr, LV_ALIGN_BOTTOM_LEFT, 400, 0);

  p = settingsUI.ntp[1] = makeTextBox(settingsUI.screen, "NTP #2:", 220);
  lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_align_to(p, settingsUI.ntp[0], LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

  p = settingsUI.ntp[2] = makeTextBox(settingsUI.screen, "NTP #3:", 220);
  lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_align_to(p, settingsUI.ntp[1], LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

  // Buttons are in a horizontal row aligned a little above the bottom
  // of the screen with their edges (left for OK, right for Cancel)
  // the same distance from the center of the screen in opposite
  // directions.
  p = settingsUI.ok = lv_btn_create(settingsUI.screen);
  lv_label_set_text(p, "OK");
  lv_obj_set_width(p, 150);
  lv_obj_set_align(p, LV_ALIGN_BOTTOM_LEFT);
  lv_obj_align_to(p, settingsUI.screen, LV_ALIGN_OUT_BOTTOM_MID, -250, -25);

  p = settingsUI.cancel = lv_btn_create(settingsUI.screen);
  lv_label_set_text(p, "Cancel");
  lv_obj_set_width(p, 150);
  lv_obj_set_align(p, LV_ALIGN_BOTTOM_RIGHT);
  lv_obj_align_to(p, settingsUI.screen, LV_ALIGN_OUT_BOTTOM_MID, 250, -25);

  lv_obj_update_layout(settingsUI.screen);
}


static void setupUI(void) {
  setupStyles();
  setupClockUI();
  setupKeyboard();
  setupPasswordUI();
  setupWifiUI();
  setupSettingsUI();
}


static void setupBacklightPWM(void) {
  ESP_LOGI(TAG, "[configure LCD backlight PWM]");
  static const ledc_timer_config_t backlightTimer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .timer_num = LEDC_TIMER_0,
    .duty_resolution = LEDC_TIMER_13_BIT,
    .freq_hz = 5000,
    .clk_cfg = LEDC_AUTO_CLK,
  };
  
  ESP_ERROR_CHECK(ledc_timer_config(&backlightTimer));

  // LEDC PWM channel configuration
  static const ledc_channel_config_t backlightChannel = {
    .speed_mode     = LEDC_LOW_SPEED_MODE,
    .channel        = LEDC_CHANNEL_0,
    .timer_sel      = LEDC_TIMER_0,
    .intr_type      = LEDC_INTR_DISABLE,
    .gpio_num       = GPIO_NUM_2,
    .duty           = 10 * 8192 / 100, /* duty cycle in % as < full count 8192 */
    .hpoint         = 0,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&backlightChannel));
}


static void setupLCD(void) {

  static const esp_lcd_rgb_panel_config_t panelConfig = {
    .data_width = 16, // RGB565 in parallel mode, thus 16bit in width
    .psram_trans_align = 64,
    .num_fbs = 2,
    .clk_src = LCD_CLK_SRC_DEFAULT,
    .disp_gpio_num = -1,
    .pclk_gpio_num = 0,
    .vsync_gpio_num = 40,
    .hsync_gpio_num = 39,
    .de_gpio_num = 41,
    .data_gpio_nums = {
      15,			/* B0 */
      7,			/* B1 */
      6,			/* B2 */
      5,			/* B3 */
      4,			/* B4 */
      9,			/* G0 */
      46,			/* G1 */
      3,			/* G2 */
      8,			/* G3 */
      16,			/* G4 */
      1,			/* G5 */
      14,			/* R0 */
      21,			/* R1 */
      47,			/* R2 */
      48,			/* R3 */
      45,			/* R4 */
    },
    // The timing parameters should refer to your LCD spec
    .timings = {
      .pclk_hz = 16 * 1000 * 1000,
      .h_res = HRESOLUTION,
      .v_res = VRESOLUTION,
      .hsync_back_porch = 40,
      .hsync_front_porch = 40,
      .hsync_pulse_width = 48,
      .vsync_back_porch = 31,
      .vsync_front_porch = 13,
      .vsync_pulse_width = 1,
      .flags = {
	.pclk_active_neg = true,
      },
    },
    .flags = {
      .fb_in_psram = true,
    },
  };

  ESP_LOGI(TAG, "[initialize LCD]");
  semVsyncEnd = xSemaphoreCreateBinary();
  assert(semVsyncEnd);
  semGuiReady = xSemaphoreCreateBinary();
  assert(semGuiReady);

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panelConfig, &panelH));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panelH));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panelH));

  esp_lcd_rgb_panel_event_callbacks_t cbs = {
    .on_vsync = vsyncCB,
  };

  static lv_disp_drv_t dispDriver;      // contains callback functions
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panelH, &cbs, &dispDriver));

  lv_init();
  void *buf1 = NULL;
  void *buf2 = NULL;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panelH, 2, &buf1, &buf2));
  lv_disp_draw_buf_init(&dispBuf, buf1, buf2, HRESOLUTION * VRESOLUTION);

  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = HRESOLUTION;
  dispDrv.ver_res = VRESOLUTION;
  dispDrv.flush_cb = flushCB;
  dispDrv.draw_buf = &dispBuf;
  dispDrv.user_data = panelH;

  // The full_refresh mode can maintain the synchronization between
  // the two frame buffers.
  dispDrv.full_refresh = true;

  disp = lv_disp_drv_register(&dispDrv);

  static const esp_timer_create_args_t timerArgs = {
    .callback = &lvglTickCB,
    .name = "lvglTickCB",
  };
  esp_timer_handle_t tickTimer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&timerArgs, &tickTimer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tickTimer, LVGL_TICK_PERIOD_MS * 1000));
}


static void setupTouch(void) {
  ESP_LOGI(TAG, "[configure LCD touch]");
  esp_lcd_panel_io_handle_t touchIOH;
  static const esp_lcd_panel_io_i2c_config_t touchI2CConfig = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

  static const esp_lcd_touch_config_t touchESPConfig = {
    .x_max = HRESOLUTION,
    .y_max = VRESOLUTION,
    .rst_gpio_num = -1,
    .int_gpio_num = GPIO_NUM_38,
    .levels = {
      .reset = 0,
      .interrupt = 0,
    },
    .flags = {
      .swap_xy = 0,
      .mirror_x = 0,
      .mirror_y = 0,
    },
  };

  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t) I2C_NUM_1, &touchI2CConfig, &touchIOH));
  ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(touchIOH, &touchESPConfig, &touchH));

  const lvgl_port_touch_cfg_t touchLVGLConfig = {
    .disp = disp,
    .handle = touchH,
  };

  touchIndevP = lvgl_port_add_touch(&touchLVGLConfig);
}


static void printChipInfo(void) {
  /* Print chip information */
  esp_chip_info_t chip_info;
  uint32_t flash_size;

  esp_chip_info(&chip_info);
  unsigned major_rev = chip_info.revision / 100;
  unsigned minor_rev = chip_info.revision % 100;

  ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

  ESP_LOGI(TAG, "[%s chip with %d CPU core(s), %s%s%s%s, silicon rev v%d.%d, %"PRIu32 "MB %s flash]",
	   CONFIG_IDF_TARGET,
	   chip_info.cores,
	   (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
	   (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
	   (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
	   (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "",
	   major_rev, minor_rev,
	   flash_size / (uint32_t)(1024 * 1024),
	   (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
  ESP_LOGI(TAG, "[Minimum free heap size: %" PRIu32 " bytes]", esp_get_minimum_free_heap_size());
}


static void setupI2C(void) {

  static const i2c_config_t i2c_conf = {
    .mode = I2C_MODE_MASTER,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_io_num = GPIO_NUM_20,
    .sda_io_num = GPIO_NUM_19,
    .master.clk_speed = 400 * 1000,
  };

  ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_1, &i2c_conf));
  ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_1, i2c_conf.mode, 0, 0, 0));
}


// Set up the network. If network isn't configured or connection to
// WiFi or NTP server pool fails, bring up a GUI to configure the WiFi
// and NTP server pool. Once WiFi connects properly, it's a short time
// after that we get NTP result, updating the time display.
static void setupNetwork(void) {
}


#if 0
static void settingsButtonEventCB(lv_event_t *e) {

  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    lv_obj_clear_flag(settingsUI.screen, LV_OBJ_FLAG_HIDDEN);
  }
}
#endif


#if 0
static void settingsCloseButtonEventCB(lv_event_t *e) {

  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    lv_obj_add_flag(settingsUI.screen, LV_OBJ_FLAG_HIDDEN);
  }
}
#endif


#if 0
static void mboxConnectButtonEventCB(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    ssidPW = String(lv_textarea_get_text(mboxPassword));
    networkConnector();
    lv_obj_move_background(mboxConnect);
    popupMsgBox("Connecting!", "Attempting to connect to the selected network.");
  }
}
#endif


#if 0
static void buttonEventCB(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    if (btn == mboxConnectBtn) {
      ssidPW = String(lv_textarea_get_text(mboxPassword));

      networkConnector();
      lv_obj_move_background(mboxConnect);
      popupMsgBox("Connecting!", "Attempting to connect to the selected network.");
    } else if (btn == mboxCloseBtn) {
      lv_obj_move_background(mboxConnect);
    } else if (btn == popupBoxCloseBtn) {
      lv_obj_move_background(popupBox);
    }

  } else if (code == LV_EVENT_VALUE_CHANGED) {

    if (ntScanTaskHandler == NULL) {
      networkStatus = NETWORK_SEARCHING;
      networkScanner();
      timer = lv_timer_create(timerForNetwork, 1000, wfList);
      lv_list_add_text(wfList, "WiFi: Looking for Networks...");
    }
  }
}
#endif


// Read our NVS variables
static void setupNVS(void) {
  // Initialize NVS subsystem to use default partition.
  ESP_ERROR_CHECK(nvs_flash_init());

  // Open our NVS namespace.
  nvs_handle_t wifiNVSH;
  ESP_ERROR_CHECK(nvs_open("WiFi", NVS_READONLY, &wifiNVSH));

  nvs_stats_t stats;
  ESP_ERROR_CHECK(nvs_get_stats(NULL, &stats));
  ESP_LOGI(TAG, "NVS: used=%d  free=%d  total=%d  nscount=%d",
	   stats.used_entries, stats.free_entries, stats.total_entries, stats.namespace_count);

  // In the `WiFi` namespace, read the list of WiFi APs and
  // credentials and NTP pool names.  Each entry is with key as a
  // four digit decimal number like `0001` whose value consists of a
  // single string with fields separated by 0xFF bytes and
  // containing the AP name, the password, and the optional NTP pool
  // semicolon separated list.  These are tried in order from the
  // lowest numbered to the highest. The entry with key `0000` is
  // the default. If NTP pool is not present in an entry, we use the
  // value from the `0000` default or `pool.ntp.org` if none
  // exists. The key decimal values need not be contiguous.
  //
  // For example (using [FF] for the 0xFF separator bytes):
  // 0000=Apple Corp[FF]SuperSecret[FF]pool.ntp.org
  // 0001=SecretFBIvan[FF]VeryySeqret![FF]pool.kernel.org
  // 0037=MyOpenWifi[FF]OpenWide
  nvs_iterator_t it = NULL;
  esp_err_t st = nvs_entry_find_in_handle(wifiNVSH, NVS_TYPE_ANY, &it);

  while (st == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    ESP_LOGI(TAG, "NVS: '%s' type %d", info.key, info.type);
    st = nvs_entry_next(&it);
  }

  if (it != NULL) nvs_release_iterator(it);

  // XXX TODO place the entries into a sorted list and return it.
}


static void setupSNTP(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
}


static void getSettings(void) {
  setenv("TZ", settings.tz, 1);
}


static void enableClockUI(void) {
}


void app_main(void) {
  printChipInfo();

  setupI2C();
  setupLCD();
  setupBacklightPWM();
  setupTouch();

  setupNVS();
  setupSNTP();

  setupNetwork();
  setupUI();		 /* Setup but don't actually display UI yet */

  // These two MUST be last because the LV_EVENT_VALUE_CHANGED event
  // callbacks set everything up according to saved NVS values or UI
  // changes and the UI needs to not be displayed until after NVS
  // values are used to set those.
  getSettings();
  enableClockUI();	  /* Finally, enable clock UI to be visible */

  while (1) {
    // raise the task priority of LVGL and/or reduce the handler period can improve the performance
    vTaskDelay(pdMS_TO_TICKS(10));
    // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
    lv_timer_handler();
  }
}
