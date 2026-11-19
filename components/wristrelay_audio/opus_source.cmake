function(wr_add_opus)
    if(TARGET opus)
        return()
    endif()
    include(FetchContent)
    set(WR_OPUS_ARCHIVE "" CACHE STRING "Opus 1.5.2 offline archive path or HTTPS URL override")
    if(WR_OPUS_ARCHIVE)
        set(archives "${WR_OPUS_ARCHIVE}")
        if(NOT archives MATCHES "^https://")
            cmake_path(CONVERT "${archives}" TO_CMAKE_PATH_LIST archives NORMALIZE)
        endif()
    else()
        set(archives https://ftp.osuosl.org/pub/xiph/releases/opus/opus-1.5.2.tar.gz
                     https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz)
    endif()
    set(OPUS_FIXED_POINT ON CACHE BOOL "Build fixed-point Opus" FORCE)
    set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "Use portable fixed-point Opus kernels" FORCE)
    set(OPUS_ENABLE_FLOAT_API OFF CACHE BOOL "Use the integer Opus API only" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "Skip upstream Opus tests in the firmware build" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "Skip upstream Opus programs" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "Skip upstream Opus install metadata" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "Skip upstream Opus install metadata" FORCE)
    FetchContent_Declare(wr_opus_upstream
        URL ${archives}
        URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
        TLS_VERIFY TRUE
        TIMEOUT 90
        INACTIVITY_TIMEOUT 30
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(wr_opus_upstream)
endfunction()
