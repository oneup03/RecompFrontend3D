#include <atomic>
#include <memory>
#include <cstring>
#include <variant>
#include <algorithm>

#include "hle/rt64_application.h"
#include "rt64_render_hooks.h"
#include "overloaded.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "renderer.h"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "concurrentqueue.h"

using namespace recompui;

static RT64::UserConfiguration::Antialiasing device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
static bool sample_positions_supported = false;
static bool high_precision_fb_enabled = false;

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

struct TexturePackEnableAction {
    std::string mod_id;
};

struct TexturePackDisableAction {
    std::string mod_id;
};

struct TexturePackSecondaryEnableAction {
    std::string mod_id;
};

struct TexturePackSecondaryDisableAction {
    std::string mod_id;
};

struct TexturePackUpdateAction {
};

using TexturePackAction = std::variant<TexturePackEnableAction, TexturePackDisableAction, TexturePackSecondaryEnableAction, TexturePackSecondaryDisableAction, TexturePackUpdateAction>;

static moodycamel::ConcurrentQueue<TexturePackAction> texture_pack_action_queue;

// Stereo state pushed by set_stereo_config() and consumed by RT64Context before
// the RT64 application advances a frame. Packed into a single atomic<uint64_t>
// so all four values land coherently without a mutex.
//   bits [15:0]  separation slider (0..50)
//   bits [31:16] convergence slider in tenths (1..500)
//   bits [47:32] mode (StereoMode enum value)
//   bits [63:48] HUD depth slider (0..100)
static std::atomic<uint64_t> stereo_config_packed{0};

// Everything that didn't fit the four-field uint64 above, packed into a second
// atomic. Kept separate so that one doesn't need repacking and so the runtime
// flag (which the BK side toggles per scene) can change at any frame without
// touching the user's configured values.
//   bits [7:0]   autoConvergenceScale slider (0..100)
//   bits [8]     autoConvergence on/off
//   bits [16:9]  ghostContrast slider (0..100, 100 = off)
//   bits [24:17] ghostBlackFloor slider (0..100, 0 = off)
// Initialised with ghostContrast = 100 (the no-op) rather than an all-zero
// word: a zero contrast field would decode as a full squeeze to mid-grey if
// anything read this before the host pushed its first configuration.
static std::atomic<uint32_t> stereo_auto_packed{100u << 9};

// Set by the BK side once per frame via recomp_stereo_set_low_convergence_scene.
// True when the current scene matches one of the detection patterns
// (FMV / file select / Bottles bonus / first-person view). The renderer scales
// userConfig.stereoConvergence down by auto_packed's scale when this is true
// AND auto_packed's enable bit is set. Defaults to false so existing users
// see no behavior change until they opt in.
static std::atomic<bool> stereo_runtime_low_convergence{false};

static uint64_t pack_stereo_config(RT64::UserConfiguration::StereoMode mode, uint32_t separation, uint32_t convergence, uint32_t hudDepth) {
    return (static_cast<uint64_t>(hudDepth & 0xFFFFu) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(mode) & 0xFFFFu) << 32) |
           (static_cast<uint64_t>(convergence & 0xFFFFu) << 16) |
           (static_cast<uint64_t>(separation & 0xFFFFu));
}

static void unpack_stereo_config(uint64_t packed, RT64::UserConfiguration::StereoMode &mode, uint32_t &separation, uint32_t &convergence, uint32_t &hudDepth) {
    separation = static_cast<uint32_t>(packed & 0xFFFFu);
    convergence = static_cast<uint32_t>((packed >> 16) & 0xFFFFu);
    mode = static_cast<RT64::UserConfiguration::StereoMode>((packed >> 32) & 0xFFFFu);
    hudDepth = static_cast<uint32_t>((packed >> 48) & 0xFFFFu);
}

unsigned int MI_INTR_REG = 0;

unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

void dummy_check_interrupts() {}

