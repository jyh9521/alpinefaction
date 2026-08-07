#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/StaticBufferResizePatch.h>
#include <algorithm>
#include "sound.h"
#include "../rf/sound/sound.h"
#include "../rf/sound/sound_ds.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/os/frametime.h"
#include "../multi/multi.h"
#include "../misc/alpine_settings.h"
#include "../main/main.h"
#include "../os/console.h"
#include "../misc/vpackfile.h"
#include "../rf/file/file.h"
#include "../rf/hud.h"
#include <unordered_map>
#include <string>
#include <string_view>
#include <cctype>
#include <cmath>
#include <vector>
// gr_font.h, not gr.h: split_str / string_aligned / get_font_height /
// load_font live there, and rf/hud.h only pulls in gr/gr.h.
#include "../rf/gr/gr_font.h"

// Defined further down, next to the subtitle table. Alpine already hooks both
// snd_play and snd_play_3d, so we call into it from those hooks rather than
// installing a second FunHook on the same address -- two hooks on one function
// means whichever installs last silently wins.
static void npc_subtitle_on_play(int handle, const rf::Vector3* pos);
static void cutscene_subtitle_begin(const char* track);

static int g_cutscene_bg_sound_sig = -1;
static int g_custom_sound_entry_start = -1;
static int g_taunt_sound_start = -1;
static int g_radmsg_sound_start = -1;
#ifdef DEBUG
int g_sound_test = 0;
#endif

namespace rf
{
    rf::SoundInstance sound_instances[rf::num_sound_channels];
}

void player_fpgun_move_sounds(const rf::Vector3& camera_pos, const rf::Vector3& camera_vel);

void set_play_sound_events_volume_scale()
{
    uintptr_t offsets[] = {
        // Play Sound event
        0x004BA4D8, 0x004BA515, 0x004BA71C, 0x004BA759, 0x004BA609, 0x004BA5F2, 0x004BA63F,
    };
    for (auto offset : offsets) {
        write_mem<float>(offset + 1, g_alpine_game_config.level_sound_volume);
    }
}

StaticBufferResizePatch<rf::SoundInstance> sound_instances_resize_patch{
    0x01753C38,
    30,
    rf::sound_instances,
    {
        {0x005053F9},
        {0x00505599},
        {0x005055CB},
        {0x0050571F},
        {0x005058E7},
        {0x00505A04},
        {0x00505A65},
        {0x00505A6C},
        {0x005060BB},
        {0x005058EE},
        {0x00505995},
        {0x0050599C},
        {0x00505C28},
        {0x00506197},
        {0x00505866},
        {0x00505F35},
        {0x005055C1, true},
        {0x005055DB, true},
        {0x00505A12, true},
        {0x005060EB, true},
        {0x00505893, true},
        {0x00505F68, true},
        {0x005061C1, true},
    },
};

static int snd_get_free_instance()
{
    for (unsigned i = 0; i < std::size(rf::sound_instances); ++i) {
        auto& instance = rf::sound_instances[i];
        if (instance.handle < 0) {
            return i;
        }
        if (!rf::snd_pc_is_playing(instance.sig)) {
            rf::snd_pc_stop(instance.sig);
            rf::snd_init_instance(&instance);
            return i;
        }
    }
    return -1;
}

CallHook<void()> cutscene_play_music_hook{
    0x0045BB85,
    []() {
        g_cutscene_bg_sound_sig = -1;
        cutscene_play_music_hook.call_target();
    },
};

FunHook<int(const char*, float)> snd_music_play_cutscene_hook{
    0x00505D70,
    [](const char *filename, float volume) {
        cutscene_subtitle_begin(filename);
        g_cutscene_bg_sound_sig = snd_music_play_cutscene_hook.call_target(filename, volume);
        return g_cutscene_bg_sound_sig;
    },
};

void disable_sound_before_cutscene_skip()
{
    if (g_cutscene_bg_sound_sig != -1) {
        rf::snd_pc_stop(g_cutscene_bg_sound_sig);
        g_cutscene_bg_sound_sig = -1;
    }

    rf::snd_pause(true);
    rf::snd_stop_all_paused();
    set_sound_enabled(false);
}

void enable_sound_after_cutscene_skip()
{
    set_sound_enabled(true);
}

void set_sound_enabled(bool enabled)
{
    if (is_headless_mode()) {
        rf::sound_enabled = false;
        return;
    }

    if (!g_alpine_game_config.sound_enabled) {
        rf::sound_enabled = false;
        return;
    }

    rf::sound_enabled = enabled;
}

ConsoleCommand2 level_sounds_cmd{
    "levelsounds",
    [](std::optional<float> volume) {
        if (volume.has_value()) {
            g_alpine_game_config.set_level_sound_volume(volume.value());
            set_play_sound_events_volume_scale();
        }
        rf::console::print("Level sound volume: {:.1f}", g_alpine_game_config.level_sound_volume);
    },
    "Sets level sounds volume scale",
    "levelsounds <volume>",
};

ConsoleCommand2 sound_enabled_cmd{
    "dbg_togglesound",
    [](std::optional<bool> enabled) {
        g_alpine_game_config.sound_enabled = !g_alpine_game_config.sound_enabled;
        set_sound_enabled(g_alpine_game_config.sound_enabled);
        rf::console::print("Game sound is now {}.", g_alpine_game_config.sound_enabled ? "enabled" : "disabled");
    },
    "Toggle all game sound.",
    "dbg_togglesound",
};

