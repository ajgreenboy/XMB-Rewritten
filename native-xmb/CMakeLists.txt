cmake_minimum_required(VERSION 3.25)
project(RePS3Native VERSION 0.3.35 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets OpenGLWidgets Multimedia Concurrent Network)
find_package(Freetype QUIET)
if(NOT Freetype_FOUND)
    find_package(Qt6BundledFreetype CONFIG REQUIRED)
    find_package(Qt6FreetypePrivate CONFIG REQUIRED)
endif()
# zlib: Qt ships Qt6ZlibPrivate only in the Windows SDK, so fall back to the
# system zlib on other platforms. XMB_ZLIB is referenced at every link site.
find_package(Qt6ZlibPrivate CONFIG QUIET)
if(TARGET Qt6::ZlibPrivate)
    set(XMB_ZLIB Qt6::ZlibPrivate)
else()
    find_package(ZLIB REQUIRED)
    set(XMB_ZLIB ZLIB::ZLIB)
endif()
if(WIN32)
    set(XMB_WIN32_ONLY_SOURCES src/desktop_pad_win.cpp)
    set(XMB_NON_WIN32_ONLY_SOURCES "")
else()
    set(XMB_WIN32_ONLY_SOURCES "")
    set(XMB_NON_WIN32_ONLY_SOURCES src/desktop_pad_stub.cpp)
endif()
# SDL2 drives the Linux controller backend (pad.cpp __linux__); Windows reads pads through XInput/HID instead.
# SDL2 ships a CMake config on Ubuntu (libsdl2-dev), so SDL2::SDL2 is available directly.
# libpulse (pkg-config, no CMake config) is the Linux master-volume backend in host.cpp __linux__.
if(NOT WIN32)
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
endif()