RT64::UserConfiguration::Antialiasing compute_max_supported_aa(plume::RenderSampleCounts bits) {
    if (bits & plume::RenderSampleCount::Bits::COUNT_2) {
        if (bits & plume::RenderSampleCount::Bits::COUNT_4) {
            if (bits & plume::RenderSampleCount::Bits::COUNT_8) {
                return RT64::UserConfiguration::Antialiasing::MSAA8X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        }
        return RT64::UserConfiguration::Antialiasing::MSAA2X;
    };
    return RT64::UserConfiguration::Antialiasing::None;
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original:
            return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:
            return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual:
            return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::OptionCount:
            return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
}

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:
            return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X:
            return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X:
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X:
            return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::OptionCount:
            return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original:
            return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:
            return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:
            return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::OptionCount:
            return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
            return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:
            return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto:
            return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::OptionCount:
            return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
}

RT64::EnhancementConfiguration::Presentation::Mode to_rt64(ultramodern::renderer::PresentationMode mode) {
    switch (mode) {
        case ultramodern::renderer::PresentationMode::Console:
            return RT64::EnhancementConfiguration::Presentation::Mode::Console;
        case ultramodern::renderer::PresentationMode::SkipBuffering:
            return RT64::EnhancementConfiguration::Presentation::Mode::SkipBuffering;
        case ultramodern::renderer::PresentationMode::PresentEarly:
            return RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    }
}

void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0/9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success:
            return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }

    fprintf(stderr, "Unhandled `RT64::Application::SetupResult` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:
            return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:
            return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:
            return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic:
            return ultramodern::renderer::GraphicsApi::Auto;
        default:
            break;
    }

    fprintf(stderr, "Unhandled `RT64::UserConfiguration::GraphicsAPI` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

renderer::RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, ultramodern::renderer::PresentationMode presentation_mode, bool debug) {
    static unsigned char dummy_rom_header[0x40];
    recompui::set_render_hooks();

    // Set up the RT64 application core fields.
    RT64::Application::Core appCore{};
#if defined(_WIN32)
    appCore.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
    appCore.window = window_handle;
#elif defined(__APPLE__)
    appCore.window.window = window_handle.window;
    appCore.window.view = window_handle.view;
#endif

    appCore.checkInterrupts = dummy_check_interrupts;

    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;

    appCore.MI_INTR_REG = &MI_INTR_REG;

    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs *vi_regs = ultramodern::renderer::get_vi_regs();
    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    // Set up the RT64 application configuration fields.
    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;

    // Create the RT64 application.
    app = std::make_unique<RT64::Application>(appCore, appConfig);

    // Set initial user config settings based on the current settings.
    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = debug;
    // Force gbi depth branches to prevent LODs from kicking in.
    app->enhancementConfig.f3dex.forceBranch = true;
    // Scale LODs based on the output resolution.
    app->enhancementConfig.textureLOD.scale = true;
    // Set the target presentation mode.
    app->enhancementConfig.presentation.mode = to_rt64(presentation_mode);
    // Pick an API if the user has set an override.
    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        case ultramodern::renderer::GraphicsApi::Metal:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
            break;
        case ultramodern::renderer::GraphicsApi::Auto:
        default:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
            break;
    }

    // Set up the RT64 application.
    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = window_handle.thread_id;
#endif
    setup_result = map_setup_result(app->setup(thread_id));
    // Get the API that RT64 chose.
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        app = nullptr;
        return;
    }

    // Set the application's fullscreen state.
    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

    // Check if the selected device actually supports MSAA sample positions and MSAA for for the formats that will be used
    // and downgrade the configuration accordingly.
    if (app->device->getCapabilities().sampleLocations) {
        plume::RenderSampleCounts color_sample_counts = app->device->getSampleCountsSupported(plume::RenderFormat::R8G8B8A8_UNORM);
        plume::RenderSampleCounts depth_sample_counts = app->device->getSampleCountsSupported(plume::RenderFormat::D32_FLOAT);
        plume::RenderSampleCounts common_sample_counts = color_sample_counts & depth_sample_counts;
        device_max_msaa = compute_max_supported_aa(common_sample_counts);
        sample_positions_supported = true;
    }
    else {
        device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
        sample_positions_supported = false;
    }

    recompui::config::graphics::update_msaa_supported(sample_positions_supported);

    high_precision_fb_enabled = app->shaderLibrary->usesHDR;
}

