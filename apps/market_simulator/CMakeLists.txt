add_executable(MarketSimulator main.cpp)

target_link_libraries(MarketSimulator PRIVATE exchange_core)

target_include_directories(MarketSimulator PRIVATE ${PROJECT_SOURCE_DIR}/src)
