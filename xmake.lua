add_rules("mode.debug", "mode.release")

local srcs = {
    "src/ast/**.cpp",
    "src/mir/**.cpp",
    "src/oir/**.cpp",
    "src/yir/**.cpp",
    "src/front/**.cpp",
    "src/pass/**.cpp",
}

target("compiler")
    set_kind("binary")
    set_languages("c++17")
    add_includedirs("include")
    add_files(srcs)
    add_files("src/main/main.cpp")