FunHook<int(int, int, float, float)> snd_play_hook{
    0x00505560,
    [](int handle, int group, float pan, float volume) {
        xlog::trace("snd_play {} {} {:.2f} {:.2f}", handle, group, pan, volume);

        if (!rf::sound_enabled || handle < 0) {
            return -1;
        }
        if (rf::snd_load_hint(handle) != 0) {
            xlog::warn("Failed to load sound {}", handle);
            return -1;
        }

        npc_subtitle_on_play(handle, nullptr);

        float vol_scale = volume * rf::snd_group_volume[group];
        bool looping = rf::sounds[handle].is_looping;
        int instance_index = snd_get_free_instance();
        if (instance_index < 0) {
            return -1;
        }

        auto& instance = rf::sound_instances[instance_index];
        int sig;
        if (looping) {
            sig = rf::snd_pc_play_looping(handle, vol_scale, pan, 0.0f, false);
        }
        else {
            sig = rf::snd_pc_play(handle, vol_scale, pan, 0.0f, false);
        }
        xlog::trace("snd_play handle {} vol {:.2f} sig {}", handle, vol_scale, sig);
        if (sig < 0) {
            return -1;
        }

        instance.sig = sig;
        instance.handle = handle;
        instance.group = group;
        instance.base_volume = volume;
        instance.pan = pan;
        instance.is_3d_sound = false;
        instance.use_count++;

        int instance_handle = instance_index | (instance.use_count << 8);
        return instance_handle;
    },
};

int snd_pc_play_3d_new(int handle, const rf::Vector3& pos, float vol_scale, bool looping, const rf::Vector3& vel = rf::zero_vector)
{
    // Original function has some flaws:
    // * it does not have vol_scale and vel parameters
    // * it mutes sounds below volume=0.05 (snd_pc_play does not do that)
    // * sounds.tbl volume is used (snd_update_sounds ignores it for 3D sounds)
    if (!rf::sound_pc_inited || handle == -1) {
        return -1;
    }
    int sid = rf::snd_pc_load_hint(handle, false, false);
    if (sid == -1) {
        return -1;
    }
    float pan;
    float volume_2d;
    rf::snd_calculate_2d_from_3d_info(handle, pos, &pan, &volume_2d, vol_scale);
    if (rf::sound_backend == rf::SOUND_BACKEND_DS) {
        if (rf::snd_pc_is_ds3d_enabled()) {
            // use a calculated volume scale that takes distance into account because we do not use
            // Direct Sound 3D distance attenuation model
            return rf::snd_ds_play_3d(sid, pos, vel,
                rf::sounds[handle].min_range,
                rf::sounds[handle].max_range,
                volume_2d,
                looping,
                0.0f);
        }
        float pan = rf::snd_pc_calculate_pan(pos);
        if (looping)
            return rf::snd_pc_play_looping(handle, volume_2d, pan, 0.0, true);
        return rf::snd_pc_play(handle, volume_2d, pan, 0.0f, true);
    }
    return -1;
}


FunHook<int(int, const rf::Vector3&, float, const rf::Vector3&, int)> snd_play_3d_hook{
    0x005056A0,
    [](int handle, const rf::Vector3& pos, float volume, const rf::Vector3&, int group) {
        xlog::trace("snd_play_3d {} {:.2f} {}", handle, volume, group);

        if (!rf::sound_enabled || handle < 0) {
            return -1;
        }
        if (rf::snd_load_hint(handle) != 0) {
            xlog::warn("Failed to load sound {}", handle);
            return -1;
        }

        npc_subtitle_on_play(handle, &pos);

        bool looping = rf::sounds[handle].is_looping;
        int instance_index = snd_get_free_instance();
        if (instance_index < 0) {
            return -1;
        }

        float base_volume_2d;
        float pan;
        rf::snd_calculate_2d_from_3d_info(handle, pos, &pan, &base_volume_2d, volume);
        // xlog::warn("snd_play_3d vol {:.2f}", vol_scale);
        auto& instance = rf::sound_instances[instance_index];
        int sig;
        if (rf::ds3d_enabled) {
            // Note that snd_pc_play_3d_new calls snd_calculate_2d_from_3d_info on its own
            // so use volume here instead of base_volume_2d
            float vol_scale = volume * rf::snd_group_volume[group];
            sig = snd_pc_play_3d_new(handle, pos, vol_scale, looping);
        }
        else {
            float vol_scale = base_volume_2d * rf::snd_group_volume[group];
            if (looping) {
                sig = rf::snd_pc_play_looping(handle, vol_scale, pan, 0.0f, true);
            }
            else {
                sig = rf::snd_pc_play(handle, vol_scale, pan, 0.0f, true);
            }
        }
        xlog::trace("snd_play_3d handle {} pos <{:.2f} {:.2f} {:.2f}> vol {:.2f} sig {}", handle, pos.x, pos.y, pos.z, volume, sig);
        if (sig < 0) {
            return -1;
        }

        instance.sig = sig;
        instance.handle = handle;
        instance.group = group;
        instance.base_volume = base_volume_2d;
        instance.pan = pan;
        instance.is_3d_sound = true;
        instance.pos = pos;
        instance.base_volume_3d = volume;
        instance.use_count++;

        int instance_handle = instance_index | (instance.use_count << 8);
        return instance_handle;
    },
};

