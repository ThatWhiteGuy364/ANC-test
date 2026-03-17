# Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

cmake_minimum_required(VERSION 3.10.2)
project("anc-lib")

find_package(oboe REQUIRED CONFIG)

add_library(anc-lib SHARED LmsEngine.cpp)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(anc-lib oboe::oboe ${log-lib} ${android-lib})
