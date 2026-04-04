add_rules("mode.debug", "mode.release")

target("bsb")
    set_kind("binary")
    set_languages("c++17")
    add_files("src/**.cpp")    