FunHook<void(int, const rf::Vector3&, const rf::Vector3&, float)> snd_change_3d_hook{
    0x005058C0,
    [](int instance_handle, const rf::Vector3& pos, const rf::Vector3& vel, float volume) {
        if (!rf::ds3d_enabled) {
            snd_change_3d_hook.call_target(instance_handle, pos, vel, volume);
            return;
        }
        auto instance_index = static_cast<uint8_t>(instance_handle);
        if (instance_index >= std::size(rf::sound_instances)) {
            assert(false);
            return;
        }
        auto& instance = rf::sound_instances[instance_index];
        if (instance.use_count != (instance_handle >> 8) || !instance.is_3d_sound) {
            return;
        }

        instance.pos = pos;

        if (volume != instance.base_volume_3d) {
            instance.base_volume_3d = volume;
            rf::snd_calculate_2d_from_3d_info(instance.handle, instance.pos, &instance.pan, &instance.base_volume, instance.base_volume_3d);
            float vol_scale = instance.base_volume * rf::snd_group_volume[instance.group];
            rf::snd_pc_set_volume(instance.sig, vol_scale);
        }

        auto chnl = rf::snd_ds_get_channel(instance.sig);
        if (chnl >= 0) {
            rf::Sound* sound;
            rf::snd_pc_get_by_id(instance.handle, &sound);
            rf::snd_ds3d_update_buffer(chnl, sound->min_range, sound->max_range, pos, vel);
        }
    },
};

void snd_update_sound_instances([[ maybe_unused ]] const rf::Vector3& camera_pos)
{
    // Update sound volume of all instances because we do not use Direct Sound 3D distance attenuation model
    for (auto& instance : rf::sound_instances) {
        if (instance.handle >= 0 && instance.is_3d_sound) {
            // Note: we should use snd_pc_calc_volume_3d because snd_calculate_2d_from_3d_info ignores volume from sounds.tbl
            //       but that is how it works in RF and fixing it would change volume levels for many sounds in game...
            rf::snd_calculate_2d_from_3d_info(instance.handle, instance.pos, &instance.pan, &instance.base_volume, instance.base_volume_3d);
            float vol_scale = instance.base_volume * rf::snd_group_volume[instance.group];
            rf::snd_pc_set_volume(instance.sig, vol_scale);
            if (!rf::ds3d_enabled) {
                rf::snd_pc_set_pan(instance.sig, instance.pan);
            }
        }
    }
}

void snd_update_ambient_sounds(const rf::Vector3& camera_pos)
{
    // Play/stop ambiend sounds and update their volume
    for (auto& ambient_snd : rf::ambient_sounds) {
        if (ambient_snd.handle >= 0) {
            float distance = (camera_pos - ambient_snd.pos).len();
            auto& sound = rf::sounds[ambient_snd.handle];
            bool in_range = distance <= sound.max_range;
            // Note: when ambient sound is muted its volume is set to 0 (events can mute/unmute ambient sounds)
            // When it's muted destroy it so it can be recreated when unmuted and start playing from the beginning
            if (in_range && ambient_snd.volume > 0.0f) {
                float vol_scale = ambient_snd.volume * rf::snd_group_volume[rf::SOUND_GROUP_EFFECTS] * g_alpine_game_config.level_sound_volume;
                if (ambient_snd.sig < 0) {
                    bool is_looping = rf::sounds[ambient_snd.handle].is_looping;
                    ambient_snd.sig = snd_pc_play_3d_new(ambient_snd.handle, ambient_snd.pos, vol_scale, is_looping);
                }
                // Update volume even for non-looping sounds unlike original code
                float volume = rf::snd_pc_calc_volume_3d(ambient_snd.handle, ambient_snd.pos, vol_scale);
                rf::snd_pc_set_volume(ambient_snd.sig, volume);
                if (!rf::ds3d_enabled) {
                    float pan = rf::snd_pc_calculate_pan(ambient_snd.pos);
                    rf::snd_pc_set_pan(ambient_snd.sig, pan);
                }
            }
            else if (ambient_snd.sig >= 0) {
                rf::snd_pc_stop(ambient_snd.sig);
                ambient_snd.sig = -1;
            }
        }
    }
}

#ifdef DEBUG

ConsoleCommand2 sound_stress_test_cmd{
    "sound_stress_test",
    [](int num) {
        g_sound_test = num;
    },
};

void sound_test_do_frame()
{
    if (g_sound_test && rf::local_player_entity) {
        static float sounds_to_add = 0.0f;
        sounds_to_add += rf::frametime * g_sound_test;
        while (sounds_to_add >= 1.0f) {
            auto pos = rf::local_player_entity->p_data.pos;
            pos.x += ((rand() % 200) - 100) / 50.0f;
            pos.y += ((rand() % 200) - 100) / 50.0f;
            pos.z += ((rand() % 200) - 100) / 50.0f;
            rf::snd_play_3d(0, pos, 1.0f, rf::Vector3{}, 0);
            sounds_to_add -= 1.0f;
        }
    }
}

#endif // DEBUG

