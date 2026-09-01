#pragma once

#include <unordered_set>
#include <filesystem>

#include "common/rt64_user_configuration.h"
#include "ultramodern/renderer_context.hpp"
#include "librecomp/config.hpp"
#include "librecomp/mods.hpp"

namespace RT64 {
    struct Application;
}

namespace recompui {
    namespace renderer {
        inline const std::string special_option_texture_pack_enabled = "_recomp_texture_pack_enabled";

        class RT64Context final : public ultramodern::renderer::RendererContext {
        public:
            ~RT64Context() override;
            RT64Context(uint8_t *rdram, ultramodern::renderer::WindowHandle window_handle, ultramodern::renderer::PresentationMode presentation_mode, bool developer_mode);

            bool valid() override { return static_cast<bool>(app); }

            bool update_config(const ultramodern::renderer::GraphicsConfig &old_config, const ultramodern::renderer::GraphicsConfig &new_config) override;

            void enable_instant_present() override;
            void send_dl(const OSTask *task) override;
            void send_dummy_workload(uint32_t fb_address) override;
            void update_screen() override;
            void shutdown() override;
            uint32_t get_display_framerate() const override;
            float get_resolution_scale() const override;

        private:
            std::unique_ptr<RT64::Application> app;
            std::unordered_set<std::string> enabled_texture_packs;
            std::unordered_set<std::string> secondary_disabled_texture_packs;
            uint32_t last_refresh_rate = 0;

            void check_texture_pack_actions();
            void check_refresh_rate_changes();
        };

        std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t *rdram, ultramodern::renderer::WindowHandle window_handle, ultramodern::renderer::PresentationMode presentation_mode, bool developer_mode);

        RT64::UserConfiguration::Antialiasing RT64MaxMSAA();
        bool RT64SamplePositionsSupported();
        bool RT64HighPrecisionFBEnabled();

        // Pushes stereoscopic 3D parameters that the active render context will pick up
        // on its next frame. Safe to call from any thread.
        // separation is the user-facing 0..50 slider value. convergence arrives in
        // TENTHS of its 0.1..50 slider (so 1..500), because that slider steps in
        // tenths below 1 and the bridge below is integer-only. The render layer
        // maps both to world-space units.
        // hudDepth is 0..100 with 50 = screen plane.
        // autoConvergence: when true, the renderer scales the configured convergence
        // by autoConvergenceScale (0..100, applied as a fraction) during scenes the
        // BK-side detection function flags as flat/near-camera (file select, FMVs,
        // first-person, cutscenes, Bottles' bonus).
        // ghostContrast (0..100, 100 = off) and ghostBlackFloor (0..100,
        // 0 = off) drive the compose shader's anti-crosstalk range compression:
        // squeezing the signal range shrinks the brightness difference between
        // the eyes, which is what makes a display's crosstalk visible, and the
        // black floor gives a cancelling display's subtraction room before it
        // clips at zero.
        void set_stereo_config(RT64::UserConfiguration::StereoMode mode, uint32_t separation, uint32_t convergence, uint32_t hudDepth, bool autoConvergence, uint32_t autoConvergenceScale, uint32_t ghostContrast, uint32_t ghostBlackFloor);

        // Called by the BK side (via the recomp_api bridge) once per frame to
        // tell the renderer whether the current scene is in a "low-convergence"
        // state (FMV, file select, first-person view, etc.). The renderer reads
        // this on its next frame and scales the configured convergence down
        // when set_stereo_config's autoConvergence flag is on. Safe to call
        // from any thread.
        void set_stereo_runtime_low_convergence(bool active);

        // Called once per frame by the game to say whether the first-person
        // camera is live, and so whether the aiming reticle is on screen. The
        // renderer gates its reticle search on this. Safe to call from any
        // thread.
        void set_stereo_runtime_first_person(bool active);

        void trigger_texture_pack_update();
        void enable_texture_pack(const recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod);
        void disable_texture_pack(const recomp::mods::ModHandle& mod);
        void secondary_enable_texture_pack(const std::string& mod_id);
        void secondary_disable_texture_pack(const std::string& mod_id);

        // Texture pack enable option. Must be an enum with two options.
        // The first option is treated as disabled and the second option is treated as enabled.
        bool is_texture_pack_enable_config_option(const recomp::config::ConfigOption& option, bool show_errors);
    }
}
