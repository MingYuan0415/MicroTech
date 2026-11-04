# MicroTech product composition resolver.
#
# A product selects a board, a design target and an explicit subset of the App
# catalog. It may declare non-BSP service capabilities (for example a hosted
# radio), but it cannot invent a BSP peripheral capability.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../layers/apps/app_catalog.cmake")

set(MT_PRODUCTS_DIR "${CMAKE_CURRENT_LIST_DIR}")
set_property(GLOBAL PROPERTY MT_PRODUCT_RESOLVED FALSE)

function(_mt_product_manifest_path product_id out_var)
    string(REPLACE "/" ";" parts "${product_id}")
    list(LENGTH parts part_count)
    if(NOT part_count EQUAL 1)
        message(FATAL_ERROR "Invalid MicroTech product id '${product_id}'")
    endif()
    set(manifest "${MT_PRODUCTS_DIR}/${product_id}/product.cmake")
    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR
            "Unknown MicroTech product '${product_id}': missing ${manifest}")
    endif()
    set(${out_var} "${manifest}" PARENT_SCOPE)
endfunction()

function(mt_resolve_product board_id requested_product)
    set(product_id "${requested_product}")
    if(product_id STREQUAL "")
        if(board_id STREQUAL "waveshare_esp32_s3_touch_amoled_1_8")
            set(product_id "s3_reference")
        elseif(board_id STREQUAL "waveshare_esp32_p4_wifi6_touch_lcd_3_5")
            set(product_id "p4_reference")
        else()
            message(FATAL_ERROR
                "No default MicroTech product for board ${board_id}")
        endif()
    endif()

    _mt_product_manifest_path("${product_id}" manifest)
    unset(MT_PRODUCT_ID)
    unset(MT_PRODUCT_BOARD)
    unset(MT_PRODUCT_DESIGN_TARGET)
    unset(MT_PRODUCT_HAL_CAPABILITIES)
    unset(MT_PRODUCT_RADIO_ENABLED)
    unset(MT_PRODUCT_RADIO_OPTIONAL)
    unset(MT_PRODUCT_SYSTEM_PM_SUPPORTED)
    unset(MT_PRODUCT_HOSTED_RADIO)
    unset(MT_PRODUCT_DEVICE_PANEL)
    unset(MT_PRODUCT_DEVICE_PLATFORM)
    unset(MT_PRODUCT_FIRMWARE_REQUIRES)
    unset(MT_PRODUCT_APPS)
    unset(MT_PRODUCT_REQUIRED_APPS)
    include("${manifest}")

    foreach(field MT_PRODUCT_ID MT_PRODUCT_BOARD MT_PRODUCT_DESIGN_TARGET
            MT_PRODUCT_RADIO_ENABLED MT_PRODUCT_RADIO_OPTIONAL
            MT_PRODUCT_SYSTEM_PM_SUPPORTED MT_PRODUCT_HOSTED_RADIO
            MT_PRODUCT_DEVICE_PANEL MT_PRODUCT_DEVICE_PLATFORM
            MT_PRODUCT_APPS MT_PRODUCT_REQUIRED_APPS)
        if(NOT DEFINED ${field} OR "${${field}}" STREQUAL "")
            message(FATAL_ERROR "${manifest} lacks ${field}")
        endif()
    endforeach()
    if(NOT MT_PRODUCT_ID STREQUAL product_id)
        message(FATAL_ERROR
            "${manifest} declares MT_PRODUCT_ID=${MT_PRODUCT_ID}, expected ${product_id}")
    endif()
    if(NOT MT_PRODUCT_BOARD STREQUAL board_id)
        message(FATAL_ERROR
            "Product ${MT_PRODUCT_ID} requires board ${MT_PRODUCT_BOARD}, got ${board_id}")
    endif()

    mt_app_load_catalog(catalog)
    foreach(app_id IN LISTS MT_PRODUCT_APPS)
        if(NOT app_id IN_LIST catalog)
            message(FATAL_ERROR
                "Product ${MT_PRODUCT_ID} selects unknown App '${app_id}'")
        endif()
    endforeach()
    foreach(required_app IN LISTS MT_PRODUCT_REQUIRED_APPS)
        if(NOT required_app IN_LIST MT_PRODUCT_APPS)
            message(FATAL_ERROR
                "Product ${MT_PRODUCT_ID} requires unselected App '${required_app}'")
        endif()
    endforeach()

    foreach(app_id IN LISTS MT_APP_CATALOG_IDS)
        mt_app_read("${app_id}" selected_id)
        if(MT_APP_REQUIRED AND NOT app_id IN_LIST MT_PRODUCT_APPS)
            message(FATAL_ERROR
                "Product ${MT_PRODUCT_ID} omits required App '${app_id}'")
        endif()
    endforeach()

    set(${requested_product} "${MT_PRODUCT_ID}" PARENT_SCOPE)
    set(MT_PRODUCT_ID "${MT_PRODUCT_ID}" PARENT_SCOPE)
    set(MT_PRODUCT_BOARD "${MT_PRODUCT_BOARD}" PARENT_SCOPE)
    set(MT_PRODUCT_DESIGN_TARGET "${MT_PRODUCT_DESIGN_TARGET}" PARENT_SCOPE)
    set(MT_PRODUCT_HAL_CAPABILITIES
        "${MT_PRODUCT_HAL_CAPABILITIES}" PARENT_SCOPE)
    set(MT_PRODUCT_RADIO_ENABLED "${MT_PRODUCT_RADIO_ENABLED}" PARENT_SCOPE)
    set(MT_PRODUCT_RADIO_OPTIONAL "${MT_PRODUCT_RADIO_OPTIONAL}" PARENT_SCOPE)
    set(MT_PRODUCT_SYSTEM_PM_SUPPORTED
        "${MT_PRODUCT_SYSTEM_PM_SUPPORTED}" PARENT_SCOPE)
    set(MT_PRODUCT_HOSTED_RADIO "${MT_PRODUCT_HOSTED_RADIO}" PARENT_SCOPE)
    set(MT_PRODUCT_DEVICE_PANEL "${MT_PRODUCT_DEVICE_PANEL}" PARENT_SCOPE)
    set(MT_PRODUCT_DEVICE_PLATFORM "${MT_PRODUCT_DEVICE_PLATFORM}" PARENT_SCOPE)
    set(MT_PRODUCT_FIRMWARE_REQUIRES
        "${MT_PRODUCT_FIRMWARE_REQUIRES}" PARENT_SCOPE)
    set(MT_PRODUCT_APPS "${MT_PRODUCT_APPS}" PARENT_SCOPE)
    set(MT_PRODUCT_REQUIRED_APPS "${MT_PRODUCT_REQUIRED_APPS}" PARENT_SCOPE)
    set(MT_PRODUCT_MANIFEST "${manifest}" PARENT_SCOPE)
    set(MT_PRODUCT_DIR "${MT_PRODUCTS_DIR}/${product_id}" PARENT_SCOPE)
    set_property(GLOBAL PROPERTY MT_PRODUCT_RESOLVED TRUE)