FunHook<void(const rf::Vector3&, const rf::Vector3&, const rf::Matrix3&)> snd_update_sounds_hook{
     0x00505EC0,
    [](const rf::Vector3& camera_pos, const rf::Vector3& camera_vel, const rf::Matrix3& camera_orient) {
        if (is_headless_mode()) {
            rf::sound_enabled = false;
            return;
        }

        player_fpgun_move_sounds(camera_pos, camera_vel);

#ifdef DEBUG
        sound_test_do_frame();
#endif

        rf::sound_listener_pos = camera_pos;
        rf::sound_listener_rvec = camera_orient.rvec;

        // Update DirectSound 3D listener parameters
        rf::snd_pc_change_listener(camera_pos, camera_vel, camera_orient);

        snd_update_sound_instances(camera_pos);
        snd_update_ambient_sounds(camera_pos);
    },
};

FunHook<bool(const char*)> snd_pc_file_exists_hook{
    0x00544680,
    [](const char *filename) {
        bool exists = snd_pc_file_exists_hook.call_target(filename);

        if (!exists) {
            // For some reason built-in maps often use spaces in place of sound filename (their way to disable a sound?)
            // To reduce spam ignore space-only filenames and avoid logging the same filename multiple times in a row
            bool has_only_spaces = std::string_view{filename}.find_first_not_of(" ") == std::string_view::npos;
            static std::string last_file_not_found;
            if (!has_only_spaces && !is_known_missing_stock_asset(filename) && filename != last_file_not_found) {
                xlog::warn("Sound file not found: {}", filename);
                last_file_not_found = filename;
            }
        }
        return exists;
    }
};

FunHook<int(const char*, float, float, float)> snd_get_handle_hook{
    0x005054B0,
    [](const char* filename, float min_range, float base_volume, float rolloff) {
        if (!rf::sound_enabled) {
            return -1;
        }
        return snd_get_handle_hook.call_target(filename, min_range, base_volume, rolloff);
    },
};

int get_custom_sound_id(int custom_id) {
    return g_custom_sound_entry_start + custom_id;
}

int get_custom_chat_message_sound_id(int custom_id, bool is_taunt)
{
    return is_taunt ? g_taunt_sound_start + custom_id : g_radmsg_sound_start + custom_id;
}

