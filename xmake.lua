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

target("yir-pipeline-analyzer")
    set_kind("binary")
    set_languages("c++17")
    add_includedirs("include")
    add_files("tools/yir-pipeline-analyzer/main.cpp")
    add_files("tools/yir-pipeline-analyzer/liveness_analysis.cpp")
    add_files("tools/yir-pipeline-analyzer/pipeline_diff.cpp")
    add_files("src/ast/**.cpp")
    add_files("src/mir/**.cpp")
    add_files("src/oir/**.cpp")
    add_files("src/yir/**.cpp")
    add_files("src/front/**.cpp")
    add_files("src/pass/**.cpp")
