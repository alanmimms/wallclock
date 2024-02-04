# WTF?

This is a wall clock based on the [Elecrow display-7.0 Inch HMI
Display 800x400 RGB TFT LCD Screen Compatible with
Arduino/LVGL/PlatformIO/Micropython](https://m.elecrow.com/pages/shop/product/details?id=206594&=). For
more information about this device, see its
[Wiki](https://www.elecrow.com/wiki/index.php?title=ESP32_Display_7.0%27%27_Intelligent_Touch_Screen_Wi-Fi%26BLE_800*480_HMI_Display)

This device is a wall clock that will eventually display Internet
accurate time of day, day of week, date, local weather, and
automagically adjust for daylight savings time (if it survives 2024 as
a concept -- it seems there are many people like me who think it's a
bogus concept and should be eliminated -- one can only hope).

I built this because I bought an "atomic" (WWVB in the USA) calibrated
wall clock which is very nice, but it could never hear WWVB -- I
assume this is because my home lab is rather electrically noisy. But I
wanted a wall clock so I wouldn't have to fumble with my mouse or
reserve desktop screen real estate to know the time of day and date or
weather at a glance.

I built this using ESP-IDF from Espressif, a Shanghai company whose
products I have come to admire in recent years. This device uses the
ESP32-S3, which as I developed this is one of the newer forms of ESP32
SoC, including a LCD panel driving module in the SoC that drives the
LCD on this board.

I found Arduino stuff to be too version-drifty and too badly documented.

I found LVGL to be well documented, but integrating with Arduino could
be challenging because, again, version-driftiness.

So I use LVGL 8.3 (as of this writing) integrated with ESP32 using
ESP-IDF. The LVGL software was added simply by adding LVGL as a
dependency in my project (see `idf_component.yml` for details). The
ESP-IDF build system pulled in the specified version of LVGL and built
it with no fuss.

I used LVGL 8.* because version 9.* isn't quite baked yet and the APIs
changed a good bit for integrating a display driver. What can I say?
I'm lazy.


