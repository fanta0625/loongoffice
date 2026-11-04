cmake_minimum_required(VERSION 3.16)
project(loongoffice_pdf_reader LANGUAGES CXX)

# Standalone contracts, deliberately independent of LibreOffice's gbuild and SDK.
# The renderer implementation and session scheduler will be separate targets.
add_library(pdf_reader_contracts INTERFACE)
target_include_directories(pdf_reader_contracts INTERFACE core/include)
target_compile_features(pdf_reader_contracts INTERFACE cxx_std_17)