void gamesound_parse_custom_sounds() 
{
    // Record first custom sound ID
    // Note this does NOT work on servers because servers don't load sounds
    // When server sends packets with sound IDs, it must send custom IDs and client uses get_custom_sound_id
    g_custom_sound_entry_start = rf::g_num_sounds;

    std::vector<CustomSoundEntry> custom_sounds = {
        {"af_achievement1.wav", 10.0f, 1.0f, 1.0f},     // 0
        {"af_pinglocation1.wav", 10.0f, 1.0f, 1.0f},    // 1
        {"af_hitsound1.wav", 10.0f, 1.0f, 1.0f},        // 2
        {"af_killsound1.wav", 10.0f, 1.0f, 1.0f},       // 3
        {"Console_Large_03.wav", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_000.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_001.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_002.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_003.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_004.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_005.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_006.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_007.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_008.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_009.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_010.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_011.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_012.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_013.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_014.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_015.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_016.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_017.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_018.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_019.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_020.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_021.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_022.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_023.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_024.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_025.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_026.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_027.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_028.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_029.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_030.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_031.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_032.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_033.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_034.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_035.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_036.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_037.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_038.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_039.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_040.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_041.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_042.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_043.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_044.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_045.ogg", 10.0f, 1.0f, 1.0f},
        {"af_radmsg_046.ogg", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_16.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_17.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_18.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_19.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_20.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_21.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_22.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_23.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_24.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_25.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_26.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_27.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_28.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_29.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_30.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_31.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_32.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_33.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_34.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_35.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_36.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_37.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_38.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_39.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_40.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_41.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_42.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_43.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_44.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_45.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_46.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_47.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_48.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_49.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_50.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_51.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_52.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_53.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_54.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_55.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_56.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_57.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_58.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_59.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_60.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_61.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_62.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_63.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_64.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_65.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_66.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_67.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_68.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_69.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_70.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_71.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_72.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_73.wav", 10.0f, 1.0f, 1.0f},
        {"MP_TAUNT_74.wav", 10.0f, 1.0f, 1.0f},
    };

    for (const auto& sound : custom_sounds) 
    {
        int sound_id = rf::snd_get_handle(sound.filename, sound.min_range, sound.base_volume, sound.rolloff);
        if (sound_id >= 0) {
            //xlog::warn("Added custom sound {} at ID {}", sound.filename, sound_id);
        } else {
            xlog::error("Failed to index sound file: {}", sound.filename);
        }
    }

    g_taunt_sound_start = rf::snd_pc_find_by_name("MP_TAUNT_16.wav");
    g_radmsg_sound_start = rf::snd_pc_find_by_name("af_radmsg_000.ogg");

    //xlog::warn("Custom sounds added, starting at ID {}. Taunts start at ID {}", g_custom_sound_entry_start, g_taunt_sound_start);
}

CodeInjection gamesound_parse_sounds_table_patch{
    0x00434708,
    []() {
        if (!rf::is_dedicated_server && !is_headless_mode()) {
            gamesound_parse_custom_sounds();
        }
    },
};

void play_chat_sound(const std::string_view msg, const bool is_taunt) {
    if (is_taunt && !g_alpine_game_config.play_taunt_sounds) {
        return; // taunts are turned off
    }

    // Mapping of chat messages to custom sound IDs - strings must match exactly
    static const std::unordered_map<std::string_view, int> sound_map = {
        // Express
        {"Hello", 0},
        {"Goodbye", 1},
        {"Oops...", 2},
        {"RED FACTION!", 3},
        {"Quiet!", 4},
        {"Modder", 5},

        // Compliment
        {"Good job!", 6},
        {"Well played!", 7},
        {"Nice frag!", 8},
        {"You're on fire!", 9},

        // Respond
        {"Yes", 10},
        {"No", 11},
        {"I don't know", 12},
        {"Thanks", 13},
        {"Any time", 14},
        {"Got it", 15},
        {"Sorry", 16},
        {"Wait", 17},

        // Attack/Defend
        {"Attack incoming from high", 18},
        {"Attack incoming from mid", 19},
        {"Attack incoming from low", 20},
        {"Defend!", 21},
        {"Cover me!", 22},
        {"Wait for my signal", 23},

        // Enemy
        {"Enemy is going high", 24},
        {"Enemy is going mid", 25},
        {"Enemy is going low", 26},
        {"Enemy is down", 27},

        // Timing
        {"Damage Amplifier is respawning soon", 28},
        {"Fusion is respawning soon", 29},
        {"Super Armor is respawning soon", 30},
        {"Super Health is respawning soon", 31},
        {"Invulnerability is respawning soon", 32},
        {"Rail Driver is respawning soon", 33},

        // Powerup
        {"Damage Amplifier is up!", 34},
        {"Fusion is up!", 35},
        {"Super Armor is up!", 36},
        {"Super Health is up!", 37},
        {"Invulnerability is up!", 38},
        {"Rail Driver is up!", 39},

        // Flag
        {"Where's the enemy flag?", 40},
        {"Where's our flag?", 41},
        {"Take the flag from me", 42},
        {"Give me the flag", 43},
        {"I'm retrieving the flag", 44},
        {"Retrieve our flag!", 45},
        {"Our flag is secure", 46},

        // Start taunts
        // Commander 1
        {"Commence beatdown!", 0},
        {"Rest in pieces.", 1},
        {"Hey, is this your head?", 2},
        {"Aw yeah!", 3},
        {"You make a nice target.", 4},
        {"Squeegee time!", 5},
        {"Nice catch!", 6},
        {"Goodbye Mr. Gibs!", 7},

        // Commander 2
        {"Got death smarts.", 8},
        {"Just a flesh wound.", 9},
        {"Ka-ching!", 10},
        {"Frag-o-licious!", 11},
        {"Me red, you dead!", 12},
        {"Look, a jigsaw puzzle!", 13},
        {"Damn, I'm good.", 14},

        // Guard 1
        {"Here's Johnny!", 15},
        {"Lay down, play dead!", 16},
        {"Sucks to be you!", 17},
        {"You are so dead.", 18},
        {"Woohoo!", 19},
        {"Target practice!", 20},
        {"Get a load of this!", 21},
        {"Chump!", 22},

        // Guard 2
        {"Feeble!", 23},
        {"Sit down!", 24},
        {"Owned!", 25},
        {"Have a seat, son!", 26},
        {"Fresh meat!", 27},
        {"Aw yeah!", 28},
        {"Boom!", 29},

        // Enviro Guard 1
        {"Messy.", 30},
        {"Blams!", 31},
        {"Splat!", 32},
        {"Crunch time!", 33},
        {"Eat it!", 34},
        {"Annihilation!", 35},
        {"Banned.", 36},
        {"Catch!", 37},

        // Enviro Guard 2
        {"You lack discipline!", 38},
        {"Lamer!", 39},
        {"Llama!", 40},
        {"Order up!", 41},
        {"Your move, creep!", 42},
        {"What's your name, scumbag?!", 43},
        {"Arr matey!", 44},

        // Riot Guard 1
        {"I make this look good.", 45},
        {"Take off, hoser!", 46},
        {"Get on the bus!", 47},
        {"What's up, fool?!", 48},
        {"Want some more?!", 49},
        {"Give it up!", 50},
        {"Oh, I still love you!", 51},

        // Riot Guard 2
        {"Geeze, what smells?", 52},
        {"Aww, does it hurt?", 53},
        {"Bring it!", 54},
        {"Any time, anywhere!", 55},
        {"Pathetic!", 56},
        {"Sweet!", 57},
        {"Tool!", 58}
    };

    // Lookup the sound ID and play it
    const auto it = sound_map.find(msg);
    if (it != sound_map.end()) {
        const int sound_id = it->second;
        if ((sound_id <= 5 && g_alpine_game_config.play_global_rad_msg_sounds)
            || (sound_id > 5 && g_alpine_game_config.play_team_rad_msg_sounds)
        ) {
            play_local_sound_2d(get_custom_chat_message_sound_id(sound_id, is_taunt), 2, 1.0f);
            // xlog::warn("Playing custom sound {} for radio message {}", sound_id, chat_message);
        }
    }
}

extern void snd_ds_apply_patch();
extern void sound_foley_apply_patches();


// ---- NPC voice subtitles ----------------------------------------------------
// Ambient NPC barks have no subtitles in the original game. The table lives in a
// packfile as UTF-8 text, one entry per line:
//     <wav filename><TAB><subtitle>
// Blank lines and lines starting with // are ignored. If the file is absent the
// feature is simply off and behaviour is identical to stock.
constexpr const char* npc_subtitle_filename = "alpine_npc_subtitles.tbl";

static std::string npc_sub_lower(const char* s)
{
    std::string out;
    for (; s && *s; ++s) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
    }
    return out;
}

static const std::unordered_map<std::string, std::string>& npc_subtitle_table()
{
    static std::unordered_map<std::string, std::string> table;
    static bool loaded = false;
    if (loaded) {
        return table;
    }
    loaded = true;

    rf::File file;
    if (file.open(npc_subtitle_filename) != 0) {
        // Log it: a silent empty table is indistinguishable from a hook that never ran.
        xlog::info("{} not found, NPC subtitles are off", npc_subtitle_filename);
        return table;
    }
    const int len = file.size();
    std::string buf(static_cast<size_t>(len), '\0');
    int total = 0;
    while (total < len) {
        const int n = file.read(buf.data() + total, len - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    file.close();
    buf.resize(static_cast<size_t>(total));

    size_t pos = 0;
    while (pos < buf.size()) {
        size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos) {
            eol = buf.size();
        }
        std::string_view line{buf.data() + pos, eol - pos};
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        if (line.empty() || line.substr(0, 2) == "//") {
            continue;
        }
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos) {
            continue;
        }
        // foley.tbl spells these "Grd_Alert_01.wav" but the packfile index has them
        // upper case, so the key is always lowered on both sides.
        std::string key{line.substr(0, tab)};
        for (char& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string text{line.substr(tab + 1)};
        if (!key.empty() && !text.empty()) {
            table.emplace(std::move(key), std::move(text));
        }
    }
    xlog::info("Loaded {} NPC subtitles from {}", table.size(), npc_subtitle_filename);
    return table;
}

static int g_npc_subtitle_debug = 30;   // log the first N lookups, then go quiet

// ---- Independent subtitle channel -------------------------------------------
// rf::hud_msg is shared with pickup notices ("Picked up ...") and holds at most 8
// entries, so voice subtitles and item pickups keep pushing each other out. This is
// a separate queue with its own render pass: it cannot be stepped on, and position,
// lifetime and styling are ours to set.
constexpr int npc_subtitle_max_lines = 3;

struct NpcSubtitleLine
{
    std::string text;
    int expire_ms = 0;
};

static NpcSubtitleLine g_npc_subtitle_queue[npc_subtitle_max_lines];

static void npc_subtitle_push(const std::string& text, int duration_ms)
{
    for (int i = 0; i + 1 < npc_subtitle_max_lines; ++i) {
        g_npc_subtitle_queue[i] = std::move(g_npc_subtitle_queue[i + 1]);
    }
    auto& slot = g_npc_subtitle_queue[npc_subtitle_max_lines - 1];
    slot.text = text;
    slot.expire_ms = rf::frametime_total_milliseconds + duration_ms;
}

// Same font the message log body uses (0x006C66F0 is set when the log UI is created).
// Cached rather than loaded per call: the .vf redirect logs a line every time.
static auto& npc_subtitle_msglog_font = addr_as_ref<int>(0x006C66F0);

static int npc_subtitle_font()
{
    if (npc_subtitle_msglog_font) {
        return npc_subtitle_msglog_font;
    }
    static int fallback = rf::gr::load_font("rfpc-medium.vf");
    return fallback;
}

// ---- Cutscene subtitles ------------------------------------------------------
// In-engine cutscenes do not play their dialogue through snd_play/snd_play_3d at
// all: the whole scene is one pre-mixed track (RFCS_*_FinalMix.wav) streamed via
// snd_music_play, so there is no per-line filename to look up. The engine also
// ships no text for these lines -- they were transcribed and translated by hand.
// So this is a plain timed track: remember when playback started, then show
// whichever phrase covers the elapsed time.
constexpr const char* cutscene_subtitle_filename = "alpine_cutscene_subtitles.tbl";

struct CutscenePhrase
{
    int start_ms;
    int end_ms;
    std::string text;
};

// Local decimal parser: avoids pulling <cstdlib> in just for atoi.
static int cutscene_parse_int(std::string_view s)
{
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            break;
        }
        v = v * 10 + (c - '0');
    }
    return v;
}

