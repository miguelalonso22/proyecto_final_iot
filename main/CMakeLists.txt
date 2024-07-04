set(COMPONENT_SRCS "main.c" )

register_component()

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)

target_compile_options(${COMPONENT_LIB} PRIVATE
                                        -Wno-deprecated-declarations)
