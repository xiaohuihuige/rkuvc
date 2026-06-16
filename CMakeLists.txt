cmake_minimum_required(VERSION 2.8.0 FATAL_ERROR)
PROJECT(uvc_app)

# 设置交叉编译工具链路径
SET(CMAKE_C_COMPILER /home/wmh/falcon/Omni3576-sdk/buildroot/output/rockchip_rk3576_ipc/host/bin/aarch64-buildroot-linux-gnu-gcc)
SET(CMAKE_CXX_COMPILER /home/wmh/falcon/Omni3576-sdk/buildroot/output/rockchip_rk3576_ipc/host/bin/aarch64-buildroot-linux-gnu-g++)
SET(CMAKE_STRIP /home/wmh/falcon/Omni3576-sdk/buildroot/output/rockchip_rk3576_ipc/host/bin/aarch64-buildroot-linux-gnu-strip)

include_directories(uvc)

set(LIB_SOURCE
    uvc/uvc-gadget.c
    uvc/uvc_gadget_run.c
    uvc/uvc_video.cpp
    uvc/yuv.c
    uvc/uvc_control.c
    uvc/uvc_encode.cpp
    uvc/uevent.c
    uvc/uvc_event.c
    uvc/uvc_device_monitor.c
    uvc/uvc_monitor_parse.c
    uvc/uvc_session.c
    uvc/uvc_app.c
    uvc/uvc_epoll.c
    uvc/drm.c
)
add_library(rkuvc SHARED ${LIB_SOURCE})
target_link_libraries(rkuvc pthread drm)

set(SOURCE
    main.c
    ${LIB_SOURCE}
)

ADD_EXECUTABLE(uvc_app ${SOURCE})
target_link_libraries(uvc_app pthread drm)

install(TARGETS rkuvc DESTINATION lib)
install(DIRECTORY ./uvc DESTINATION include
        FILES_MATCHING PATTERN "*.h")

install(TARGETS uvc_app DESTINATION bin)

if(RK3588)
install(FILES rk3588/usb_config.sh DESTINATION bin
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE)
elseif(RK3576)
install(FILES rk3576/usb_config.sh DESTINATION bin
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE)
else()
install(FILES uvc_config.sh DESTINATION bin
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE)
endif()
