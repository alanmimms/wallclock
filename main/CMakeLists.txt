idf_component_register(
  SRCS "wallclock.c" "LoraBold240.c" "RobotoMedium40.c" "cog.c" "visible.c" "invisible.c"
  INCLUDE_DIRS ".")

nvs_create_partition_image(nvs ../nvs.csv)