static const std::unordered_map<std::string, std::vector<CutscenePhrase>>& cutscene_subtitle_table()
{
    static std::unordered_map<std::string, std::vector<CutscenePhrase>> table;
    static bool loaded = false;
    if (loaded) {
        return table;
    }
    loaded = true;

    rf::File file;
    if (file.open(cutscene_subtitle_filename) != 0) {
        xlog::info("{} not found, cutscene subtitles are off", cutscene_subtitle_filename);
        return table;
    }
    const int len = file.size();
    std::string buf(static_cast<size_t>(len), '\0');
    int total = 0;
    while (total < len) {
        const int n = file.read(buf.data() + total, len - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    file.close();
    buf.resize(static_cast<size_t>(total));

    size_t pos = 0;
    while (pos < buf.size()) {
        size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos) {
            eol = buf.size();
        }
        std::string_view line{buf.data() + pos, eol - pos};
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        if (line.empty() || line.substr(0, 2) == "//") {
            continue;
        }
        const size_t t1 = line.find('\t');
        if (t1 == std::string_view::npos) continue;
        const size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string_view::npos) continue;
        const size_t t3 = line.find('\t', t2 + 1);
        if (t3 == std::string_view::npos) continue;
        CutscenePhrase p;
        p.start_ms = cutscene_parse_int(line.substr(t1 + 1, t2 - t1 - 1));
        p.end_ms = cutscene_parse_int(line.substr(t2 + 1, t3 - t2 - 1));
        p.text = std::string{line.substr(t3 + 1)};
        if (p.end_ms > p.start_ms && !p.text.empty()) {
            table[npc_sub_lower(std::string{line.substr(0, t1)}.c_str())].push_back(std::move(p));
        }
    }
    size_t n_lines = 0;
    for (auto& kv : table) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const CutscenePhrase& a, const CutscenePhrase& b) {
                      return a.start_ms < b.start_ms;
                  });
        n_lines += kv.second.size();
    }
    xlog::info("Loaded {} cutscene subtitles across {} tracks from {}",
        n_lines, table.size(), cutscene_subtitle_filename);
    return table;
}

