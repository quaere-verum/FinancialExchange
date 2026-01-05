add_executable(FinancialExchange main.cpp)

target_link_libraries(FinancialExchange PRIVATE exchange_core)

target_include_directories(FinancialExchange PRIVATE ${PROJECT_SOURCE_DIR}/src)
