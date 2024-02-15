idf_component_register(SRCS "wallclock.c" "LoraBold.c"
  INCLUDE_DIRS ".")

set_source_files_properties(wallclock.c
  PROPERTIES
  COMPILE_DEFINITIONS "WIFI_SSID=\"$ENV{WIFI_SSID}\";WIFI_PASS=\"$ENV{WIFI_PASS}\"")