static const std::vector<CutscenePhrase>* g_cutscene_phrases = nullptr;
static int g_cutscene_started_ms = 0;

static void cutscene_subtitle_begin(const char* track)
{
    g_cutscene_phrases = nullptr;
    if (!track) {
        return;
    }
    const auto& table = cutscene_subtitle_table();
    const auto it = table.find(npc_sub_lower(track));
    if (it == table.end()) {
        return;
    }
    g_cutscene_phrases = &it->second;
    g_cutscene_started_ms = rf::frametime_total_milliseconds;
    xlog::info("Cutscene subtitles armed for '{}' ({} lines)", track, it->second.size());
}

// Returns the phrase covering the current playback position, or nullptr.
static const std::string* cutscene_subtitle_current(int now)
{
    if (!g_cutscene_phrases || g_cutscene_bg_sound_sig == -1) {
        // sig is cleared when the player skips the cutscene
        return nullptr;
    }
    const int elapsed = now - g_cutscene_started_ms;
    for (const auto& p : *g_cutscene_phrases) {
        if (elapsed < p.start_ms) {
            break;      // sorted, so nothing later can match either
        }
        if (elapsed < p.end_ms) {
            return &p.text;
        }
    }
    return nullptr;
}

void npc_subtitle_render()
{
    const int now = rf::frametime_total_milliseconds;

    // Two paths drive this: the HUD message render hook during normal gameplay, and
    // the cutscene per-frame hook (the HUD is not drawn at all during cutscenes).
    // Both can run in the same frame, and drawing twice would double the drop shadow,
    // so the second call in a frame is a no-op.
    static int last_drawn_ms = -1;
    if (now == last_drawn_ms) {
        return;
    }
    last_drawn_ms = now;

    const int font = npc_subtitle_font();
    const int line_h = rf::gr::get_font_height(font);
    const int clip_w = rf::gr::clip_width();
    const int clip_h = rf::gr::clip_height();
    const int max_w = clip_w * 4 / 5;

    // Cutscene line first, then the ordinary queue. During a cutscene the queue is
    // normally empty anyway, but a sound started just before the cutscene began can
    // still be expiring, and stacking them is better than one hiding the other.
    std::vector<std::string> sources;
    if (const std::string* cut = cutscene_subtitle_current(now)) {
        sources.push_back(*cut);
    }
    for (const auto& slot : g_npc_subtitle_queue) {
        if (!slot.text.empty() && now < slot.expire_ms) {
            sources.push_back(slot.text);
        }
    }

    std::vector<std::string> lines;
    for (const auto& src : sources) {
        // gr_split_str is our CJK aware replacement, so this wraps Chinese correctly
        std::string buf = src;
        int len_array[8] = {};
        int off_array[8] = {};
        const int n = rf::gr::split_str(len_array, off_array, buf.data(), max_w, 8, 0, font);
        if (n <= 0) {
            lines.push_back(buf);
        }
        else {
            for (int i = 0; i < n; ++i) {
                lines.emplace_back(buf, static_cast<size_t>(off_array[i]),
                                   static_cast<size_t>(len_array[i]));
            }
        }
    }
    if (lines.empty()) {
        return;
    }

    // Sit above the engine's own message line so the two never overlap.
    const int center_x = clip_w / 2;
    int y = clip_h * 84 / 100 - static_cast<int>(lines.size()) * line_h;
    for (const auto& line : lines) {
        rf::gr::set_color(0, 0, 0, 170);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x + 2, y + 2, line.c_str(), font);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x, y, line.c_str(), font);
        y += line_h;
    }
}