renderer::RT64Context::~RT64Context() = default;

static std::atomic<uint64_t> stereo_config_last_applied{UINT64_MAX};

static void apply_pending_stereo_config(RT64::Application *app) {
    if (app == nullptr) {
        return;
    }
    const uint64_t packed = stereo_config_packed.load(std::memory_order_relaxed);
    const uint32_t autoPacked = stereo_auto_packed.load(std::memory_order_relaxed);
    const bool runtimeLowConv = stereo_runtime_low_convergence.load(std::memory_order_relaxed);

    // Fold all three inputs into a single composite key so the existing
    // "skip duplicate pushes" optimization still works. The auto-convergence
    // value can change frame-to-frame independently of the user's config
    // (e.g. as the player walks in and out of first-person view), so we have
    // to detect those transitions too.
    const uint64_t compositeKey = packed
        ^ (static_cast<uint64_t>(autoPacked) << 1)
        ^ (runtimeLowConv ? 0xA55A5AA5ull : 0ull);
    if (compositeKey == stereo_config_last_applied.load(std::memory_order_relaxed)) {
        return;
    }
    RT64::UserConfiguration::StereoMode mode;
    uint32_t separation;
    uint32_t convergence;
    uint32_t hudDepth;
    unpack_stereo_config(packed, mode, separation, convergence, hudDepth);

    const bool autoConvergence = (autoPacked & (1u << 8)) != 0u;
    const uint32_t autoConvergenceScale = autoPacked & 0xFFu;
    uint32_t effectiveConvergence = convergence;
    if (autoConvergence && runtimeLowConv) {
        // Round to nearest. Clamp to >=1 so downstream code that divides by
        // convergence can't blow up.
        const uint32_t scaled = (convergence * autoConvergenceScale + 50u) / 100u;
        effectiveConvergence = std::max<uint32_t>(1u, scaled);
    }

    app->userConfig.stereoMode = mode;
    app->userConfig.stereoSeparation = separation;
    app->userConfig.stereoConvergence = effectiveConvergence;
    app->userConfig.stereoHudDepth = hudDepth;
    app->userConfig.stereoGhostContrast = (autoPacked >> 9) & 0xFFu;
    app->userConfig.stereoGhostBlackFloor = (autoPacked >> 17) & 0xFFu;
    // Propagate into sharedQueueResources->userConfig so the workload and present
    // threads see the new values. discardFBs=false: stereo doesn't change render
    // target resolution or framebuffer layout.
    app->updateUserConfig(false);
    stereo_config_last_applied.store(compositeKey, std::memory_order_relaxed);
}

void renderer::RT64Context::send_dl(const OSTask* task) {
    check_texture_pack_actions();
    apply_pending_stereo_config(app.get());
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
}

void renderer::RT64Context::send_dummy_workload(uint32_t fb_address) {
    app->state->listProcessBegin();
    app->state->rdp->setColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, fb_address);
    // G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_PERSP | G_CYC_FILL | G_PM_NPRIMITIVE
    // G_AC_NONE | G_ZS_PIXEL | G_RM_NOOP | G_RM_NOOP2
    app->state->rdp->setOtherMode(0x382C30, 0);
    app->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
    app->state->fullSync();
    app->state->listProcessEnd();
}

void renderer::RT64Context::update_screen() {
    check_refresh_rate_changes();
    app->updateScreen();
}

void renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }

    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    set_application_user_config(app.get(), new_config);

    // When updating the user configuration, only discard framebuffers if an option was changed that affects the resolution.
    bool resolution_changed = new_config.res_option != old_config.res_option;
    bool aspect_ratio_changed = new_config.ar_option != old_config.ar_option;
    bool downsampling_changed = new_config.ds_option != old_config.ds_option;
    bool msaa_changed = new_config.msaa_option != old_config.msaa_option;
    app->updateUserConfig(resolution_changed || aspect_ratio_changed || downsampling_changed || msaa_changed);

    if (msaa_changed) {
        app->updateMultisampling();
    }
    return true;
}

