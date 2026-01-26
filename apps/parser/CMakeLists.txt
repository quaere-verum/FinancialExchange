add_executable(Parser main.cpp)

target_link_libraries(Parser PRIVATE exchange_core)
target_include_directories(Parser PRIVATE ${PROJECT_SOURCE_DIR}/src)

find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)

target_link_libraries(Parser PRIVATE
    Arrow::arrow_shared
    Parquet::parquet_shared
)
