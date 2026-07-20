qt_add_dbus_interface(
   HEADLESS_XDP_SRCS
   org.freedesktop.portal.ScreenCast.xml
   xdp_dbus_screencast_interface
)

qt_add_dbus_interface(
   HEADLESS_XDP_SRCS
   org.freedesktop.portal.RemoteDesktop.xml
   xdp_dbus_remotedesktop_interface
)

add_executable(kpipewireheadlesstest HeadlessTest.cpp headlessportalutils.cpp portalfdutils.cpp ${HEADLESS_XDP_SRCS})
target_link_libraries(kpipewireheadlesstest Qt::DBus Qt::Gui KPipeWire KPipeWireDmaBuf KPipeWireRecord KF6::CoreAddons)

#########

qt_add_dbus_interface(
   XDP_RECORDME_SRCS
   org.freedesktop.portal.ScreenCast.xml
   xdp_dbus_screencast_interface
)

add_executable(xdp-recordme xdp-main.cpp RecordMe.cpp portalfdutils.cpp recordme.qrc ${XDP_RECORDME_SRCS})
target_link_libraries(xdp-recordme Qt::DBus Qt::Quick Qt::QuickControls2 KPipeWire)

ecm_add_test(RecordMePortalTest.cpp RecordMe.cpp portalfdutils.cpp ${XDP_RECORDME_SRCS} TEST_NAME RecordMePortalTest LINK_LIBRARIES Qt::Core Qt::DBus Qt::Gui Qt::Quick Qt::QuickControls2 Qt::Test)
ecm_add_test(HeadlessPortalTest.cpp headlessportalutils.cpp portalfdutils.cpp TEST_NAME HeadlessPortalTest LINK_LIBRARIES Qt::Core Qt::Test)

option(INSTALL_TESTS "The recordme test needs installing, disabled by default for convenience" OFF)
if(INSTALL_TESTS)
   configure_file(org.kde.kpipewireheadlesstest.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kpipewireheadlesstest.desktop @ONLY)

   install(TARGETS xdp-recordme kpipewireheadlesstest DESTINATION ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
   install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kpipewireheadlesstest.desktop DESTINATION ${KDE_INSTALL_APPDIR})
endif()

add_executable(webcamtest webcamtest.cpp webcamtest.qml webcamtest.qrc)
target_link_libraries(webcamtest Qt::Quick KPipeWire)
