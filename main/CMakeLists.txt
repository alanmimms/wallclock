idf_component_register(SRCS "wallclock.c" "LoraBold.c" "dlink.h"
  INCLUDE_DIRS ".")

nvs_create_partition_image(nvs ../nvs.csv FLASH_IN_PROJECT)
