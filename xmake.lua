set_xmakever('3.0.1')
includes('lib/commonlibsse-ng')

set_project('Tailor')
set_version('2.3.6')
set_license('GPL-3.0')

set_languages('c++23')
set_warnings('allextra')
set_policy('package.requires_lock', true)
add_requires('nlohmann_json')
set_toolset('msvc', 'ninja')

add_rules('mode.debug', 'mode.releasedbg', 'mode.release')

-- CommonLib defaults skyrim_se, skyrim_ae, skyrim_vr all to true,
-- producing a single DLL that works on SE + AE + VR via Address Library.

target('Tailor')
    add_deps('commonlibsse-ng')
    add_packages('nlohmann_json')

    add_rules('commonlibsse-ng.plugin', {
        name        = 'Tailor',
        author      = 'ColdSun',
        description = 'An outfit and wig manager for Skyrim SE/AE/VR.',
        options = {
            address_library = true,
            struct_dependent = false
        }
    })

    add_files('src/**.cpp')
    add_headerfiles('src/**.h')

    add_includedirs(
        'src',
        '$(projectdir)'
    )

    set_pcxxheader('src/pch.h')
