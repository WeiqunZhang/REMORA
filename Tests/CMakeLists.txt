if(WIN32)
    set(FCOMPARE_EXE ${CMAKE_BINARY_DIR}/Submodules/AMReX/Tools/Plotfile/*/amrex_fcompare.exe CACHE INTERNAL "Path to fcompare executable for regression tests")
else()
    set(FCOMPARE_EXE ${CMAKE_BINARY_DIR}/Submodules/AMReX/Tools/Plotfile/amrex_fcompare CACHE INTERNAL "Path to fcompare executable for regression tests")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/CTestList.cmake)
