#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include <driver/ledc.h>
#include "lvgl.h"


static const char *TAG = "wallclock";


#define HRESOLUTION 800
#define VRESOLUTION 480

#define LVGL_TICK_PERIOD_MS 2


static lv_disp_t *disp;
extern lv_font_t LoraBold;
static lv_obj_t *timeW;
static int hours = 0;
static int minutes = 0;
static int seconds = 0;


// Use two semaphores to sync the VSYNC event and the LVGL task, to
// avoid potential tearing effect.
SemaphoreHandle_t semVsyncEnd;
SemaphoreHandle_t semGuiReady;


static void lvglTickCB(void *) {
  /* Tell LVGL how many milliseconds has elapsed */
  lv_tick_inc(LVGL_TICK_PERIOD_MS);
}


// Timer callback for updating the seconds.
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
  esp_lcd_panel_handle_t panelH = (esp_lcd_panel_handle_t) drv->user_data;
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


static void initLCD(void) {
  esp_lcd_panel_handle_t panelH = NULL;
  static lv_disp_draw_buf_t dispBuf;
  static lv_disp_drv_t dispDrv;

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

  ESP_LOGI(TAG, "[create semaphores]");
  semVsyncEnd = xSemaphoreCreateBinary();
  assert(semVsyncEnd);
  semGuiReady = xSemaphoreCreateBinary();
  assert(semGuiReady);

  ESP_LOGI(TAG, "[configure and initialize LCD panel]");
  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panelConfig, &panelH));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panelH));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panelH));

  ESP_LOGI(TAG, "[register event callbacks]");
  esp_lcd_rgb_panel_event_callbacks_t cbs = {
    .on_vsync = vsyncCB,
  };

  static lv_disp_drv_t dispDriver;      // contains callback functions
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panelH, &cbs, &dispDriver));

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

  ESP_LOGI(TAG, "[initialize LVGL library]");
  lv_init();
  void *buf1 = NULL;
  void *buf2 = NULL;
  ESP_LOGI(TAG, "[use frame buffers as LVGL draw buffers]");
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panelH, 2, &buf1, &buf2));
  lv_disp_draw_buf_init(&dispBuf, buf1, buf2, HRESOLUTION * VRESOLUTION);

  ESP_LOGI(TAG, "[create LVGL display]");
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

  ESP_LOGI(TAG, "[install LVGL tick timer]");
  static const esp_timer_create_args_t timerArgs = {
    .callback = &lvglTickCB,
    .name = "lvglTickCB",
  };
  esp_timer_handle_t tickTimer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&timerArgs, &tickTimer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tickTimer, LVGL_TICK_PERIOD_MS * 1000));
}


void app_main(void) {
  ESP_LOGI(TAG, "[initialize LCD]");
  initLCD();

  /* Print chip information */
  esp_chip_info_t chip_info;
  uint32_t flash_size;
  esp_chip_info(&chip_info);
  printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
	 CONFIG_IDF_TARGET,
	 chip_info.cores,
	 (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
	 (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
	 (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
	 (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

  unsigned major_rev = chip_info.revision / 100;
  unsigned minor_rev = chip_info.revision % 100;
  printf("silicon revision v%d.%d, ", major_rev, minor_rev);
  if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
    printf("Get flash size failed");
    return;
  }

  printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
	 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

  printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

  ESP_LOGI(TAG, "[set up clock UI]");
  setupClockUI();

  while (1) {
    // raise the task priority of LVGL and/or reduce the handler period can improve the performance
    vTaskDelay(pdMS_TO_TICKS(10));
    // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
    lv_timer_handler();
  }
}
