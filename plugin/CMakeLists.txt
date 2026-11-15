cmake_minimum_required(VERSION 3.20)
project(g923-plugin-test C ASM)

include(ps3-self)

# Plugin de prueba minimo: solo escribe una linea en un log cuando la PS3 lo carga.
add_executable(g923_plugin_test source/plugin.c)
target_compile_features(g923_plugin_test PRIVATE c_std_11)

ps3_add_prx(g923_plugin_test
    NAME g923_plugin_test
    VERSION 1.0
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/g923_plugin_test.prx"
    SIGN)
