add_library(grblhal-thermistor INTERFACE)
target_sources(grblhal-thermistor INTERFACE ${CMAKE_CURRENT_LIST_DIR}/thermistor.c)
target_include_directories(grblhal-thermistor INTERFACE ${CMAKE_CURRENT_LIST_DIR})
