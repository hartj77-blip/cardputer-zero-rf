# The RF apps: id, name, icon, colour, factory, source. On ZeroOS they are
# one installable app pack, "rf" (docs/APPS.md in the base OS): one module,
# rf.so, since they share one connection to the radio daemon.
foreach(_rf
    "spectrum|Spectrum|LV_SYMBOL_BARS|0x39c5cf|make_spectrum_app|spectrum.cpp"
    "scanner|Scanner|LV_SYMBOL_REFRESH|0x3fb950|make_scanner_app|scanner.cpp"
    "sensors|Sensors|LV_SYMBOL_TINT|0x58a6ff|make_sensors_app|sensors.cpp"
    "aircraft|Aircraft|LV_SYMBOL_GPS|0xe3b341|make_aircraft_app|aircraft.cpp"
    "aprs|APRS|LV_SYMBOL_ENVELOPE|0xff7eb6|make_aprs_app|aprs_app.cpp"
    "ft8|FT8|LV_SYMBOL_SHUFFLE|0x3fb950|make_ft8_app|ft8_app.cpp"
    "logbook|Logbook|LV_SYMBOL_LIST|0xe3b341|make_logbook_app|logbook.cpp"
    "sdrserver|SDR Server|LV_SYMBOL_UPLOAD|0xa371f7|make_sdr_server_app|sdr_server.cpp"
    "station|Station|LV_SYMBOL_HOME|0xff8a1f|make_station_app|station.cpp")
  string(REPLACE "|" ";" _f "${_rf}")
  list(GET _f 0 _id)
  list(GET _f 1 _name)
  list(GET _f 2 _icon)
  list(GET _f 3 _color)
  list(GET _f 4 _make)
  list(GET _f 5 _src)
  # After the base OS's apps (10..210) on the home screen.
  get_property(_n GLOBAL PROPERTY ZS_RF_COUNT)
  if(NOT _n)
    set(_n 0)
  endif()
  math(EXPR _order "300 + ${_n} * 10")
  math(EXPR _n "${_n} + 1")
  set_property(GLOBAL PROPERTY ZS_RF_COUNT ${_n})
  zs_app(${_id} "${_name}" ${_icon} ${_color} ${_make}
         SOURCES src/apps/${_src} LIBS zrfclient MODULE rf ORDER ${_order})
endforeach()
