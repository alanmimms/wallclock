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


# Building

First, [install the excellent ESP-IDF from
Espressif](https://docs.espressif.com/projects/esp-idf/en/v5.1.2/esp32s3/get-started/index.html). I
used that version for that SoC chip because that is what matched the
board's SoC and it is the most recent stable release as of this
writing.

Connect the board to a USB port on your computer that can source
several amps. I used a USB3 (blue) USB port on my rather hefty desktop
machine and it worked fine. The display should boot up and run its
factory sample software when you do this. For me, on my Linux box,
this device showed up as `/dev/ttyUSB0` because I make it a rule to
remove other serial devices when I'm just fooling around with a new
toy, so I didn't have any other devices to contend with.

You have to do something manual to get into the "download mode" of the
board. Just press and hold the RESET button, press and hold the BOOT
button, release the RESET button, release the BOOT button. This
becomes second nature after doing it a few thousand times. This puts
the board into "download mode" which allows the ESP-IDF software to
flash a new build.

If you want to reset the board and run what is in its flash, just
click the RESET button.

You can run the ESP-IDF `monitor` to see the terminal output of the
board when it boots. For the factory demo software I remember this as
being pretty boring, but it will show you if you're connected properly.

When you have cloned this git repository, you can just go to its top
level directory and do

	. ../esp-idf/export.sh
	idf.py -p /dev/ttyUSB0 build flash monitor

When the build is complete, ESP-IDF will start to flash the new image,
which takes a few seconds. While that is going on, the `monitor` mode
is running, so you can see its progress. If the board wasn't in
"download mode" when you ran this command you can just do the
RESET-BOOT sequence described above to get it into that mode and run
the command again. The second time, since no changes were made, the
`build flash monitor` doesn't have to rebuild anything and it will
flash the image immediately.

When the flashing process is done the `monitor` will show a message
like `Hard resetting via RTS pin...` but this board isn't designed to
reset when the USB serial interface's RTS pin is wiggled, so you have
to press the RESET button manually. This will start your new flashed
image.

To exit the `monitor` mode you can just hit control-`]`. The board
will continue to run its software even if you're not watching it, like
a good little computer.


# TO DO

* Set backlight using PWM instead of always full ON.

* Vary backlight based on room occupancy and light intensity and via
  Home Assistant MQTT messaging or similar.

* Build the clock UI to replace the toy scatterchart UI.

* Optimize display updating to avoid full copy of the framebuffer each
  time? Scatterchart gets ~16FPS with double buffering. While this is
  surely enough for a wall clock, it somehow seems inelegant to leave
  this poor little machine working so hard to accomplish so little.


# UI and Functional Notes

* In normal operation, Wallclock shows the time, day and date, a
  Settings icon button, and a status bar.

* Clicking the Settings button brings up the settings UI:
  * List of WiFi APs known to Wallclock and a UI to change or delete
    any of these.
  * A button to add more WiFi APs by scanning for them and entering
    credentials and an NTP pool hostname list.

* Wallclock saves in its internal flash storage an encrypted set of
  WiFi credentials for some number of WiFi APs it can associate with.

* If WiFi doesn't find any authenticatable, functioning, and NTP
  reachable WiFi access points it knows about, the status bar will
  show *Internet Down* or *Network Time Protocol Down* status.

See
[this link](https://lucid.app/lucidchart/3e97ec9d-b1f9-4f4e-951b-0cfc45df0f84/edit?viewport_loc=-836%2C487%2C622%2C649%2C0_0&invitationId=inv_bf1296f4-8365-4834-ac4a-15a4939b802c)
for the UI design wireframe I made with the unfortunately somewhat
crippled free version of the wonderful tool LucidChart. Each "slide"
in the LucidChart is framed as a slide solely so I could see what the
UI would look like in a frame. LucidChart can do way more than slide
presentations! Note: I have no affiliation with the Lucid.app people
other than using and liking their tools.
