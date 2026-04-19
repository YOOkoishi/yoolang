add_rules("mode.debug", "mode.release")

target("yoolang")
    set_kind("binary")
    set_languages("c++17")
    add_includedirs("include")
    add_files("src/**.cpp")