endfunction()

function(mt_validate_product_capabilities board_capabilities product_capabilities)
    set(known "${board_capabilities};${product_capabilities}")
    if(NOT board_capabilities)
        # Host consumers resolve a product without entering the IDF component
        # build. Read the product's board manifest here so capability validation
        # does not depend on caller-local variables.
        get_filename_component(_mt_products_parent
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" DIRECTORY)
        include("${_mt_products_parent}/layers/bsp/boards.cmake")
        if(MT_PRODUCT_BOARD STREQUAL "waveshare_esp32_s3_touch_amoled_1_8")
            mt_resolve_board("esp32s3" "${MT_PRODUCT_BOARD}")
        elseif(MT_PRODUCT_BOARD STREQUAL "waveshare_esp32_p4_wifi6_touch_lcd_3_5")
            mt_resolve_board("esp32p4" "${MT_PRODUCT_BOARD}")
        else()
            message(FATAL_ERROR
                "Product ${MT_PRODUCT_ID} has unknown board ${MT_PRODUCT_BOARD}")
        endif()
        set(known "${MT_BOARD_HAL_CAPABILITIES};${product_capabilities}")
    endif()
    foreach(app_id IN LISTS MT_PRODUCT_APPS)
        mt_app_read("${app_id}" selected_id)
        foreach(capability IN LISTS MT_APP_CAPABILITIES)
            if(NOT capability IN_LIST known)
                message(FATAL_ERROR
                    "Product ${MT_PRODUCT_ID} App ${app_id} requires capability "
                    "'${capability}', which neither the board nor product provides")
            endif()
        endforeach()
    endforeach()
endfunction()

function(mt_product_expand_apps)
    get_property(resolved GLOBAL PROPERTY MT_PRODUCT_RESOLVED)
    if(NOT resolved)
        message(FATAL_ERROR "mt_resolve_product() must run before App expansion")
    endif()
    mt_apps_expand("${MT_PRODUCT_APPS}"
        MT_PRODUCT_APP_SOURCES
        MT_PRODUCT_APP_INCLUDE_DIRS
        MT_PRODUCT_APP_REQUIRES
        MT_PRODUCT_APP_CAPABILITIES
        MT_PRODUCT_APP_RESOURCE_MANIFESTS
        MT_PRODUCT_APP_RESET_HOOKS
        MT_PRODUCT_SELECTED_APP_IDS)
    foreach(variable
            MT_PRODUCT_APP_SOURCES
            MT_PRODUCT_APP_INCLUDE_DIRS
            MT_PRODUCT_APP_REQUIRES
            MT_PRODUCT_APP_CAPABILITIES
            MT_PRODUCT_APP_RESOURCE_MANIFESTS
            MT_PRODUCT_APP_RESET_HOOKS
            MT_PRODUCT_SELECTED_APP_IDS)
        set(${variable} "${${variable}}" PARENT_SCOPE)
    endforeach()
endfunction()

function(mt_product_write_cmake output_file)
    set(lines
        "# Generated by products/products.cmake - do not edit"
        "set(MT_PRODUCT_ID \"${MT_PRODUCT_ID}\")"
        "set(MT_PRODUCT_HEADER \"${MT_PRODUCT_HEADER}\")"
        "set(MT_PRODUCT_FIRMWARE_REQUIRES \"${MT_PRODUCT_FIRMWARE_REQUIRES}\")")
    set(_mt_fragment "")
    foreach(variable
            MT_PRODUCT_APP_SOURCES
            MT_PRODUCT_APP_INCLUDE_DIRS
            MT_PRODUCT_APP_REQUIRES
            MT_PRODUCT_APP_RESOURCE_MANIFESTS
            MT_PRODUCT_SELECTED_APP_IDS)
        string(APPEND _mt_fragment "set(${variable}\n")
        foreach(value IN LISTS ${variable})
            string(APPEND _mt_fragment "    \"${value}\"\n")
        endforeach()
        string(APPEND _mt_fragment ")\n")
    endforeach()
    string(JOIN "\n" _mt_header ${lines})
    file(WRITE "${output_file}" "${_mt_header}\n${_mt_fragment}")
endfunction()

function(mt_product_write_header output_file)
    set(app_definitions "")
    set(app_checks "")
    foreach(app_id IN LISTS MT_PRODUCT_SELECTED_APP_IDS)
        if(app_id STREQUAL "common")
            continue()
        endif()
        string(TOUPPER "${app_id}" app_upper)
        string(APPEND app_definitions
            "#define MT_PRODUCT_HAS_${app_upper}_APP 1\n")
        string(APPEND app_checks
            "    \"${app_id}\",\n")
    endforeach()
    set(required_checks "")
    foreach(app_id IN LISTS MT_PRODUCT_REQUIRED_APPS)
        string(APPEND required_checks
            "    \"${app_id}\",\n")
    endforeach()
    string(TOLOWER "${MT_PRODUCT_ID}" id_upper_seed)
    string(TOUPPER "${id_upper_seed}" product_upper)
    foreach(_mt_bool IN ITEMS
            MT_PRODUCT_RADIO_ENABLED
            MT_PRODUCT_RADIO_OPTIONAL
            MT_PRODUCT_SYSTEM_PM_SUPPORTED
            MT_PRODUCT_HOSTED_RADIO)
        if(${_mt_bool})
            set(${_mt_bool}_C 1)
        else()
            set(${_mt_bool}_C 0)
        endif()
    endforeach()
    set(content
        "#ifndef MT_PRODUCT_PROFILE_GENERATED_H\n#define MT_PRODUCT_PROFILE_GENERATED_H\n\n#define MT_PRODUCT_NAME \"${MT_PRODUCT_ID}\"\n#define MT_PRODUCT_ID_${product_upper} 1\n#define MT_PRODUCT_RADIO_ENABLED ${MT_PRODUCT_RADIO_ENABLED_C}\n#define MT_PRODUCT_RADIO_OPTIONAL ${MT_PRODUCT_RADIO_OPTIONAL_C}\n#define MT_PRODUCT_SYSTEM_PM_SUPPORTED ${MT_PRODUCT_SYSTEM_PM_SUPPORTED_C}\n#define MT_PRODUCT_HOSTED_RADIO ${MT_PRODUCT_HOSTED_RADIO_C}\n#define MT_PRODUCT_DEVICE_PANEL \"${MT_PRODUCT_DEVICE_PANEL}\"\n#define MT_PRODUCT_DEVICE_PLATFORM \"${MT_PRODUCT_DEVICE_PLATFORM}\"\n${app_definitions}\nstatic const char *const s_mt_product_app_ids[] =\n{\n${app_checks}};\n\nstatic const char *const s_mt_product_required_app_ids[] =\n{\n${required_checks}};\n\n#endif /* MT_PRODUCT_PROFILE_GENERATED_H */\n")
    file(WRITE "${output_file}" "${content}")
endfunction()
