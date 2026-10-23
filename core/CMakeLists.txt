# core/CMakeLists.txt — 共识内核（静态库 + 三个可执行程序）

# 1) 内核静态库：密码学、JSON、共识状态机
add_library(chaincore STATIC
    src/crypto.cpp
    src/json.cpp
    src/vm.cpp
    src/chain.cpp
)
target_include_directories(chaincore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
    # CryptGenRandom（密钥随机源）
    target_link_libraries(chaincore PUBLIC advapi32)
endif()

# 2) 共识引擎进程（被 Python 节点服务通过 stdin/stdout 驱动）
add_executable(chain-engine src/engine.cpp)
target_link_libraries(chain-engine PRIVATE chaincore)

# 3) 运维命令行（可脱离 Python 独立建链、挖矿、校验）
add_executable(chainctl cli/chainctl.cpp)
target_link_libraries(chainctl PRIVATE chaincore)

# 4) 密码学自检程序（输出测试向量，供 tests/cross_verify.js 用 OpenSSL 交叉验证）
add_executable(test_crypto tests/test_crypto.cpp)
target_link_libraries(test_crypto PRIVATE chaincore)

set_target_properties(chain-engine chainctl test_crypto PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
)

# MinGW 下静态链接运行库，生成可直接分发的独立 exe（无需 libstdc++-6.dll）
if(MINGW)
    foreach(t chain-engine chainctl test_crypto)
        target_link_options(${t} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
endif()
