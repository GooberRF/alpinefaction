if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(VPP_TOOL ${CMAKE_SOURCE_DIR}/tools/vpp.exe)
else()
    set(VPP_TOOL ${CMAKE_SOURCE_DIR}/tools/vpp)
endif()

function(add_packfile packfile_name)

    list(TRANSFORM ARGN REPLACE "([^:]*):.*" "\\1" OUTPUT_VARIABLE input_deps)
    list(TRANSFORM ARGN REPLACE "[^:]*:(.*)" "\\1" OUTPUT_VARIABLE input_paths)

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/bin/${packfile_name}
        COMMAND ${VPP_TOOL} -c ${CMAKE_BINARY_DIR}/bin/${packfile_name} ${input_paths}
        DEPENDS ${input_deps}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_custom_target(${packfile_name} ALL DEPENDS ${CMAKE_BINARY_DIR}/bin/${packfile_name})
endfunction()


add_packfile(alpinefaction.vpp
    tables/maps_af.txt
    tables/events.tbl
    tables/af_level_quirks.tbl

    # compatibility _info.tbls
    tables/pdm01_info.tbl
    tables/pdm02_info.tbl
    tables/pdm03_info.tbl
    tables/pdm04_info.tbl
    tables/pdm05_info.tbl
    tables/pdm06_info.tbl
    tables/dm_DVLIACsDesk2DDS1-1_info.tbl
    tables/dm_DVLIACsDesk2DDS_info.tbl
    tables/CTF_Jupiter3StationLO1-2_info.tbl
    tables/CTF_Jupiter3StationLO1-1_info.tbl
    tables/CTF_Jupiter3StationLO_info.tbl
    tables/dm_Jupiter3StationLI1-1_info.tbl
    tables/dm_Jupiter3StationLI_info.tbl
    tables/dm_Jupiter3Station1-1_info.tbl
    tables/dm_Jupiter3Station_info.tbl
    tables/DM_EuropaOneOutpost1-1AG_info.tbl
    tables/DM_EuropaOneOutpost1-1_info.tbl
    tables/CTF_EuropaOneOutpost_info.tbl
    tables/dm-SpecialForces_info.tbl
    tables/Dm-NukeTown2050_info.tbl

    sounds/af_achievement1.wav
    sounds/af_pinglocation1.wav
    sounds/af_hitsound1.wav
    sounds/af_killsound1.wav
    sounds/af_radmsg_000.ogg
    sounds/af_radmsg_001.ogg
    sounds/af_radmsg_002.ogg
    sounds/af_radmsg_003.ogg
    sounds/af_radmsg_004.ogg
    sounds/af_radmsg_005.ogg
    sounds/af_radmsg_006.ogg
    sounds/af_radmsg_007.ogg
    sounds/af_radmsg_008.ogg
    sounds/af_radmsg_009.ogg
    sounds/af_radmsg_010.ogg
    sounds/af_radmsg_011.ogg
    sounds/af_radmsg_012.ogg
    sounds/af_radmsg_013.ogg
    sounds/af_radmsg_014.ogg
    sounds/af_radmsg_015.ogg
    sounds/af_radmsg_016.ogg
    sounds/af_radmsg_017.ogg
    sounds/af_radmsg_018.ogg
    sounds/af_radmsg_019.ogg
    sounds/af_radmsg_020.ogg
    sounds/af_radmsg_021.ogg
    sounds/af_radmsg_022.ogg
    sounds/af_radmsg_023.ogg
    sounds/af_radmsg_024.ogg
    sounds/af_radmsg_025.ogg
    sounds/af_radmsg_026.ogg
    sounds/af_radmsg_027.ogg
    sounds/af_radmsg_028.ogg
    sounds/af_radmsg_029.ogg
    sounds/af_radmsg_030.ogg
    sounds/af_radmsg_031.ogg
    sounds/af_radmsg_032.ogg
    sounds/af_radmsg_033.ogg
    sounds/af_radmsg_034.ogg
    sounds/af_radmsg_035.ogg
    sounds/af_radmsg_036.ogg
    sounds/af_radmsg_037.ogg
    sounds/af_radmsg_038.ogg
    sounds/af_radmsg_039.ogg
    sounds/af_radmsg_040.ogg
    sounds/af_radmsg_041.ogg
    sounds/af_radmsg_042.ogg
    sounds/af_radmsg_043.ogg
    sounds/af_radmsg_044.ogg
    sounds/af_radmsg_045.ogg
    sounds/af_radmsg_046.ogg

    images/alpine_options_panelp.tga
    images/alpine_options_panel0.tga
    images/alpine_options_panel1.tga
    images/alpine_options_panel2.tga
    images/alpine_options_panel3.tga
    images/audio_panel.vbm
    images/ao_tab.tga
    images/ao_tab_hover.tga
    images/ao_smbut1.tga
    images/ao_smbut1_hover.tga
    images/af_wh_ctf_blue_a.tga
    images/af_wh_ctf_red_a.tga
    images/af_wh_ctf_blue_d.tga
    images/af_wh_ctf_red_d.tga
    images/af_wh_ctf_blue_s.tga
    images/af_wh_ctf_red_s.tga
    images/af_wh_mp_spawn.tga
    images/af_wh_ping1.tga
    images/af_achtrophy1.tga
    images/af_wh_koth_base_neutral.tga
    images/af_wh_koth_atk_neutral.tga
    images/af_wh_koth_def_neutral.tga
    images/af_wh_koth_atk_red.tga
    images/af_wh_koth_def_red.tga
    images/af_wh_koth_atk_blue.tga
    images/af_wh_koth_def_blue.tga
    images/af_wh_koth_base_red.tga
    images/af_wh_koth_base_blue.tga
    images/af_wh_koth_cont_neutral.tga
    images/af_wh_koth_cont_red.tga
    images/af_wh_koth_cont_blue.tga
    images/af_wh_koth_lock_neutral.tga
    images/af_wh_koth_lock_red.tga
    images/af_wh_koth_lock_blue.tga
    images/af_wh_koth_fill_red.tga
    images/af_wh_koth_fill_blue.tga
    images/af_wh_koth_ring_fade.tga
    images/afsbi_spawned.tga
    images/afsbi_spawned_1.tga
    images/afsbi_dead.tga
    images/afsbi_dead_1.tga
    images/afsbi_spec.tga
    images/afsbi_spec_1.tga
    images/afsbi_brow.tga
    images/afsbi_brow_1.tga
    images/afsbi_idle.tga
    images/afsbi_idle_1.tga
    images/scope_zoom_corner256_red.tga
    images/128blackcircle.tga
    images/ass2_scope_circle.tga
    images/reticle_0.tga
    images/reticle_rocket_0.tga
    images/reticle_rocketlock_0.tga
    images/scope_ret_0.tga
    images/reticle_1.tga
    images/reticle_rocket_1.tga
    images/reticle_rocketlock_1.tga
    images/scope_ret_1.tga
    images/ammo_bar_1.tga
    images/ammo_bar_power_1.tga
    images/ammo_signal_green_1.tga
    images/ammo_signal_red_1.tga
    images/assault_digits.vbm
    images/bullet_icon_1.tga
    images/bullet_icon_50cal_1.tga
    images/bullet_icon_556_1.tga
    images/bullet_icon_aluminum_1.tga
    images/bullet_icon_gascanister_1.tga
    images/bullet_icon_powercell_1.tga
    images/bullet_icon_rocket_1.tga
    images/bullet_icon_shotgun_1.tga
    images/gore1.tga
    images/fle_gore1.tga
    images/enviro0_1.tga
    images/enviro10_1.tga
    images/enviro20_1.tga
    images/enviro30_1.tga
    images/enviro40_1.tga
    images/enviro50_1.tga
    images/enviro60_1.tga
    images/enviro70_1.tga
    images/enviro80_1.tga
    images/enviro90_1.tga
    images/enviro100_1.tga
    images/health0_1.tga
    images/health10_1.tga
    images/health20_1.tga
    images/health30_1.tga
    images/health40_1.tga
    images/health50_1.tga
    images/health60_1.tga
    images/health70_1.tga
    images/health80_1.tga
    images/health90_1.tga
    images/health100_1.tga
    images/hud_miniflag_blue_1.vbm
    images/hud_miniflag_hilight_1.vbm
    images/hud_miniflag_red_1.vbm
    images/noclip_ammo_bar_1.tga
    images/Icon_EAX.tga
    images/Icon_Event.tga
    images/radar_dish.tga
    images/score_aflogo.tga
    images/mtl_archway04-mip1.tga
    images/pls_airlockmat01-mip2.tga
    images/consolebutton2A.tga

    fonts/biggerfont.vf
    fonts/regularfont.ttf
    fonts/boldfont.ttf

    meshes/Hendrix.v3c
    meshes/Generator_Small01.v3m
    meshes/Generator_Small02.v3m
    meshes/LavaTester01.v3m
    meshes/envirosuit1.v3m
    meshes/Vat1.v3m
    meshes/coffeesmokedtbl2.v3m
    meshes/coffeesmokedtblAlt.v3m
    meshes/meatchunk1.v3m
    meshes/meatchunk2.v3m
    meshes/meatchunk3.v3m
    meshes/meatchunk4.v3m
    meshes/meatchunk5.v3m

    standard_vs:${CMAKE_BINARY_DIR}/shaders/standard_vs.bin
    character_vs:${CMAKE_BINARY_DIR}/shaders/character_vs.bin
    transformed_vs:${CMAKE_BINARY_DIR}/shaders/transformed_vs.bin
    outline_character_vs:${CMAKE_BINARY_DIR}/shaders/outline_character_vs.bin
    outline_standard_vs:${CMAKE_BINARY_DIR}/shaders/outline_standard_vs.bin
    standard_ps:${CMAKE_BINARY_DIR}/shaders/standard_ps.bin
    ui_ps:${CMAKE_BINARY_DIR}/shaders/ui_ps.bin
    outline_ps:${CMAKE_BINARY_DIR}/shaders/outline_ps.bin
)

# Copy licensing-info.txt
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/bin/licensing-info.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/licensing-info.txt ${CMAKE_BINARY_DIR}/bin/licensing-info.txt
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/licensing-info.txt
)
add_custom_target(LicensingInfoTxt ALL DEPENDS ${CMAKE_BINARY_DIR}/bin/licensing-info.txt)

add_subdirectory(shaders)
