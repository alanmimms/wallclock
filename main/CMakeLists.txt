idf_component_register(SRCS "wallclock.c" "LoraBold.c"
  INCLUDE_DIRS ".")

nvs_create_partition_image(nvs ../nvs.csv)
