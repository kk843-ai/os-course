add_library(
  vtpc
  STATIC
  vtpc.c
  ../src/vtpc_impl.c
)

target_include_directories(
  vtpc
  PUBLIC
  .
  ../src
)

target_compile_features(vtpc PUBLIC c_std_11)