static void npc_subtitle_on_play(int handle, const rf::Vector3* pos)
{
    const auto& table = npc_subtitle_table();
    if (table.empty() || handle < 0 || handle >= 2600) {
        return;
    }
    const rf::Sound& snd = rf::sounds[handle];
    const auto it = table.find(npc_sub_lower(snd.filename));

    // Diagnostics first, before any of the early returns below. Logging after them
    // means a line skipped by the persona check leaves no trace at all, which makes
    // "no subtitle" and "hook never saw it" look identical in the log.
    // Only level dialogue names (L<n>S<n>_...) are logged, so gunfire and footsteps
    // do not burn the budget.
    if (g_npc_subtitle_debug > 0) {
        const char* fn = snd.filename;
        // Level dialogue is named L<level>S<scene>_..., e.g. L6S3_GRYN_01.wav.
        // Checking the first two chars avoids needing <cstring> for strchr.
        const bool is_dialogue = fn && (fn[0] == 'L' || fn[0] == 'l')
            && fn[1] >= '0' && fn[1] <= '9';
        if (is_dialogue || it != table.end()) {
            --g_npc_subtitle_debug;
            const int pidx = rf::hud_persona_current_idx;
            const bool persona_owns = pidx >= 0 && pidx < 10
                && rf::hud_personas_info[pidx].sound_handle == handle;
            xlog::info("[npc-sub] '{}' {} in_table={} persona_owns={} alpha={:.2f}",
                fn ? fn : "(null)", pos ? "3D" : "2D", it != table.end(), persona_owns,
                rf::hud_persona_alpha);
        }
    }

    // Skip only the one line the engine is itself subtitling in the persona box.
    // HudPersonaInfo::sound_handle and hud_persona_current_idx are both set before the
    // sound is started (0x0043957C sets the index, the play call reads +0x20 after it),
    // so this comparison is reliable at the moment we run.
    //
    // Do NOT gate on hud_persona_alpha alone: the box is up for several seconds, and
    // world conversations often happen underneath it -- the level 1 opening has Orion
    // on the radio while a guard and a miner argue in front of you.
    const int persona_idx = rf::hud_persona_current_idx;
    if (persona_idx >= 0 && persona_idx < 10
        && rf::hud_personas_info[persona_idx].sound_handle == handle) {
        return;
    }

    float dist = -1.0f;
    if (pos) {
        const float dx = pos->x - rf::sound_listener_pos.x;
        const float dy = pos->y - rf::sound_listener_pos.y;
        const float dz = pos->z - rf::sound_listener_pos.z;
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    if (it == table.end()) {
        return;
    }
    // Out of earshot: the engine plays it inaudibly, no reason to subtitle it.
    // Only trust max_range when it is actually set -- snd_get_handle takes min_range
    // but no max_range, so for dynamically loaded sounds it can be left at zero, and
    // a naive "dist > max_range" then rejects everything.
    if (dist >= 0.0f && snd.max_range > 0.0f && dist > snd.max_range) {
        return;
    }
    // Duration follows the clip: long lines stay up long enough to read.
    const float secs = rf::snd_pc_get_duration(handle);
    int ms = static_cast<int>(secs * 1000.0f) + 900;
    ms = std::clamp(ms, 2500, 9000);
    npc_subtitle_push(it->second, ms);
}

void apply_sound_patches()
{
    // Sound loop fix in snd_music_play
    write_mem<u8>(0x00505D07 + 1, 0x00505D5B - (0x00505D07 + 2));

    // Cutscene skip support
    cutscene_play_music_hook.install();
    snd_music_play_cutscene_hook.install();

    // Fix ambient sound volume updating
    AsmWriter(0x00505FD7, 0x00505FFB)
        .mov(asm_regs::eax, *(asm_regs::esp + 0x50 + 4))
        .push(asm_regs::eax)
        .mov(asm_regs::eax, *(asm_regs::esi))
        .push(asm_regs::eax);

    // Change number of sound channels
    //write_mem<u8>(0x005055AB, asm_opcodes::jmp_rel_short); // never free sound instances, uncomment to test
    sound_instances_resize_patch.install();
    auto max_sound_instances = static_cast<i8>(std::size(rf::sound_instances));
    write_mem<i8>(0x005053F5 + 1, max_sound_instances); // snd_init_sound_instances_array
    write_mem<i8>(0x005058D8 + 2, max_sound_instances); // snd_change_3d
    write_mem<i8>(0x0050598A + 2, max_sound_instances); // snd_change
    write_mem<i8>(0x00505A36 + 2, max_sound_instances); // snd_stop_all_paused_sounds
    write_mem<i8>(0x00505A5A + 2, max_sound_instances); // snd_stop
    write_mem<i8>(0x00505C31 + 2, max_sound_instances); // snd_is_playing

    // Improve handling of instance slot allocation in snd_play
    snd_play_hook.install();

    // Use DirectSound 3D for 3D sounds
    snd_play_3d_hook.install();

    // Properly update DirectSound 3D sounds
    snd_change_3d_hook.install();
    snd_update_sounds_hook.install();

    // Apply patch for DirectSound specific code
    snd_ds_apply_patch();

    // Apply foley and impact sound set patches
    sound_foley_apply_patches();

    // eax_ks_property_set should not call Release, since it is a weak pointer.
    AsmWriter{0x00527EE0, 0x00527EE4}.nop();

    // Change default volume to 0.5
    write_mem<float>(0x00506192 + 1, 0.5f);

    // Log when sound file cannot be found
    snd_pc_file_exists_hook.install();

    // Do not update sounds array in dedicated server mode
    snd_get_handle_hook.install();

    // Add custom sounds to sounds array
    gamesound_parse_sounds_table_patch.install();
}

void register_sound_commands()
{
    level_sounds_cmd.register_cmd();
    sound_enabled_cmd.register_cmd();
#ifdef DEBUG
    sound_stress_test_cmd.register_cmd();
#endif
}
