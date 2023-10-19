project(dex2oat)

cmake_minimum_required(VERSION 3.4.1)

add_executable(dex2oat dex2oat.c)

target_link_libraries(dex2oat log)

# DEBUG_SYMBOLS_PATH 在 gradle 中定义.
# 在编译 release 版本时定义 DEBUG_SYMBOLS_PATH 变量,然后在编译完成后将调试符号文件提取出来,并将调试符号文件保存到指定目录中,以便在后续的调试过程中使用.
if (DEFINED DEBUG_SYMBOLS_PATH)
    #STATUS 为cmake内置的一种消息类型, 表示状态消息
    message(WARNING "Debug symbols will be placed at ${DEBUG_SYMBOLS_PATH}")
    #格式：add_custom_command(TARGET target_name POST_BUILD COMMAND command1 [ARGS] [args1...][COMMAND command2 [ARGS] [args2...] ...])
    # 这段代码的作用是在构建完成后将目标文件的调试符号文件提取出来，并将调试符号文件保存到指定目录中，以便在后续的调试过程中使用。
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DEBUG_SYMBOLS_PATH}/${ANDROID_ABI}
            COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:${PROJECT_NAME}>
            ${DEBUG_SYMBOLS_PATH}/${ANDROID_ABI}/${PROJECT_NAME}.debug
            COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:${PROJECT_NAME}>
            COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink ${DEBUG_SYMBOLS_PATH}/${ANDROID_ABI}/${PROJECT_NAME}.debug
            $<TARGET_FILE:${PROJECT_NAME}>)

#四个 command 的大概意思如下:
    #1. 创建目录
    #2. 保存debug信息的文件
    #3. 去除debug信息
    #4. 添加debug信息
endif()



# message 的日志查看
# 在编译的时候添加 --debug 字段输出日志信息,如果没有输出的话,可以尝试 clean 之后全量编译
# message 的 STATUS 总是不会输出,改成 WARNING 就可以输出了 ps: 不知道为什么