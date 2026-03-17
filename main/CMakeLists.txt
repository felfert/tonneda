# Embed CA, certificate & key directly into binary
idf_component_register(SRCS app.cpp embed.c https_ota.c x509helper.cpp
                    PRIV_REQUIRES esp_driver_gpio esp_wifi mqtt esp_http_client bootloader_support
                        esp_app_format mbedtls app_update syslog wpa_supplicant nvs_flash
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES ../pki/ca.crt ../pki/client.crt ../pki/client.key ../pki/broker.crt)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-missing-field-initializers")
