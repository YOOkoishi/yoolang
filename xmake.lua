add_rules("mode.debug", "mode.release")

-- 源文件列表（不含 main）
local srcs = {
    "src/ast/**.cpp",
    "src/CodeGen/**.cpp",
    "src/IR/**.cpp",
    "src/IRGen/**.cpp",
    "src/front/**.cpp",
    "src/passes/**.cpp",
}

target("yoolang")
    set_kind("binary")
    set_languages("c++17")
    add_includedirs("include")
    add_files(srcs)
    add_files("src/main/main.cpp")

target("test_ir")
    set_kind("binary")
    set_languages("c++17")
    add_includedirs("include")
    add_files(srcs)
    add_files("src/main/test_ir.cpp")