# Rust firmware-crypto (byte-parity reimplementation of src/firmware_crypto.cpp).
# Off by default: the C++ crypto in src/firmware_crypto.cpp stays the shipped
# implementation everywhere. When ON, the crate (native-xmb/rust-edge/firmware-crypto)
# is built with a rustup-installed cargo (toolchain >= 1.89, see the crate README),
# its staticlib is linked into the app next to a thin shim, and a parity test target
# asserts byte-identical output against the C++ reference. This option does NOT switch
# the app onto the Rust path.
option(XMB_RUST_FIRMWARE_CRYPTO "Build and link the Rust firmware-crypto staticlib (byte-parity with src/firmware_crypto.cpp)" OFF)
set(XMB_RUST_CRYPTO_SOURCES "")
if(XMB_RUST_FIRMWARE_CRYPTO)
    find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin")
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR "XMB_RUST_FIRMWARE_CRYPTO requires a rustup-installed cargo (toolchain >= 1.89); see native-xmb/rust-edge/firmware-crypto/README.md")
    endif()
    set(XMB_RUST_CRYPTO_CRATE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust-edge/firmware-crypto")
    # cargo output is kept in the CMake binary dir so the source tree stays clean.
    set(XMB_RUST_CRYPTO_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-firmware-crypto")
    set(XMB_RUST_CRYPTO_STATICLIB "${XMB_RUST_CRYPTO_TARGET_DIR}/release/libfirmware_crypto.a")
    add_custom_command(
        OUTPUT "${XMB_RUST_CRYPTO_STATICLIB}"
        COMMAND "${CARGO_EXECUTABLE}" build --release
                --manifest-path "${XMB_RUST_CRYPTO_CRATE_DIR}/Cargo.toml"
                --target-dir "${XMB_RUST_CRYPTO_TARGET_DIR}"
        WORKING_DIRECTORY "${XMB_RUST_CRYPTO_CRATE_DIR}"
        DEPENDS
            "${XMB_RUST_CRYPTO_CRATE_DIR}/Cargo.toml"
            "${XMB_RUST_CRYPTO_CRATE_DIR}/Cargo.lock"
            "${XMB_RUST_CRYPTO_CRATE_DIR}/src/lib.rs"
            "${XMB_RUST_CRYPTO_CRATE_DIR}/src/crypto.rs"
            "${XMB_RUST_CRYPTO_CRATE_DIR}/src/ffi.rs"
        COMMENT "Building the Rust firmware-crypto staticlib (cargo build --release)"
        VERBATIM)
    add_custom_target(xmb-rust-firmware-crypto DEPENDS "${XMB_RUST_CRYPTO_STATICLIB}")
    # Imported static library: consumers link the .a and pick up the C ABI header.
    add_library(xmb_firmware_crypto_rust STATIC IMPORTED GLOBAL)
    set_target_properties(xmb_firmware_crypto_rust PROPERTIES
        IMPORTED_LOCATION "${XMB_RUST_CRYPTO_STATICLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${XMB_RUST_CRYPTO_CRATE_DIR}/include"
        INTERFACE_LINK_LIBRARIES "pthread;dl;m")
    set(XMB_RUST_CRYPTO_SOURCES src/firmware_crypto_rust.cpp src/firmware_crypto_rust.h)
endif()
qt_add_executable(re-ps3-native
    src/main.cpp src/assets.cpp src/assets.h src/p3t.cpp src/p3t.h src/qrc.cpp src/qrc.h src/catalog.cpp src/catalog.h
    src/gtf_decode.cpp src/gtf_decode.h
    src/edge_decode.cpp src/edge_decode.h
    src/raf_render.cpp src/raf_render.h
    src/vsmx_eval.cpp src/vsmx_eval.h
    src/rco_objects.cpp src/rco_objects.h
    src/osk_layout.cpp src/osk_layout.h
    src/xmbml_view.cpp src/xmbml_view.h
    src/preferences.cpp src/preferences.h
    src/release_index.cpp src/release_index.h src/real_debrid.cpp src/real_debrid.h src/release_source.cpp src/release_source.h src/flare_solverr.cpp src/flare_solverr.h src/hydra_library.cpp src/hydra_library.h src/hydra_catalogue_cache.cpp src/hydra_catalogue_cache.h src/async_identity.h src/archive_extract.cpp src/archive_extract.h src/pending_installers.cpp src/pending_installers.h
    src/phone_remote.cpp src/phone_remote.h src/phone_remote_shell.cpp
    src/media_library.cpp src/media_library.h src/settings_menu.cpp
    src/playback_queue.cpp src/playback_queue.h
    src/musicvis.cpp src/musicvis.h
    src/playlists.cpp src/playlists.h
    src/save_data.cpp src/save_data.h
    src/spotify_client.cpp src/spotify_client.h src/spotify_shell.cpp
    src/discord_client.cpp src/discord_client.h
    src/glyphs.cpp src/glyphs.h
    src/dlna_client.cpp src/dlna_client.h src/dlna_shell.cpp
    src/steam_activity.cpp src/steam_activity.h
    src/service_icons.cpp src/service_icons.h src/jellyfin_icon_data.cpp
    src/service_installer.cpp src/service_installer.h
    src/tailscale_client.cpp src/tailscale_client.h src/tailscale_shell.cpp
    src/system_power.cpp src/system_power.h
    src/boot_prefetch.cpp src/boot_prefetch.h
    src/file_ops.cpp src/file_ops.h
    src/osk_paste.cpp src/osk_paste.h src/osk_predict.h
    src/zi8_dictionary.cpp src/zi8_dictionary.h
    src/folder_browser.cpp src/folder_browser.h src/folder_browse_shell.cpp
    src/weather_client.cpp src/weather_client.h src/weather_shell.cpp
    src/self_check.cpp src/self_check.h src/system_check_shell.cpp
    src/updater.cpp src/updater.h src/updater_shell.cpp
    src/firmware_fetcher.cpp src/firmware_fetcher.h src/firmware_install.cpp src/firmware_install.h
    src/firmware_extract.cpp src/firmware_extract.h src/firmware_crypto.cpp src/firmware_crypto.h ${XMB_RUST_CRYPTO_SOURCES}
    src/wifi_client.cpp src/wifi_client.h src/wifi_shell.cpp
    src/bluetooth_client.cpp src/bluetooth_client.h src/bluetooth_shell.cpp
    src/wol_client.cpp src/wol_client.h src/homeassistant_client.cpp src/homeassistant_client.h src/home_shell.cpp
    src/psn_pages.h src/psn_shell.cpp src/notification_toast.h src/friends_ui.h
    src/photo_viewport.cpp src/photo_viewport.h
    src/gallery_params.cpp src/gallery_params.h src/gallery_model.cpp src/gallery_model.h src/gallery_thumbs.cpp src/gallery_thumbs.h
    src/gallery_controller.cpp src/gallery_controller.h src/gallery_floor.h src/gallery_scene.cpp src/gallery_scene.h src/gallery_chrome.cpp src/gallery_view.h src/gallery_replay.cpp
    src/interaction_audio.cpp src/interaction_audio.h
    src/notify_overlay.cpp src/notify_overlay.h
    src/launch_cover.cpp src/launch_cover.h src/launch_fade.h
    src/live_overlay.cpp src/live_overlay.h
    src/text_renderer.cpp src/text_renderer.h
    src/font_raster.cpp src/font_raster.h
    src/profiles.cpp src/profiles.h
    src/steam_library.cpp src/steam_library.h src/steam_vdf.cpp src/steam_vdf.h src/steam_quit.cpp src/steam_quit.h src/installer_panel.cpp src/installer_panel.h src/steam_hidden.cpp src/steam_hidden.h src/steamcmd.cpp src/steamcmd.h src/steamcmd_shell.cpp
    src/steam_downloads.cpp src/steam_downloads.h src/steam_accounts.cpp src/steam_accounts.h src/steam_switch.cpp src/steam_switch.h src/steam_friends.cpp src/steam_friends.h
    src/steam_artwork.cpp src/steam_artwork.h src/steam_store.cpp src/steam_store.h src/steam_owned.cpp src/steam_owned.h src/steam_collections.cpp src/steam_collections.h src/steam_overrides.cpp src/steam_overrides.h src/steam_family.cpp src/steam_family.h
    src/steam_appinfo.cpp src/steam_appinfo.h src/steam_keyless.cpp src/steam_keyless.h src/steam_account_state.cpp src/steam_account_state.h src/steam_account_shell.cpp
    src/content_registry.cpp src/content_registry.h src/web_launch.cpp src/web_launch.h src/app_editor.cpp src/app_editor.h
    src/rawg_client.cpp src/rawg_client.h src/rawg_artwork.cpp src/rawg_artwork.h
    src/imported_art_match.cpp src/imported_art_match.h src/imported_art.cpp src/imported_art.h
    src/logo_tiles.cpp src/logo_tiles.h src/game_badge.h
    src/artwork_settings.cpp src/artwork_settings.h
    src/api_keys.cpp src/api_keys.h src/api_keys_shell.cpp
    src/media_controls.cpp src/media_controls.h
    src/media_keys.cpp src/media_keys.h
    src/subtitles.cpp src/subtitles.h
    src/image_subtitles.cpp src/image_subtitles.h
    src/jellyfin_client.cpp src/jellyfin_client.h src/jellyfin_stream.cpp src/jellyfin_stream.h
    src/seerr_client.cpp src/seerr_client.h src/seerr_shell.cpp src/seerr_config.cpp src/seerr_config.h
    src/sabnzbd_client.cpp src/sabnzbd_client.h
    src/jellyfin_player.cpp src/jellyfin_player.h src/jellyfin_shell.cpp src/jellyfin_music_shell.cpp src/jellyfin_setup.cpp src/jellyfin_setup.h src/jellyfin_setup_shell.cpp
    src/rsx_program.cpp src/rsx_program.h src/ribbon.cpp src/ribbon.h src/uniform_cache.cpp src/uniform_cache.h src/render_gate.cpp src/render_gate.h src/idle_profile.h src/frame_trace.h src/hdr.cpp src/hdr.h src/icons.cpp src/icons.h src/particles.cpp src/particles.h src/coldboot.cpp src/coldboot.h src/gameboot_background.h src/artwork_backdrop.h src/boot_wordmark.h
    src/setup.cpp src/setup.h src/setup_shell.cpp src/release_shell.cpp src/release_jobs.cpp src/game_details.cpp src/add_game_shell.cpp
    src/autostart.cpp src/autostart.h src/autologon.cpp src/autologon.h src/cec.cpp src/cec.h src/display_mode.cpp src/display_mode.h src/display_spec.h src/diagnostics.cpp src/diagnostics.h src/game_capture.cpp src/game_capture.h src/game_tap.cpp src/game_tap.h src/ingame_backdrop.h src/elevation.cpp src/elevation.h src/text_entry.cpp src/text_entry.h src/shell.cpp src/shell.h src/host.cpp src/host.h src/pad.cpp src/pad.h src/bd_remote.h src/ps_watcher.cpp src/ps_watcher.h src/web_adopt.cpp src/web_adopt.h src/desktop_mode.cpp src/desktop_mode.h src/desktop_pad.cpp ${XMB_WIN32_ONLY_SOURCES} ${XMB_NON_WIN32_ONLY_SOURCES} src/desktop_pad.h src/view.cpp src/view.h src/dialogs.cpp)
target_link_libraries(re-ps3-native PRIVATE Qt6::Widgets Qt6::OpenGLWidgets Qt6::Multimedia Qt6::Concurrent Qt6::Network)
target_link_libraries(re-ps3-native PRIVATE ${XMB_ZLIB})
if(XMB_RUST_FIRMWARE_CRYPTO)
    target_link_libraries(re-ps3-native PRIVATE xmb_firmware_crypto_rust)
    add_dependencies(re-ps3-native xmb-rust-firmware-crypto)
endif()
qt_add_resources(re-ps3-native seerr_icon PREFIX "/service-icons" BASE "src" FILES "src/seerr.png" "src/seerr-icon-LICENSE.txt")
# The app reports this version to the updater; set it only in project(VERSION ...) above.
target_compile_definitions(re-ps3-native PRIVATE XMB_APP_VERSION="${PROJECT_VERSION}")
# Env-gated test hooks are compiled out of release binaries. The only current hook
# injects a loopback-only download job for the pending-installer completion test.
option(XMB_TEST_HOOKS "Enable env-gated test hooks in the app (loopback-only download injection)" OFF)
if(XMB_TEST_HOOKS)
    target_compile_definitions(re-ps3-native PRIVATE XMB_TEST_HOOKS)
endif()
option(XMB_FIRMWARE_BUILD_TESTS "Build firmware synthetic fixture tests" OFF)
option(XMB_RELEASE_BUILD_TESTS "Build release index and source backend fixture tests" OFF)
option(XMB_PENDING_INSTALLER_BUILD_TESTS "Build pending installer fixture tests" OFF)
if(XMB_PENDING_INSTALLER_BUILD_TESTS)
    qt_add_executable(xmb-pending-installer-tests tests/pending_installer_tests.cpp src/pending_installers.cpp src/pending_installers.h src/archive_extract.cpp src/archive_extract.h src/content_registry.cpp src/content_registry.h src/web_launch.cpp src/web_launch.h)
    target_include_directories(xmb-pending-installer-tests PRIVATE src)
    target_link_libraries(xmb-pending-installer-tests PRIVATE Qt6::Core)
endif()
if(XMB_RELEASE_BUILD_TESTS)
    qt_add_executable(xmb-release-tests tests/release_tests.cpp src/release_index.cpp src/real_debrid.cpp src/release_source.cpp src/flare_solverr.cpp src/flare_solverr.h src/hydra_library.cpp src/hydra_library.h src/hydra_catalogue_cache.cpp src/hydra_catalogue_cache.h src/preferences.cpp src/preferences.h src/jellyfin_player.cpp src/jellyfin_player.h)
    target_include_directories(xmb-release-tests PRIVATE src)
    target_link_libraries(xmb-release-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent)
endif()
if(XMB_FIRMWARE_BUILD_TESTS)
    qt_add_executable(xmb-firmware-tests tests/firmware_tests.cpp src/firmware_install.cpp src/firmware_extract.cpp src/firmware_crypto.cpp)
    target_include_directories(xmb-firmware-tests PRIVATE src)
    target_compile_definitions(xmb-firmware-tests PRIVATE XMB_FIRMWARE_NO_EMBEDDED_KEYS)
    target_link_libraries(xmb-firmware-tests PRIVATE Qt6::Core ${XMB_ZLIB})
endif()

# Rust firmware-crypto byte-parity gate (needs a Rust toolchain; see XMB_RUST_FIRMWARE_CRYPTO).
if(XMB_RUST_FIRMWARE_CRYPTO)
    qt_add_executable(xmb-firmware-rust-parity-tests tests/firmware_crypto_rust_parity_tests.cpp
        src/firmware_crypto.cpp src/firmware_crypto_rust.cpp)
    target_include_directories(xmb-firmware-rust-parity-tests PRIVATE src)
    target_link_libraries(xmb-firmware-rust-parity-tests PRIVATE Qt6::Core xmb_firmware_crypto_rust)
    add_dependencies(xmb-firmware-rust-parity-tests xmb-rust-firmware-crypto)
endif()
qt_add_executable(xmb-updater updater/updater_main.cpp src/updater.cpp src/updater.h)
target_include_directories(xmb-updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(xmb-updater PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Network)
target_compile_definitions(xmb-updater PRIVATE XMB_APP_VERSION="${PROJECT_VERSION}")
option(XMB_UPDATER_BUILD_TESTS "Build the updater fixture runner" OFF)
if(XMB_UPDATER_BUILD_TESTS)
    qt_add_executable(xmb-updater-tests tests/updater_tests.cpp src/updater.cpp src/updater.h)
    target_include_directories(xmb-updater-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-updater-tests PRIVATE Qt6::Core Qt6::Network)
endif()
option(XMB_SETUP_BUILD_TESTS "Build the first-run setup test runner" OFF)
if(XMB_SETUP_BUILD_TESTS)
    qt_add_executable(xmb-setup-tests tests/setup_tests.cpp src/setup.cpp src/setup.h)
    target_include_directories(xmb-setup-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-setup-tests PRIVATE Qt6::Core)
endif()
option(XMB_AUTOLOGON_BUILD_TESTS "Build the automatic sign-in mapping tests" OFF)
if(XMB_AUTOLOGON_BUILD_TESTS)
    qt_add_executable(xmb-autologon-tests tests/autologon_tests.cpp src/autologon.cpp src/autologon.h)
    target_include_directories(xmb-autologon-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-autologon-tests PRIVATE Qt6::Core)
endif()
option(XMB_FRAME_PACER_BUILD_TESTS "Build the frame pacer (animation clock) tests" OFF)
if(XMB_FRAME_PACER_BUILD_TESTS)
    add_executable(xmb-frame-pacer-tests tests/frame_pacer_tests.cpp)
    target_include_directories(xmb-frame-pacer-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
option(XMB_GAMEBOOT_BACKGROUND_BUILD_TESTS "Build the game-boot background preset schedule/blend tests" OFF)
if(XMB_GAMEBOOT_BACKGROUND_BUILD_TESTS)
    add_executable(xmb-gameboot-background-tests tests/gameboot_background_tests.cpp src/gameboot_background.h)
    target_include_directories(xmb-gameboot-background-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
option(XMB_ARTWORK_BACKDROP_BUILD_TESTS "Build the PIC1 backdrop dwell/fade timing tests" OFF)
if(XMB_ARTWORK_BACKDROP_BUILD_TESTS)
    add_executable(xmb-artwork-backdrop-tests tests/artwork_backdrop_tests.cpp src/artwork_backdrop.h)
    target_include_directories(xmb-artwork-backdrop-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
option(XMB_BOOT_WORDMARK_BUILD_TESTS "Build the Sony Computer Entertainment boot-stage timing/placement tests" OFF)
if(XMB_BOOT_WORDMARK_BUILD_TESTS)
    add_executable(xmb-boot-wordmark-tests tests/boot_wordmark_tests.cpp src/boot_wordmark.h)
    target_include_directories(xmb-boot-wordmark-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
option(XMB_GALLERY_FLOOR_BUILD_TESTS "Build the Photo Gallery floor brightness level tests" OFF)
if(XMB_GALLERY_FLOOR_BUILD_TESTS)
    add_executable(xmb-gallery-floor-tests tests/gallery_floor_tests.cpp src/gallery_floor.h)
    target_include_directories(xmb-gallery-floor-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
option(XMB_LAUNCH_FADE_BUILD_TESTS "Build the launch cover state-machine tests and the stand-in game the end-to-end run launches (work/launch-fade-e2e.ts)" OFF)
if(XMB_LAUNCH_FADE_BUILD_TESTS)
    add_executable(xmb-launch-fade-tests tests/launch_fade_tests.cpp src/launch_fade.h)
    target_include_directories(xmb-launch-fade-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(xmb-launch-fade-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    if(WIN32)
        add_executable(xmb-launch-fade-standin WIN32 tests/launch_fade_standin.cpp)
        target_link_libraries(xmb-launch-fade-standin PRIVATE user32 gdi32 shell32 dwmapi)
        if(MSVC)
            target_compile_options(xmb-launch-fade-standin PRIVATE /W4 /permissive- /utf-8)
            target_compile_definitions(xmb-launch-fade-standin PRIVATE UNICODE _UNICODE)
        endif()
    endif()
endif()
option(XMB_INGAME_BACKDROP_BUILD_TESTS "Build the in-game XMB backdrop decision tests (blank-frame rejection, title-window latch, live vs frozen)" OFF)
if(XMB_INGAME_BACKDROP_BUILD_TESTS)
    add_executable(xmb-ingame-backdrop-tests tests/ingame_backdrop_tests.cpp src/ingame_backdrop.h)
    target_include_directories(xmb-ingame-backdrop-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(xmb-ingame-backdrop-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_VIDEO_PANEL_BUILD_TESTS "Build the video player control panel model (slot order, focus walk, Go To, Screen Mode) tests" OFF)
if(XMB_VIDEO_PANEL_BUILD_TESTS)
    add_executable(xmb-video-panel-tests tests/video_panel_tests.cpp src/video_panel.h)
    target_include_directories(xmb-video-panel-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(xmb-video-panel-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_PLAYER_CHROME_BUILD_TESTS "Build the video player loading-card and chrome legibility decision tests" OFF)
if(XMB_PLAYER_CHROME_BUILD_TESTS)
    add_executable(xmb-player-chrome-tests tests/player_chrome_tests.cpp src/player_chrome.h)
    target_include_directories(xmb-player-chrome-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(xmb-player-chrome-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_STORE_WAVE_BUILD_TESTS "Build the legacy Store wave material (MNU extraction, HSV, crest geometry) tests" OFF)
if(XMB_STORE_WAVE_BUILD_TESTS)
    qt_add_executable(xmb-store-wave-tests tests/store_wave_tests.cpp src/store_wave.cpp src/store_wave.h)
    target_include_directories(xmb-store-wave-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-store-wave-tests PRIVATE Qt6::Core Qt6::Gui)
    if(MSVC)
        target_compile_options(xmb-store-wave-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_FILE_OPS_BUILD_TESTS "Build the Delete and Copy refusal tests" OFF)
if(XMB_FILE_OPS_BUILD_TESTS)
    qt_add_executable(xmb-file-ops-tests tests/file_ops_tests.cpp src/file_ops.cpp src/file_ops.h)
    target_include_directories(xmb-file-ops-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-file-ops-tests PRIVATE Qt6::Core)
    if(WIN32)
        target_link_libraries(xmb-file-ops-tests PRIVATE ole32 shell32)
    endif()
endif()
option(XMB_OSK_PASTE_BUILD_TESTS "Build the on-screen keyboard clipboard paste (sanitising and limit) tests" OFF)
if(XMB_OSK_PASTE_BUILD_TESTS)
    qt_add_executable(xmb-osk-paste-tests tests/osk_paste_tests.cpp src/osk_paste.cpp src/osk_paste.h)
    target_include_directories(xmb-osk-paste-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-osk-paste-tests PRIVATE Qt6::Core)
endif()
option(XMB_OSK_PAD_BUILD_TESTS "Build the on-screen keyboard pad extras (L1/R1/L2/R2/SELECT, page and fade) tests" OFF)
if(XMB_OSK_PAD_BUILD_TESTS)
    qt_add_executable(xmb-osk-pad-tests tests/osk_pad_input_tests.cpp src/osk_pad_input.h src/osk_layout.h src/osk_accent_rows.h)
    target_include_directories(xmb-osk-pad-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-osk-pad-tests PRIVATE Qt6::Core Qt6::Gui)
endif()
option(XMB_OSK_COMPOSITION_BUILD_TESTS "Build the on-screen keyboard composition-word (typed-word underline) tests" OFF)
if(XMB_OSK_COMPOSITION_BUILD_TESTS)
    qt_add_executable(xmb-osk-composition-tests tests/osk_composition_tests.cpp src/osk_composition.h)
    target_include_directories(xmb-osk-composition-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-osk-composition-tests PRIVATE Qt6::Core)
endif()
option(XMB_ZI8_BUILD_TESTS "Build the Zi8 predictive-text dictionary reader tests (synthetic dictionaries; XMB_DEV_FLASH opts into the real file)" OFF)
if(XMB_ZI8_BUILD_TESTS)
    add_executable(xmb-zi8-tests tests/zi8_dictionary_tests.cpp src/zi8_dictionary.cpp src/zi8_dictionary.h src/osk_predict.h)
    target_include_directories(xmb-zi8-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
option(XMB_BD_REMOTE_BUILD_TESTS "Build the PS3 BD Remote report parser, key table and mapping tests (synthetic reports)" OFF)
if(XMB_BD_REMOTE_BUILD_TESTS)
    add_executable(xmb-bd-remote-tests tests/bd_remote_tests.cpp src/bd_remote.h)
    target_include_directories(xmb-bd-remote-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(xmb-bd-remote-tests PRIVATE cxx_std_20)
endif()
option(XMB_PS_PAD_STATUS_BUILD_TESTS "Build the PS-menu controller panel and confirm-hint decision tests" OFF)
if(XMB_PS_PAD_STATUS_BUILD_TESTS)
    qt_add_executable(xmb-ps-pad-status-tests tests/ps_pad_status_tests.cpp src/ps_pad_status.h src/pad.h)
    target_include_directories(xmb-ps-pad-status-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-ps-pad-status-tests PRIVATE Qt6::Core)
endif()
option(XMB_RAF_BUILD_TESTS "Build the dynamic-theme (RAF) GTF, Edge and VSMX decoder tests (synthetic fixtures)" OFF)
if(XMB_RAF_BUILD_TESTS)
    qt_add_executable(xmb-raf-tests tests/raf_tests.cpp src/gtf_decode.cpp src/gtf_decode.h src/edge_decode.cpp src/edge_decode.h
        src/vsmx_eval.cpp src/vsmx_eval.h)
    target_include_directories(xmb-raf-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-raf-tests PRIVATE Qt6::Core Qt6::Gui)
endif()
option(XMB_FOLDER_BROWSER_BUILD_TESTS "Build the in-XMB folder browser listing and navigation tests" OFF)
if(XMB_FOLDER_BROWSER_BUILD_TESTS)
    qt_add_executable(xmb-folder-browser-tests tests/folder_browser_tests.cpp src/folder_browser.cpp src/folder_browser.h)
    target_include_directories(xmb-folder-browser-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-folder-browser-tests PRIVATE Qt6::Core)
endif()
option(XMB_STEAM_VDF_BUILD_TESTS "Build the Steam KeyValues (VDF) golden tests" OFF)
if(XMB_STEAM_VDF_BUILD_TESTS)
    qt_add_executable(xmb-steam-vdf-tests tests/steam_vdf_tests.cpp
        src/steam_library.cpp src/steam_library.h src/steam_accounts.cpp src/steam_accounts.h
        src/steam_vdf.cpp src/steam_vdf.h)
    target_include_directories(xmb-steam-vdf-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(xmb-steam-vdf-tests PRIVATE XMB_VDF_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/tests/steam_vdf_golden.txt")
    target_link_libraries(xmb-steam-vdf-tests PRIVATE Qt6::Core)
endif()
option(XMB_STEAM_TRACKING_BUILD_TESTS "Build deterministic Steam lifecycle observation tests" OFF)
if(XMB_STEAM_TRACKING_BUILD_TESTS)
    qt_add_executable(xmb-steam-tracking-tests tests/steam_tracking_tests.cpp src/steam_library.cpp src/steam_library.h src/steam_vdf.cpp src/steam_vdf.h)
    target_include_directories(xmb-steam-tracking-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-steam-tracking-tests PRIVATE Qt6::Core)
endif()
option(XMB_STEAM_LINUX_BUILD_TESTS "Build the Linux Steam discovery and launch hand-off fixture tests" OFF)
if(XMB_STEAM_LINUX_BUILD_TESTS)
    qt_add_executable(xmb-steam-linux-tests tests/steam_linux_discovery_tests.cpp src/steam_library.cpp src/steam_library.h src/steam_vdf.cpp src/steam_vdf.h)
    target_include_directories(xmb-steam-linux-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-steam-linux-tests PRIVATE Qt6::Core)
endif()
option(XMB_STEAM_SWITCH_BUILD_TESTS "Build the Steam account switch tests (loginusers.vdf rewrite, file and registry writes against a scratch root)" OFF)
if(XMB_STEAM_SWITCH_BUILD_TESTS)
    qt_add_executable(xmb-steam-switch-tests tests/steam_switch_tests.cpp
        src/steam_switch.cpp src/steam_switch.h src/steam_accounts.cpp src/steam_accounts.h
        src/steam_vdf.cpp src/steam_vdf.h)
    target_include_directories(xmb-steam-switch-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-steam-switch-tests PRIVATE Qt6::Core)
endif()
option(XMB_STEAM_ACCOUNT_BUILD_TESTS "Build the Steam account selection, appinfo reader and keyless library tests" OFF)
if(XMB_STEAM_ACCOUNT_BUILD_TESTS)
    qt_add_executable(xmb-steam-account-tests tests/steam_account_tests.cpp
        src/steam_library.cpp src/steam_library.h src/steam_vdf.cpp src/steam_vdf.h
        src/steam_appinfo.cpp src/steam_appinfo.h src/steam_keyless.cpp src/steam_keyless.h
        src/steam_collections.cpp src/steam_collections.h src/steam_activity.cpp src/steam_activity.h
        src/steam_family.cpp src/steam_family.h src/steam_accounts.cpp src/steam_accounts.h
        src/steam_account_state.cpp src/steam_account_state.h src/steam_refresh.h src/steam_switch.h)
    target_include_directories(xmb-steam-account-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-steam-account-tests PRIVATE Qt6::Core)
endif()
option(XMB_DISCORD_BUILD_TESTS "Build the Discord sign-in message and discord.json tests" OFF)
if(XMB_DISCORD_BUILD_TESTS)
    qt_add_executable(xmb-discord-tests tests/discord_tests.cpp src/discord_client.cpp src/discord_client.h)
    target_include_directories(xmb-discord-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(xmb-discord-tests PRIVATE XMB_DISCORD_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/examples/discord.json")
    target_link_libraries(xmb-discord-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
    if(WIN32)
        target_link_libraries(xmb-discord-tests PRIVATE crypt32)
    endif()
endif()
option(XMB_IMPORTED_ART_BUILD_TESTS "Build the imported-game artwork (Steam store title matching, cache metadata) tests" OFF)
if(XMB_IMPORTED_ART_BUILD_TESTS)
    qt_add_executable(xmb-imported-art-tests tests/imported_art_tests.cpp src/imported_art_match.cpp src/imported_art_match.h)
    target_include_directories(xmb-imported-art-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-imported-art-tests PRIVATE Qt6::Core Qt6::Gui)
    if(MSVC)
        target_compile_options(xmb-imported-art-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_LOGO_TILES_BUILD_TESTS "Build the PS3 logo-tile tests (logo path resolution, art fallback choice, trim and aspect fit)" OFF)
if(XMB_LOGO_TILES_BUILD_TESTS)
    qt_add_executable(xmb-logo-tiles-tests tests/logo_tiles_tests.cpp src/logo_tiles.cpp src/logo_tiles.h)
    target_include_directories(xmb-logo-tiles-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-logo-tiles-tests PRIVATE Qt6::Core Qt6::Gui)
    if(MSVC)
        target_compile_options(xmb-logo-tiles-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_GAME_BADGE_BUILD_TESTS "Build the Game column PS3 badge helper tests (draw rule, date line, layout)" OFF)
if(XMB_GAME_BADGE_BUILD_TESTS)
    qt_add_executable(xmb-game-badge-tests tests/game_badge_tests.cpp src/game_badge.h)
    target_include_directories(xmb-game-badge-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-game-badge-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(xmb-game-badge-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_API_KEYS_BUILD_TESTS "Build the API Keys tests (key format validation, status derivation, atomic write/clear helpers)" OFF)
if(XMB_API_KEYS_BUILD_TESTS)
    qt_add_executable(xmb-api-keys-tests tests/api_keys_tests.cpp src/api_keys.cpp src/api_keys.h)
    target_include_directories(xmb-api-keys-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-api-keys-tests PRIVATE Qt6::Core)
    if(WIN32)
        target_link_libraries(xmb-api-keys-tests PRIVATE advapi32)
    endif()
    if(MSVC)
        target_compile_options(xmb-api-keys-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_PSN_PAGES_BUILD_TESTS "Build the PlayStation Network column page tests (sign-in form logic, Account Management tree, truthful wording)" OFF)
if(XMB_PSN_PAGES_BUILD_TESTS)
    qt_add_executable(xmb-psn-pages-tests tests/psn_pages_tests.cpp src/psn_pages.h)
    target_include_directories(xmb-psn-pages-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-psn-pages-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(xmb-psn-pages-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_NOTIFICATION_TOAST_BUILD_TESTS "Build the info-bar message log and notification toast geometry tests" OFF)
if(XMB_NOTIFICATION_TOAST_BUILD_TESTS)
    qt_add_executable(xmb-notification-toast-tests tests/notification_toast_tests.cpp src/notification_toast.h)
    target_include_directories(xmb-notification-toast-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-notification-toast-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(xmb-notification-toast-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_FRIENDS_UI_BUILD_TESTS "Build the Friends column visual-rule tests (avatar tile, status dot, speech bubble gating, options rows)" OFF)
if(XMB_FRIENDS_UI_BUILD_TESTS)
    qt_add_executable(xmb-friends-ui-tests tests/friends_ui_tests.cpp src/friends_ui.h)
    target_include_directories(xmb-friends-ui-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-friends-ui-tests PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(xmb-friends-ui-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_RAWG_BUILD_TESTS "Build the RAWG client game-detail parser fixture tests (synthetic fixture, no network)" OFF)
if(XMB_RAWG_BUILD_TESTS)
    qt_add_executable(xmb-rawg-tests tests/rawg_tests.cpp src/rawg_client.cpp src/rawg_client.h)
    target_include_directories(xmb-rawg-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(xmb-rawg-tests PRIVATE XMB_RAWG_DETAILS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/rawg_game_details_fixture.json")
    target_link_libraries(xmb-rawg-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent)
    if(MSVC)
        target_compile_options(xmb-rawg-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_JELLYFIN_SETUP_BUILD_TESTS "Build the Jellyfin setup (address, discovery reply, apps.json save) tests" OFF)
if(XMB_JELLYFIN_SETUP_BUILD_TESTS)
    qt_add_executable(xmb-jellyfin-setup-tests tests/jellyfin_setup_tests.cpp
        src/jellyfin_setup.cpp src/jellyfin_setup.h src/content_registry.cpp src/content_registry.h src/web_launch.cpp src/web_launch.h)
    target_include_directories(xmb-jellyfin-setup-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-jellyfin-setup-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
    if(MSVC)
        target_compile_options(xmb-jellyfin-setup-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_JELLYFIN_PLAYBACK_BUILD_TESTS "Build the Jellyfin playback (PlaybackInfo, transcode URL, loopback HLS relay, diagnostics) tests" OFF)
if(XMB_JELLYFIN_PLAYBACK_BUILD_TESTS)
    qt_add_executable(xmb-jellyfin-playback-tests tests/jellyfin_playback_tests.cpp
        src/jellyfin_client.cpp src/jellyfin_client.h src/jellyfin_stream.cpp src/jellyfin_stream.h src/jellyfin_player.cpp src/jellyfin_player.h src/video_panel.h)
    target_include_directories(xmb-jellyfin-playback-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-jellyfin-playback-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
    if(WIN32)
        target_link_libraries(xmb-jellyfin-playback-tests PRIVATE crypt32)
    endif()
    if(MSVC)
        target_compile_options(xmb-jellyfin-playback-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_SEERR_WATCH_BUILD_TESTS "Build the Seerr->Jellyfin Watch resolution policy (pure match decision) tests" OFF)
if(XMB_SEERR_WATCH_BUILD_TESTS)
    qt_add_executable(xmb-seerr-watch-tests tests/seerr_jellyfin_match_tests.cpp)
    target_include_directories(xmb-seerr-watch-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-seerr-watch-tests PRIVATE Qt6::Core Qt6::Gui)
    if(MSVC)
        target_compile_options(xmb-seerr-watch-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_JELLYFIN_PLAYER_BUILD_TESTS "Build the Jellyfin player state-logic (position ticks, played rule, chapters, per-title choice) tests" OFF)
if(XMB_JELLYFIN_PLAYER_BUILD_TESTS)
    qt_add_executable(xmb-jellyfin-player-tests tests/jellyfin_player_tests.cpp
        src/jellyfin_player.cpp src/jellyfin_player.h src/jellyfin_client.cpp src/jellyfin_client.h src/jellyfin_stream.cpp src/jellyfin_stream.h)
    target_include_directories(xmb-jellyfin-player-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-jellyfin-player-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
    if(WIN32)
        target_link_libraries(xmb-jellyfin-player-tests PRIVATE crypt32)
    endif()
    if(MSVC)
        target_compile_options(xmb-jellyfin-player-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_SUBTITLE_BUILD_TESTS "Build the sidecar subtitle parsing, cue lookup, label and Jellyfin English-only rule tests" OFF)
if(XMB_SUBTITLE_BUILD_TESTS)
    qt_add_executable(xmb-subtitle-tests tests/subtitle_tests.cpp
        src/subtitles.cpp src/subtitles.h src/image_subtitles.cpp src/image_subtitles.h)
    target_include_directories(xmb-subtitle-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-subtitle-tests PRIVATE Qt6::Core Qt6::Gui)
    if(MSVC)
        target_compile_options(xmb-subtitle-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
option(XMB_STEAM_QUIT_BUILD_TESTS "Build the Steam Quit Game process-selection and three-stage tests (and the stand-in game they close)" OFF)
if(XMB_STEAM_QUIT_BUILD_TESTS)
    qt_add_executable(xmb-steam-quit-tests tests/steam_quit_tests.cpp src/steam_quit.cpp src/steam_quit.h)
    target_include_directories(xmb-steam-quit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-steam-quit-tests PRIVATE Qt6::Core)
    if(WIN32)
        target_link_libraries(xmb-steam-quit-tests PRIVATE user32)
        add_executable(xmb-steam-quit-standin WIN32 tests/steam_quit_standin.cpp)
        target_link_libraries(xmb-steam-quit-standin PRIVATE user32 shell32)
        if(MSVC)
            target_compile_options(xmb-steam-quit-standin PRIVATE /W4 /permissive- /utf-8)
            target_compile_options(xmb-steam-quit-tests PRIVATE /W4 /permissive- /utf-8)
        endif()
        add_dependencies(xmb-steam-quit-tests xmb-steam-quit-standin)
    endif()
endif()
option(XMB_INSTALLER_PANEL_BUILD_TESTS "Build the Installing panel tests (installer log parser, truthful status, exit classification, close escalation, process tree) and the synthetic installer stand-in they start" OFF)
if(XMB_INSTALLER_PANEL_BUILD_TESTS)
    qt_add_executable(xmb-installer-panel-tests tests/installer_panel_tests.cpp src/installer_panel.cpp src/installer_panel.h src/steam_quit.cpp src/steam_quit.h)
    target_include_directories(xmb-installer-panel-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-installer-panel-tests PRIVATE Qt6::Core)
    if(WIN32)
        target_link_libraries(xmb-installer-panel-tests PRIVATE user32)
        add_executable(xmb-installer-standin WIN32 tests/installer_standin.cpp)
        target_link_libraries(xmb-installer-standin PRIVATE user32 shell32)
        if(MSVC)
            target_compile_options(xmb-installer-standin PRIVATE /W4 /permissive- /utf-8)
            target_compile_options(xmb-installer-panel-tests PRIVATE /W4 /permissive- /utf-8)
        endif()
        add_dependencies(xmb-installer-panel-tests xmb-installer-standin)
    endif()
endif()
option(XMB_STEAM_HIDE_BUILD_TESTS "Build the Steam background-mode tests (switch set, readiness, window rules) and the stand-in steam.exe they start" OFF)
if(XMB_STEAM_HIDE_BUILD_TESTS)
    qt_add_executable(xmb-steam-hide-tests tests/steam_hide_tests.cpp src/steam_hidden.cpp src/steam_hidden.h src/steam_quit.cpp src/steam_quit.h)
    target_include_directories(xmb-steam-hide-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-steam-hide-tests PRIVATE Qt6::Core)
    if(WIN32)
        target_link_libraries(xmb-steam-hide-tests PRIVATE user32 advapi32)
        add_executable(xmb-steam-hide-standin WIN32 tests/steam_hide_standin.cpp)
        target_link_libraries(xmb-steam-hide-standin PRIVATE user32 shell32 advapi32)
        if(MSVC)
            target_compile_options(xmb-steam-hide-standin PRIVATE /W4 /permissive- /utf-8)
            target_compile_options(xmb-steam-hide-tests PRIVATE /W4 /permissive- /utf-8)
        endif()
        add_dependencies(xmb-steam-hide-tests xmb-steam-hide-standin)
    endif()
endif()
option(XMB_STEAMCMD_BUILD_TESTS "Build the SteamCMD client tests (prompt matching, progress, command lines, credential audit) and the fake steamcmd they drive" OFF)
if(XMB_STEAMCMD_BUILD_TESTS)
    qt_add_executable(xmb-steamcmd-tests tests/steamcmd_tests.cpp src/steamcmd.cpp src/steamcmd.h src/steam_vdf.cpp src/steam_vdf.h)
    target_include_directories(xmb-steamcmd-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-steamcmd-tests PRIVATE Qt6::Core)
    if(WIN32)
        add_executable(xmb-steamcmd-standin tests/steamcmd_standin.cpp)
        if(MSVC)
            target_compile_options(xmb-steamcmd-standin PRIVATE /W4 /permissive- /utf-8)
            target_compile_options(xmb-steamcmd-tests PRIVATE /W4 /permissive- /utf-8)
        endif()
        add_dependencies(xmb-steamcmd-tests xmb-steamcmd-standin)
    endif()
endif()
option(XMB_OVERLAY_BUILD_TESTS "Build the in-game live overlay stand-in game (borderless/windowed D3D11 window, never shipped)" OFF)
if(XMB_OVERLAY_BUILD_TESTS AND WIN32)
    add_executable(xmb-overlay-standin WIN32 tests/overlay_standin.cpp)
    target_link_libraries(xmb-overlay-standin PRIVATE user32 shell32 d3d11 dxgi)
    if(MSVC)
        target_compile_options(xmb-overlay-standin PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(xmb-overlay-standin PRIVATE UNICODE _UNICODE)
    endif()
endif()
option(XMB_JELLYFIN_MUSIC_BUILD_TESTS "Build the Jellyfin music (lists, paging, covers, audio direct play, error reasons) tests" OFF)
if(XMB_JELLYFIN_MUSIC_BUILD_TESTS)
    qt_add_executable(xmb-jellyfin-music-tests tests/jellyfin_music_tests.cpp
        src/jellyfin_client.cpp src/jellyfin_client.h src/jellyfin_stream.cpp src/jellyfin_stream.h)
    target_include_directories(xmb-jellyfin-music-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xmb-jellyfin-music-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
    if(WIN32)
        target_link_libraries(xmb-jellyfin-music-tests PRIVATE crypt32)
    endif()
    if(MSVC)
        target_compile_options(xmb-jellyfin-music-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
if(Freetype_FOUND)
    target_link_libraries(re-ps3-native PRIVATE Freetype::Freetype)
else()
    target_link_libraries(re-ps3-native PRIVATE Qt6::BundledFreetype Qt6::FreetypePrivate)
endif()
qt_add_executable(xmb-reference-frames src/reference_frames.cpp)
target_link_libraries(xmb-reference-frames PRIVATE Qt6::Gui Qt6::Multimedia)
if(WIN32)
    target_link_libraries(re-ps3-native PRIVATE xinput9_1_0 hid setupapi ole32 iphlpapi ws2_32 shell32 advapi32)
else()
    target_link_libraries(re-ps3-native PRIVATE SDL2::SDL2 PkgConfig::PULSE)
endif()
if(MSVC)
    target_compile_options(re-ps3-native PRIVATE /W4 /permissive- /utf-8 /Zi)
    # The env-driven debug logs and the crash handler use fopen/_wfopen deliberately; fopen_s adds nothing there.
    target_compile_definitions(re-ps3-native PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Program database for the crash handler's stack traces (main.cpp); optimisation is unchanged.
    target_link_options(re-ps3-native PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    target_link_libraries(re-ps3-native PRIVATE dbghelp windowsapp d3d11 dxgi dcomp dwmapi shell32 wlanapi)
endif()

# Refresh build-info.json after every relink of re-ps3-native, so a direct
# `cmake --build`/Ninja rebuild cannot leave the executable's metadata stale.
# The helper writes only a JSON side file (never a build input), so it cannot
# trigger another link. build.ps1 calls the same helper with the start-of-build
# snapshot; a direct build snapshots the sources at link time instead.
get_filename_component(XMB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(WIN32)
    add_custom_command(TARGET re-ps3-native POST_BUILD
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/write-build-info.ps1"
                -OutputDirectory "${CMAKE_BINARY_DIR}"
                -ProjectRoot "${XMB_REPO_ROOT}"
                -Configuration "$<CONFIG>"
        COMMENT "Refresh build-info.json"
        VERBATIM)
endif()

# XMB Manager: the standalone companion tool (CLI + Qt UI). It shares the
# output directory with re-ps3-native, so windeployqt's Qt runtime deployment
# for the main executable also serves xmb-manager.exe. The manager never
# depends on firmware and writes only to its own data directory.
qt_add_executable(xmb-manager
    manager/manager_main.cpp
    manager/manager_core.cpp manager/manager_core.h
    manager/manager_emulators.cpp manager/manager_emulators.h
    manager/manager_scanner.cpp manager/manager_scanner.h
    manager/manager_package.cpp manager/manager_package.h
    manager/manager_cli.cpp manager/manager_cli.h
    manager/manager_ui.cpp manager/manager_ui.h
    src/content_registry.cpp src/content_registry.h src/web_launch.cpp src/web_launch.h
)
target_include_directories(xmb-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/manager)
target_link_libraries(xmb-manager PRIVATE Qt6::Widgets Qt6::Gui Qt6::Core)
if(MSVC)
    target_compile_options(xmb-manager PRIVATE /W4 /permissive- /utf-8 /Zi)
    target_link_options(xmb-manager PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    target_link_libraries(xmb-manager PRIVATE dbghelp)
endif()

# Optional headless test runner for the manager core (fixtures + CLI verbs).
# Off by default so a normal build stays exactly the shipped targets.
option(XMB_MANAGER_BUILD_TESTS "Build the XMB Manager test runner" OFF)
if(XMB_MANAGER_BUILD_TESTS)
    qt_add_executable(xmb-manager-tests
        manager/tests/manager_tests.cpp
        manager/manager_core.cpp manager/manager_core.h
        manager/manager_emulators.cpp manager/manager_emulators.h
        manager/manager_scanner.cpp manager/manager_scanner.h
        manager/manager_package.cpp manager/manager_package.h
        src/content_registry.cpp src/content_registry.h src/web_launch.cpp src/web_launch.h
    )
    target_include_directories(xmb-manager-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/manager)
    target_link_libraries(xmb-manager-tests PRIVATE Qt6::Gui Qt6::Core)
    if(MSVC)
        target_compile_options(xmb-manager-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()

# Optional logged-in Steam client helper. SDK headers stay external; no Steam DLL is redistributed.
set(XMB_STEAMWORKS_HEADERS "" CACHE PATH "Steamworks public header directory containing isteamclient.h")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND XMB_STEAMWORKS_HEADERS)
    if(NOT EXISTS "${XMB_STEAMWORKS_HEADERS}/isteamclient.h")
        message(FATAL_ERROR "XMB_STEAMWORKS_HEADERS must contain the Steamworks public headers")
    endif()
    add_executable(xmb-steam-client src/steam_client_helper.cpp src/steam_library.cpp src/steam_library.h src/steam_vdf.cpp src/steam_vdf.h
    src/steam_downloads.cpp src/steam_downloads.h src/steam_accounts.cpp src/steam_accounts.h)
    target_include_directories(xmb-steam-client PRIVATE "${XMB_STEAMWORKS_HEADERS}")
    target_compile_definitions(xmb-steam-client PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(xmb-steam-client PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(xmb-steam-client PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
