#include <stdio.h>

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

#include "dlink.h"


static const char *TAG = "wallclock";

// THIS COULD BE HELPFUL:
// https://components.espressif.com/components/espressif/esp_lvgl_port
// https://github.com/pjaos/mgos_esp32_littlevgl_wifi_setup/tree/master


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


// Styles and UI widgets.
static lv_style_t borderStyle;
static lv_style_t popupBoxStyle;

static lv_disp_t *disp;
extern lv_font_t LoraBold;

static lv_obj_t *timeW;

static lv_obj_t *settings;
static lv_obj_t *settingsBtn;
static lv_obj_t *settingsCloseBtn;
static lv_obj_t *wfList;
static lv_obj_t *settingsLabel;
static lv_obj_t *mboxConnect;
static lv_obj_t *mboxTitle;
static lv_obj_t *mboxPassword;
static lv_obj_t *mboxConnectBtn;
static lv_obj_t *mboxCloseBtn;
static lv_obj_t *keyboard;
static lv_obj_t *popupBox;
static lv_obj_t *popupBoxCloseBtn;


static const char *ssid;
static const char *ssidPW;


// Doubly-linked-list of WiFi SSIDs and their passwords.
typedef struct sWiFiEntry {
  tDLEntry dl;			/* MUST be first element */
  char *ssidP;			/* Pointer to NUL terminated SSID name */
  char *passP;			/* Pointer to NUL terminated password */
} tWiFiEntry;

static tWiFiEntry wifiList = {
  .dl.prevP = &wifiList.dl,
  .dl.nextP = &wifiList.dl,
};


// Doubly-linked-list of NTP servers.
typedef struct sNTPEntry {
  tDLEntry dl;			/* MUST be first element */
  char *server;			/* Pointer to NUL terminated NTP server hostname */
} tNTPEntry;

static tNTPEntry ntpList = {
  .dl.prevP = &ntpList.dl,
  .dl.nextP = &ntpList.dl,
};

// Use two semaphores to sync the VSYNC event and the LVGL task, to
// avoid potential tearing effect.
SemaphoreHandle_t semVsyncEnd;
SemaphoreHandle_t semGuiReady;


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

  lv_label_set_text_fmt(timeW, "%02d:%02d", hours, minutes);
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


static void setupClockUI(void) {
  lv_obj_t *screenW = lv_disp_get_scr_act(disp);

  ESP_LOGI(TAG, "[set up clock UI]");
  timeW = lv_label_create(screenW);
  lv_label_set_text_static(timeW, "00:00");
  lv_obj_set_style_text_font(timeW, &LoraBold, LV_PART_MAIN);
  lv_obj_set_style_text_align(timeW, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_update_layout(timeW);

  int w = lv_obj_get_width(timeW);
  int h = lv_obj_get_height(timeW);
  lv_obj_set_pos(timeW, (HRESOLUTION - w)/2, (VRESOLUTION - h)/2);

  lv_timer_create(secondsCB, 1000, NULL);
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

  // XXX need to setup GPIOs?

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
    .sda_io_num = GPIO_NUM_19,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_io_num = GPIO_NUM_20,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
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


static void setupStyles(void) {
  lv_style_init(&borderStyle);
  lv_style_set_border_width(&borderStyle, 2);
  lv_style_set_border_color(&borderStyle, lv_palette_main(LV_PALETTE_TEAL));

  lv_style_init(&popupBoxStyle);
  lv_style_set_radius(&popupBoxStyle, 10);
  lv_style_set_bg_opa(&popupBoxStyle, LV_OPA_COVER);
  lv_style_set_border_color(&popupBoxStyle, lv_palette_main(LV_PALETTE_BLUE_GREY));
  lv_style_set_border_width(&popupBoxStyle, 5);
}


static void settingsButtonEventCB(lv_event_t *e) {

  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    lv_obj_clear_flag(settings, LV_OBJ_FLAG_HIDDEN);
  }
}


static void settingsCloseButtonEventCB(lv_event_t *e) {

  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN);
  }
}


static void mboxConnectButtonEventCB(lv_event_t *e) {

  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    ssidPW = String(lv_textarea_get_text(mboxPassword));
    networkConnector();
    lv_obj_move_background(mboxConnect);
    popupMsgBox("Connecting!", "Attempting to connect to the selected network.");
  }
}


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


static void setupKeyboard(void) {
  keyboard = lv_keyboard_create(lv_scr_act());
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}


static void setupSettings(void) {
  settings = lv_obj_create(lv_scr_act());
  lv_obj_add_style(settings, &borderStyle, 0);
  lv_obj_set_size(settings, HRESOLUTION - 100, VRESOLUTION - 40);
  lv_obj_align(settings, LV_ALIGN_TOP_RIGHT, -20, 20);

  settingsLabel = lv_label_create(settings);
  lv_label_set_text(settingsLabel, "Settings " LV_SYMBOL_SETTINGS);
  lv_obj_align(settingsLabel, LV_ALIGN_TOP_LEFT, 0, 0);

  settingsCloseBtn = lv_btn_create(settings);
  lv_obj_set_size(settingsCloseBtn, 30, 30);
  lv_obj_align(settingsCloseBtn, LV_ALIGN_TOP_RIGHT, 0, -10);
  lv_obj_add_event_cb(settingsCloseBtn, buttonEventCB, LV_EVENT_ALL, NULL);
  lv_obj_t *btnSymbol = lv_label_create(settingsCloseBtn);
  lv_label_set_text(btnSymbol, LV_SYMBOL_CLOSE);
  lv_obj_center(btnSymbol);

  wfList = lv_list_create(settings);
  lv_obj_set_size(wfList, HRESOLUTION - 140, 210);
  lv_obj_align_to(wfList, settingsLabel, LV_ALIGN_TOP_LEFT, 0, 30);
}


// Read our NVS variables
static void setupNVS(void) {
  // Initialize NVS subsystem to use default partition.
  ESP_ERROR_CHECK(nvs_flash_init());

  // Open our NVS namespace.
  nvs_handle_t wifiNVSH;
  ESP_ERROR_CHECK(nvs_open("WiFi", NVS_READONLY, &h));

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


void app_main(void) {
  printChipInfo();

  setupI2C();
  setupLCD();
  setupBacklightPWM();
  setupTouch();

  setupNVS();
  setupSNTP();

  setupStyles();
  setupKeyboard();
  setupStatusBar();
  setupPasswordBox();
  setupSettings();

  setupNetwork();
  setupClockUI();

  while (1) {
    // raise the task priority of LVGL and/or reduce the handler period can improve the performance
    vTaskDelay(pdMS_TO_TICKS(10));
    // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
    lv_timer_handler();
  }
}
