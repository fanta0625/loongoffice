cmake_minimum_required(VERSION 3.16)
project(loongoffice_pdf_reader LANGUAGES CXX)

# Standalone contracts, deliberately independent of LibreOffice's gbuild and SDK.
# The renderer implementation and session scheduler will be separate targets.
add_library(pdf_reader_contracts INTERFACE)
target_include_directories(pdf_reader_contracts INTERFACE core/include)
target_compile_features(pdf_reader_contracts INTERFACE cxx_std_17)

include(CTest)
if(BUILD_TESTING)
    add_executable(pdf_hybrid_layout_tests tests/test_host_layout.cxx)
    target_include_directories(pdf_hybrid_layout_tests PRIVATE ../sd/source/ui/inc)
    target_compile_features(pdf_hybrid_layout_tests PRIVATE cxx_std_17)
    add_test(NAME pdf_hybrid_layout COMMAND pdf_hybrid_layout_tests)
endif()