void renderer::RT64Context::enable_instant_present() {
    // Enable the present early presentation mode for minimal latency.
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;

    app->updateEnhancementConfig();
}

uint32_t renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float renderer::RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            else {
                return 1.0f;
            }
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

void renderer::RT64Context::check_texture_pack_actions() {
    bool packs_changed = false;
    TexturePackAction cur_action;
    while (texture_pack_action_queue.try_dequeue(cur_action)) {
        std::visit(overloaded{
            [&](TexturePackDisableAction &to_disable) {
                enabled_texture_packs.erase(to_disable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackEnableAction &to_enable) {
                enabled_texture_packs.insert(to_enable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackSecondaryDisableAction &to_override_disable) {
                secondary_disabled_texture_packs.insert(to_override_disable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackSecondaryEnableAction &to_override_enable) {
                secondary_disabled_texture_packs.erase(to_override_enable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackUpdateAction &) {
                packs_changed = true;
            }
        }, cur_action);
    }

    // If any packs were disabled, unload all packs and load all the active ones.
    if (packs_changed) {
        // Sort the enabled texture packs in reverse order so that earlier ones override later ones.
        std::vector<std::string> sorted_texture_packs{};
        sorted_texture_packs.reserve(enabled_texture_packs.size());
        for (const std::string& mod : enabled_texture_packs) {
            if (!secondary_disabled_texture_packs.contains(mod)) {
                sorted_texture_packs.emplace_back(mod);
            }
        }

        std::sort(sorted_texture_packs.begin(), sorted_texture_packs.end(),
            [](const std::string& lhs, const std::string& rhs) {
                return recomp::mods::get_mod_order_index(lhs) > recomp::mods::get_mod_order_index(rhs);
            }
        );

        // Build the path list from the sorted mod list.
        std::vector<RT64::ReplacementDirectory> replacement_directories;
        replacement_directories.reserve(enabled_texture_packs.size());
        for (const std::string &mod_id : sorted_texture_packs) {
            replacement_directories.emplace_back(RT64::ReplacementDirectory(recomp::mods::get_mod_filename(mod_id)));
        }

        if (!replacement_directories.empty()) {
            app->textureCache->loadReplacementDirectories(replacement_directories);
        }
        else {
            app->textureCache->clearReplacementDirectories();
        }
    }
}

void renderer::RT64Context::check_refresh_rate_changes() {
    uint32_t new_refresh_rate = get_display_framerate();
    if (new_refresh_rate != last_refresh_rate) {
        if (new_refresh_rate > 0) {
            recompui::config::graphics::update_refresh_rate(new_refresh_rate);
        }

        last_refresh_rate = new_refresh_rate;
    }
}

RT64::UserConfiguration::Antialiasing renderer::RT64MaxMSAA() {
    return device_max_msaa;
}

std::unique_ptr<ultramodern::renderer::RendererContext> renderer::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, ultramodern::renderer::PresentationMode presentation_mode, bool developer_mode) {
    return std::make_unique<renderer::RT64Context>(rdram, window_handle, presentation_mode, developer_mode);
}

bool renderer::RT64SamplePositionsSupported() {
    return sample_positions_supported;
}

bool renderer::RT64HighPrecisionFBEnabled() {
    return high_precision_fb_enabled;
}

void renderer::set_stereo_config(RT64::UserConfiguration::StereoMode mode, uint32_t separation, uint32_t convergence, uint32_t hudDepth, bool autoConvergence, uint32_t autoConvergenceScale, uint32_t ghostContrast, uint32_t ghostBlackFloor) {
    separation = std::clamp<uint32_t>(separation, 0, 50);
    // Tenths of a convergence slider unit: 1 = 0.1, 500 = 50.
    convergence = std::clamp<uint32_t>(convergence, 1, 500);
    hudDepth = std::clamp<uint32_t>(hudDepth, 0, 100);
    autoConvergenceScale = std::clamp<uint32_t>(autoConvergenceScale, 0, 100);
    ghostContrast = std::clamp<uint32_t>(ghostContrast, 0, 100);
    ghostBlackFloor = std::clamp<uint32_t>(ghostBlackFloor, 0, 100);
    stereo_config_packed.store(pack_stereo_config(mode, separation, convergence, hudDepth), std::memory_order_relaxed);
    const uint32_t autoPacked = (autoConvergence ? (1u << 8) : 0u) | (autoConvergenceScale & 0xFFu) |
        ((ghostContrast & 0xFFu) << 9) | ((ghostBlackFloor & 0xFFu) << 17);
    stereo_auto_packed.store(autoPacked, std::memory_order_relaxed);
}

void renderer::set_stereo_runtime_low_convergence(bool active) {
    stereo_runtime_low_convergence.store(active, std::memory_order_relaxed);
}

void renderer::trigger_texture_pack_update() {
    texture_pack_action_queue.enqueue(TexturePackUpdateAction{});
}

void renderer::enable_texture_pack(const recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    texture_pack_action_queue.enqueue(TexturePackEnableAction{mod.manifest.mod_id});

    // Check for the texture pack enabled config option.
    const recomp::config::ConfigSchema& config_schema = context.get_mod_config_schema(mod.manifest.mod_id);
    auto find_it = config_schema.options_by_id.find(renderer::special_option_texture_pack_enabled);
    if (find_it != config_schema.options_by_id.end()) {
        const recomp::config::ConfigOption& config_option = config_schema.options[find_it->second];

        if (is_texture_pack_enable_config_option(config_option, false)) {
            uint32_t value = 0;

            recomp::config::ConfigValueVariant value_variant = context.get_mod_config_value(mod.manifest.mod_id, config_option.id);
            switch (config_option.type) {
                case recomp::config::ConfigOptionType::Enum: {
                    if (uint32_t* value_ptr = std::get_if<uint32_t>(&value_variant)) {
                        value = *value_ptr;
                    }
                    break;
                }
                case recomp::config::ConfigOptionType::Bool:
                    if (bool* value_ptr = std::get_if<bool>(&value_variant)) {
                        value = *value_ptr ? 1 : 0;
                    }
                    break;
                default:
                    break;
            }

            if (value) {
                renderer::secondary_enable_texture_pack(mod.manifest.mod_id);
            }
            else {
                renderer::secondary_disable_texture_pack(mod.manifest.mod_id);
            }
        }
    }
}

void renderer::disable_texture_pack(const recomp::mods::ModHandle& mod) {
    texture_pack_action_queue.enqueue(TexturePackDisableAction{mod.manifest.mod_id});
}

void renderer::secondary_enable_texture_pack(const std::string& mod_id) {
    texture_pack_action_queue.enqueue(TexturePackSecondaryEnableAction{mod_id});
}

void renderer::secondary_disable_texture_pack(const std::string& mod_id) {
    texture_pack_action_queue.enqueue(TexturePackSecondaryDisableAction{mod_id});
}


// HD texture enable option. Must be an enum with two options.
// The first option is treated as disabled and the second option is treated as enabled.
bool renderer::is_texture_pack_enable_config_option(const recomp::config::ConfigOption& option, bool show_errors) {
    if (option.id == renderer::special_option_texture_pack_enabled) {
        switch (option.type) {
            case recomp::config::ConfigOptionType::Enum: {
                const recomp::config::ConfigOptionEnum &option_enum = std::get<recomp::config::ConfigOptionEnum>(option.variant);
                if (option_enum.options.size() != 2) {
                    if (show_errors) {
                        recompui::message_box(("Mod has the special config option id for enabling an HD texture pack (\"" + renderer::special_option_texture_pack_enabled + "\"), but the config option doesn't have exactly 2 values.").c_str());
                    }
                    return false;
                }
                return true;
            }
            case recomp::config::ConfigOptionType::Bool:
                return true;
            default:
                if (show_errors) {
                    recompui::message_box(("Mod has the special config option id for enabling an HD texture pack (\"" + renderer::special_option_texture_pack_enabled + "\"), but the config option is not an enum.").c_str());
                }
                return false;
        }
    }
    return false;
}
