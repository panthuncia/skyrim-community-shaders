# The targets upstream's DirectX-Headers config defines, for the header-only overlay port: Microsoft::DirectX-Headers adds the
# include directory (<directx/d3d12.h>, <dxguids/dxguids.h>, <wsl/winadapter.h>). Microsoft::DirectX-Guids has no library
# here; on Windows the SDK's dxguid.lib defines the GUIDs.
get_filename_component(_directx_headers_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET Microsoft::DirectX-Headers)
    add_library(Microsoft::DirectX-Headers INTERFACE IMPORTED)
    set_target_properties(Microsoft::DirectX-Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_directx_headers_prefix}/include")
    if(NOT WIN32)
        set_property(TARGET Microsoft::DirectX-Headers APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_directx_headers_prefix}/include/wsl/stubs")
    endif()
endif()
if(NOT TARGET Microsoft::DirectX-Guids)
    add_library(Microsoft::DirectX-Guids INTERFACE IMPORTED)
    set_target_properties(Microsoft::DirectX-Guids PROPERTIES INTERFACE_LINK_LIBRARIES Microsoft::DirectX-Headers)
endif()
set(directx-headers_FOUND TRUE)
set(DirectX-Headers_FOUND TRUE)
unset(_directx_headers_prefix)
