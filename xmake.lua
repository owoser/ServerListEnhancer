add_rules("mode.debug", "mode.release")

-- 默认使用官方仓库；设置环境变量 LEVIMC_LOCAL_REPO 可指向本机已有克隆（离线构建用）
local levimcRepoUrl = os.getenv("LEVIMC_LOCAL_REPO")
if levimcRepoUrl and os.isdir(levimcRepoUrl) then
    add_repositories("levimc-repo " .. levimcRepoUrl)
else
    add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")
end

-- server / client 由配置切换；两端共用 src/common，各自编译 src-server 或 src-client
option("target_type")
    set_default("server")
    set_showmenu(true)
    set_description("Build target side: server or client")
    set_values("server", "client")
option_end()

-- LeviLamina 版本可切换：默认 26.60.7（与常见客户端一致），可用 --ll_version=26.60.2 指定
option("ll_version")
    set_default("26.60.7")
    set_showmenu(true)
    set_description("LeviLamina version, e.g. 26.60.7 / 26.60.2")
option_end()

local targetType = get_config("target_type") or "server"
local llVersion  = get_config("ll_version") or "26.60.7"

add_requires("levilamina " .. llVersion, {configs = {target_type = targetType}})
add_requires("levibuildscript")
add_requires("nlohmann_json v3.12.0")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("ServerListEnhancer")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {modName = "ServerListEnhancer", modFile = "ServerListEnhancer.dll", modVersion = "1.0.0"})
    set_kind("shared")
    set_languages("c++23")
    set_symbols("debug")

    add_packages("levilamina", "nlohmann_json")
    add_syslinks("winhttp", "ws2_32", "user32")

    if is_plat("windows") then
        set_toolchains("clang-cl")
        set_exceptions("none")
        add_cxflags("/utf-8", "/EHa", "/W4")
        add_cxflags("/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204", {tools = {"clang_cl"}})
        add_cxflags(
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            {tools = {"clang_cl"}}
        )
    end

    add_includedirs("src")
    add_headerfiles("src/**.h")
    add_files("src/common/*.cpp")

    if targetType == "server" then
        add_defines("LL_PLAT_S", "SLE_SERVER")
        add_files("src-server/*.cpp")
        add_includedirs("src/common", "src-server")
    else
        add_defines("LL_PLAT_C", "SLE_CLIENT")
        add_files("src-client/*.cpp")
        remove_files("src-client/OreUIBridge.cpp", "src-client/ServerListPage.cpp")
        add_includedirs("src/common", "src-client")
        add_syslinks("windowscodecs", "ole32", "bcrypt", "crypt32")
    end