cmake_minimum_required(VERSION 3.22)
project(uperf-gui LANGUAGES C)
set(CMAKE_C_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4>=4.6)
pkg_check_modules(LIBADWAITA REQUIRED libadwaita-1>=1.2)

add_executable(uperf-gui
    main.c
    src/dbus_proxy.c
    include/dbus_proxy.h
)

target_include_directories(uperf-gui PRIVATE
    ${GTK4_INCLUDE_DIRS}
    ${LIBADWAITA_INCLUDE_DIRS}
    include
)

target_link_libraries(uperf-gui PRIVATE
    ${GTK4_LIBRARIES}
    ${LIBADWAITA_LIBRARIES}
)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
target_link_libraries(uperf-gui PRIVATE ${GLIB2_LIBRARIES})

install(TARGETS uperf-gui DESTINATION /usr/local/bin)
install(FILES uperf-gui.desktop DESTINATION share/applications)
