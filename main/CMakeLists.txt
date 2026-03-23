
# List all pki files (certs/keys) to be embedded. Files MUST be in pki/ subdir.
# If you want different names, you must change main/app.cpp. So you probably want to use symlinks instead.
set(ALL_PKI_PEM ca.crt client.crt client.key broker.crt)

function(set_all_pki_der)
    unset(_all)
    foreach(_arg IN LISTS ARGN)
        get_filename_component(_name "${_arg}" NAME)
        list(APPEND all "${CMAKE_BINARY_DIR}/${_name}.der")
    endforeach()
    set(ALL_PKI_DER ${all} PARENT_SCOPE)
endfunction()
set_all_pki_der(${ALL_PKI_PEM})

idf_component_register(SRCS app.cpp embed.c https_ota.c x509helper.cpp tonneda.cpp
                    PRIV_REQUIRES esp_driver_gpio esp_wifi mqtt esp_http_client bootloader_support
                        esp_app_format mbedtls app_update syslog wpa_supplicant nvs_flash
                    INCLUDE_DIRS "."
                    EMBED_FILES ${ALL_PKI_DER})

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-missing-field-initializers")

function(generate_der_target target)
    unset(sources)
    foreach(_arg IN LISTS ARGN)
        set(_src "${CMAKE_SOURCE_DIR}/pki/${_arg}")
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
generate_der_target(derfiles ${ALL_PKI_PEM})
