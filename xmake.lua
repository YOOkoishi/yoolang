add_rules("mode.debug", "mode.release")

local srcs = {
    "src/ast/**.cpp",
    "src/mir/**.cpp",
    "src/oir/**.cpp",
    "src/smt/**.cpp",
    "src/yir/**.cpp",
    "src/front/**.cpp",
    "src/builtin/**.cpp",
    "src/sema/**.cpp",
    "src/pass/**.cpp",
    "src/target/**.cpp",
}

target("compiler")
    set_kind("binary")
    set_languages("c++17")
    add_includedirs("include")
    add_files(srcs)
    add_files("src/main/**.cpp")

target("smt_solver_tests")
    set_kind("static")
    set_default(false)
    set_languages("c++17")
    add_includedirs("include")
    add_files("src/smt/**.cpp")
    add_files("src/pass/CostModel.cpp")
    add_files("src/pass/SMTProof.cpp")
    add_files("test/smt/smt_solver_tests.cpp")
