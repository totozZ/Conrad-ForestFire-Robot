idf_component_register(SRCS "robot.cpp" "service.cpp" "dm_codec.cpp" INCLUDE_DIRS "include" REQUIRES json)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
