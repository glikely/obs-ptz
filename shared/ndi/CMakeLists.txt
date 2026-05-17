cmake_minimum_required(VERSION 3.28...3.30)

add_library(ndi INTERFACE)
add_library(OBS::ndi ALIAS ndi)

target_sources(
  ndi
  INTERFACE
        Processing.NDI.compat.h
        Processing.NDI.deprecated.h
        Processing.NDI.DynamicLoad.h
        Processing.NDI.Find.h
        Processing.NDI.FrameSync.h
        Processing.NDI.Lib.cplusplus.h
        Processing.NDI.Lib.h
        Processing.NDI.Recv.ex.h
        Processing.NDI.Recv.h
        Processing.NDI.RecvAdvertiser.h
        Processing.NDI.RecvListener.h
        Processing.NDI.Routing.h
        Processing.NDI.Send.h
        Processing.NDI.structs.h
        Processing.NDI.utilities.h
)
target_include_directories(ndi INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
