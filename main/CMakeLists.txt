# Embed CA, certificate & key directly into binary
idf_component_register(SRCS app.cpp embed.c https_ota.c x509helper.cpp tonneda.cpp
                    PRIV_REQUIRES esp_driver_gpio esp_wifi mqtt esp_http_client bootloader_support
                        esp_app_format mbedtls app_update syslog wpa_supplicant nvs_flash
                    INCLUDE_DIRS "."
                    EMBED_FILES ../build/ca.crt.der ../build/client.crt.der ../build/client.key.der ../build/broker.crt.der)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-missing-field-initializers")

function(generate_der_target target)
    unset(sources)
    foreach(_src IN LISTS ARGN)
        get_filename_component(_name "${_src}" NAME)
        set(_out "${CMAKE_BINARY_DIR}/${_name}.der")
        message(STATUS "_out=${_out}")
        add_custom_command(
            OUTPUT ${_out}
            COMMAND sh -c "openssl x509 -in ${_src} -outform DER -out ${_out} || openssl rsa -in ${_src} -outform DER -out ${_out}"
            VERBATIM
        )
        list(APPEND sources "${_out}")
  endforeach()
  add_custom_target(${target} DEPENDS ${sources})
  add_dependencies(${COMPONENT_LIB} ${target})
endfunction()

generate_der_target(derfiles
    ${CMAKE_SOURCE_DIR}/pki/ca.crt
    ${CMAKE_SOURCE_DIR}/pki/client.crt
    ${CMAKE_SOURCE_DIR}/pki/client.key
    ${CMAKE_SOURCE_DIR}/pki/broker.crt
)
