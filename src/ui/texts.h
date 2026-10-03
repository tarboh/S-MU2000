// license:BSD-3-Clause
//
// Display strings for the panel pages and the main window around
// them (strip, popup menus, status line). The struct below is the source
// of truth: changing a member changes every platform at once, and shared
// code only ever reads ui::texts().
//
// Which table texts() returns comes from ui::lang (src/ui/lang.h): --lang,
// then lang= in editor.ini, then the locale (Japanese iff LC_ALL /
// LC_MESSAGES / LANG / LANGUAGE says ja, English otherwise).
//
// One language per file: texts_ja.h, texts_en.h, ... Each file defines one
// function returning ui_texts with one designated .field per member, in
// struct order. To add a string, add the member here and one .field line
// per language file; to add a language, copy texts_en.h to texts_<code>.h,
// register the code in ui/lang.h and dispatch it in texts() below.
//
// tools/check_texts.py checks all of this: every language file must define
// exactly the struct's field set (missing/extra fail), and every *_fmt
// must carry the same printf sequences in every language.
//
// Header-only on purpose: no build file on any platform needs a new source.

#ifndef S_MU2000_UI_TEXTS_H
#define S_MU2000_UI_TEXTS_H

#pragma once

#include "ui/lang.h"

namespace ui {

struct ui_texts {
	// Bottom strip tabs (panel.cpp)
	const char *tab_panel;
	const char *tab_editor;
	const char *tab_effects;
	// Front page hint (panel.cpp)
	const char *hint_front;
	// Editor page (editor.cpp)
	const char *editor_hint;         // may contain \n
	const char *editor_xg_reset;
	const char *editor_all_off;
	// Effects page (effects.cpp)
	const char *effects_title;
	const char *effects_values_note;
	const char *effects_insertion_note;   // may contain \n
	// panel.txt parse errors (layout.cpp; printf format taking %d and %s)
	const char *layout_line_error;
	// Engine boot log (engine.h, bootcache.h; shown on the terminal).
	// The *_fmt entries are printf formats taking the shown arguments.
	const char *engine_warn_fmt;          // %s: a ROM file problem
	const char *engine_nvram_fmt;         // %s: settings file in use
	const char *engine_boot_cached_fmt;   // %s: snapshot path
	const char *engine_boot_saved_fmt;    // %s: snapshot path
	const char *engine_boot_failed;       // shown in the window's message area
	const char *engine_resetting;         // shown in the window's message area
	const char *engine_reset_done;
	const char *bootcache_read_error_fmt; // %s
	// Top strip buttons (toolbar.h)
	const char *bar_list;
	const char *bar_editor;
	const char *bar_shapes;
	const char *bar_fx;
	const char *bar_master;
	// Popup menus (menu.h). menu_in_* also names the ports in the
	// startup report (app.h).
	const char *menu_in_a;
	const char *menu_in_b;
	const char *menu_in_c;
	const char *menu_in_d;
	const char *menu_unused;
	const char *menu_no_devices;
	const char *menu_ain_title;
	const char *menu_no_ain;
	const char *menu_audio_title;
	const char *menu_audio_default;
	const char *menu_no_audio;
	const char *audio_switch_failed_fmt; // %s: device and backend error
	const char *menu_out_mu;
	const char *menu_thru_a;
	const char *menu_thru_b;
	const char *menu_open_list;
	const char *menu_open_editor;
	const char *menu_native_fx;
	const char *menu_native_engine;
	const char *menu_factory;
	const char *menu_card_new;
	const char *menu_card_open;
	const char *menu_card_eject;
	const char *menu_card_eject_fmt;  // %s: the file in the slot
	const char *menu_play_file;
	const char *menu_stop;
	const char *menu_stop_fmt;        // %s: the song playing
	const char *menu_fold34;
	const char *menu_drop34;
	const char *menu_thin_bends;
	const char *menu_out_title;
	const char *menu_out_digital;
	const char *menu_out_analog;
	// Status line (status.h, app.h; printf format taking %d, %.0f, %.1f
	// and three %s). status_none fills IN/OUT with no port, status_booting
	// shows while the engine is not up yet.
	const char *status_format;
	const char *status_none;
	const char *status_booting;
	// Status middle per backend (app_win.h, app_mac.h, app_linux.h): what
	// each audio backend measures. Shown inside status_format's %s.
	const char *status_middle_win_fmt;    // %.0f ms of wait, drop count
	const char *status_middle_mac_fmt;    // drop count
	const char *status_middle_linux_fmt;  // drop count
	// Command-line help (tool_args.h; may contain \n, no printf verbs).
	const char *help_usage;
	// The help checkbox above the editor windows (xg_ui.cpp). The language
	// itself is global (--lang, editor.ini, locale), so there is no
	// language combo here.
	const char *xgui_help_show;
	const char *xgui_help_tip;
	// Insertion editor (fx_editor.cpp).
	const char *fxe_eq_tip;
	const char *fxe_no_desc;
	const char *fxe_hover_knob;
	// File dialogs, confirmations and error notes (app_win.h, app_mac.h,
	// app_linux.h, window_mac.mm, view_win.cpp, view_mac.mm, view.cpp,
	// pc_window.cpp, pc_window_mac.mm, pc_window_linux.cpp). One wording
	// serves every backend; the identical console twins in app.h share
	// these fields rather than duplicating them.
	const char *dlg_card_open;
	const char *dlg_card_save;
	const char *dlg_midi_open;
	const char *dlg_sysex_desc;
	const char *dlg_smartmedia_desc;
	const char *dlg_all_files;
	const char *dlg_midi_desc;
	const char *dlg_factory_text;    // may contain \n
	const char *dlg_factory_ok;
	const char *dlg_cannot_fmt;      // %s: what failed
	const char *dlg_window_fail;
	const char *dlg_editor_fail;
	const char *dlg_metal_fail;
	const char *dlg_d3d_fail_fmt;    // 0x%08lx: the HRESULT
	const char *dlg_card_create_fail;
	const char *dlg_fresh_card;      // may contain \n
	const char *dlg_roms_needed;
	const char *dlg_roms_bad_fmt;
	const char *dlg_roms_pick;
	const char *dlg_roms_quit;
	const char *dlg_roms_ok_cancel;
	const char *dlg_cancel;
	// .syx file notes, shown in the master editor (produced by the
	// pc_window backends and master_editor.cpp).
	const char *note_exported_fmt;   // %zu
	const char *note_export_fail;
	const char *note_imported;
	const char *note_import_fail;
	const char *note_sysex_busy_fmt; // %zu
	const char *note_sysex_idle;
	// XG editor shared (xg_ui.cpp): part parameter groups, the voice
	// picker and the effect category names. Category labels are matched
	// by their Japanese name (never blank on unknown ones).
	const char *xgui_group_voice;
	const char *xgui_group_vol;
	const char *xgui_group_rcv;
	const char *xgui_group_filter;
	const char *xgui_group_peg;
	const char *xgui_group_porta;
	const char *xgui_group_vib;
	const char *xgui_group_eq;
	const char *xgui_group_mod;
	const char *xgui_group_bend;
	const char *xgui_bend_now_fmt;
	const char *xgui_group_cat;
	const char *xgui_group_pat;
	const char *xgui_part_fmt;       // %s: A1..
	const char *xgui_kit_title_fmt;  // %s + MSB %d
	const char *xgui_kit_drum;
	const char *xgui_kit_sfx;
	const char *xgui_no_rom_names;
	const char *xgui_bank_diff_fmt;  // %d
	const char *xgui_no_bank_kit;
	const char *xgui_drum_pick_kit;
	const char *xgui_no_bank_norom;
	const char *xgui_no_bank;
	const char *xgui_other;
	const char *fxcat_reverb;
	const char *fxcat_early;
	const char *fxcat_delay;
	const char *fxcat_karaoke;
	const char *fxcat_chorus;
	const char *fxcat_flange;
	const char *fxcat_rotary;
	const char *fxcat_dist;
	const char *fxcat_eq;
	const char *fxcat_comp;
	const char *fxcat_combo;
	const char *fxcat_lofi;
	const char *fxcat_pitch;
	const char *fxcat_other;
	// Effect words shared by the overview, the insertion editor and the
	// master editor (same meaning everywhere).
	const char *fx_kind;
	const char *fx_part;
	const char *fx_connect;
	const char *sys_reverb;
	const char *sys_chorus;
	const char *sys_variation;
	// PC editor (pc_editor.cpp).
	const char *ed_slider_tip_fmt;   // %s %s + how to turn
	const char *ed_col_part;
	const char *ed_col_rcv;
	const char *ed_col_voice;
	const char *ed_col_vol;
	const char *ed_tab_mixer;
	const char *ed_tab_part;
	const char *ed_tab_drum;
	const char *ed_tab_sysex;
	const char *sxd_hint;
	const char *sxd_clear;
	const char *sxd_send_all_out;
	const char *sxd_send_all_in;
	const char *sxd_sent_fmt;
	const char *sxd_send_out;
	const char *sxd_send_out_tip;
	const char *sxd_send_in;
	const char *sxd_send_in_tip;
	const char *sxd_sent_line;
	const char *sxd_played_line;
	const char *drum_used_by;
	const char *drum_none;
	const char *drum_reset;
	const char *drum_names_from_fmt;
	const char *drum_names_hint;
	const char *drum_plain_note;
	const char *drum_dblclick_hint;
	const char *drum_no_user_hint;
	const char *ed_knobs_on;
	const char *ed_knobs_off;
	// Part voice window (part_shapes.cpp).
	const char *ps_graph;
	const char *ps_knobs;
	const char *ps_hint_knobs;       // may contain \n
	const char *ps_hint_graph;       // may contain \n
	const char *ps_tab_shape;
	const char *ps_tab_all;
	const char *ps_tab_drum;
	const char *ps_out_label;
	const char *ps_out_panel;
	const char *ps_out_hint;
	const char *ps_out_sent_fmt;
	const char *ps_out_no_rcv;
	const char *ps_drum_not;
	const char *ps_drum_plain;
	const char *ps_drum_mode_hint;
	const char *ps_drum_play;
	const char *ps_drum_follow;
	const char *ps_drum_title_mix;
	const char *ps_drum_title_filter;
	const char *ps_drum_title_env;
	const char *ps_drum_no_shape;
	const char *ps_drum_env_fmt;
	const char *ps_drum_pitch_fmt;
	const char *ps_title_vib;
	const char *ps_title_wobble;
	const char *ps_about_wobble;
	const char *ps_bend_hint_fmt;
	const char *ps_title_mod;
	const char *ps_title_filterenv;
	const char *ps_title_env;
	const char *ps_about_vib;        // may contain \n
	const char *ps_about_mod;        // may contain \n
	const char *ps_about_filterenv;  // may contain \n
	const char *ps_about_env;        // may contain \n
	const char *ps_fx_ins_fmt;       // %d %d
	const char *ps_fx_var_ins;
	const char *ps_fx_var;
	const char *ps_fx_cho;
	const char *ps_fx_rev;
	const char *ps_about_fx_part;    // may contain \n
	const char *ps_about_fx_var;     // may contain \n
	const char *ps_about_fx_sys;     // may contain \n
	const char *ps_title_route;
	const char *ps_about_route;      // may contain \n
	const char *ps_tab_matrix;
	const char *ps_hint_bar;
	// Insertion editor (fx_editor.cpp); kind/part covered by fx_kind/fx_part.
	const char *fxe_slider_tip_fmt;  // %s %s + how to turn
	const char *fxe_noeffect;
	const char *fxe_thru;
	const char *fxe_no_table;
	// Master editor (master_editor.cpp).
	const char *me_sys;
	const char *me_tune_note;        // may contain \n
	const char *me_fx;
	const char *me_params;
	const char *me_knobs_open;
	const char *me_back;
	const char *me_pan;
	const char *me_to_rev;
	const char *me_to_cho;
	const char *me_insert_note;
	const char *me_master_eq;
	const char *me_eq_type_note;
	const char *me_band_peak_fmt;    // %d
	const char *me_band_shape1;
	const char *me_band_shape5;
	const char *me_band_fmt;         // %d
	const char *me_w_gain;
	const char *me_w_freq;
	const char *me_w_q;
	const char *me_export;
	const char *me_import;
	const char *me_no_dialog;
	const char *me_diff_only;
	const char *me_diff_only_tip;    // may contain \n
	const char *me_reading_defaults;
	const char *me_sysex_title;
	// Overview list (overview.cpp). Column headers that double as help
	// keys keep their Japanese key (see headers_with_help call sites).
	const char *ov_bighint;          // may start with \n
	const char *ov_bighint_master;   // may start with \n
	const char *ov_var_off_fmt;      // %s %s + why unused
	const char *ov_slider_fmt;       // %s %s + how to turn
	const char *ov_slider_ro_fmt;    // %s %s, display only
	const char *ov_ins1;
	const char *ov_ins2;
	const char *ov_ins3;
	const char *ov_ins4;
	const char *ov_fx_for_part_fmt;  // %s: part
	const char *ov_fx_now_sys_fmt;   // %s + type
	const char *ov_fx_now_fmt;       // %s + part + type
	const char *ov_unhook;
	const char *ov_unhook_sys;
	const char *ov_hook;
	const char *ov_hook_sys;
	const char *ov_drag_note;        // may contain \n
	const char *ov_drag_note2;       // may contain \n
	const char *ov_rclick_fx;
	const char *ov_not_fx;
	const char *ov_move_fmt;         // %s %s
	const char *ov_tip_ins_fmt;      // %s %s + help, may contain \n
	const char *ov_tip_var_fmt;      // %s %s + help, may contain \n
	const char *ov_peg_hint;         // may contain \n
	const char *ov_porta_hint;       // may contain \n
	const char *ov_khz;
	const char *ov_hpf_on_fmt;       // %s %s
	const char *ov_hpf_off_fmt;      // %s
	const char *ov_filter_hint;      // may contain \n
	const char *ov_eq_pt_hint;
	const char *ov_eq_master_hint;
	const char *ov_vib_hint;         // may contain \n
	const char *ov_rcv_mute_fmt;     // %s %s
	const char *ov_mute_word;
	const char *ov_solo_off_word;
	const char *ov_rcv_fmt;          // %s %d %d
	const char *ov_range_fmt;        // %s %s %d %d
	const char *ov_kb_audition_tip;  // may contain \n
	const char *ov_kb_play_tip;
	const char *ov_mod_tip_fmt;      // %d, may contain \n
	const char *ov_off_no_part;
	const char *ov_conn_sys;
	const char *ov_conn_ins;
	const char *ov_conn_tip;
	const char *ov_fx_kind_fmt;      // %s: effect slot
	const char *ov_rclick_kind;
	const char *ov_move_tip_fmt;     // %s %s %s, may contain \n
	const char *ov_drag_to_fmt;      // %s %s: drag source
	const char *ov_master_name_tip;
	const char *ov_part_col;
	const char *ov_tip_mute;
	const char *ov_tip_solo;
	const char *ov_voices_prefix;
	const char *ov_engine_fmt;       // %s: native/firmware
	const char *ov_engine_tip;       // may contain \n
	const char *ov_voices_tip;       // may contain \n
	const char *ov_cpu_tip;
	const char *ov_zoom_label;
	const char *ov_ins_section;
	// Modulation matrix (part_shapes.cpp): row/column labels rebuilt per
	// call (cheap, always current); keys stay in the static tables.
	const char *mx_src_mw_name;
	const char *mx_src_mw_about;
	const char *mx_src_bend_name;
	const char *mx_src_bend_about;
	const char *mx_src_cat_name;
	const char *mx_src_cat_about;
	const char *mx_src_pat_name;
	const char *mx_src_pat_about;
	const char *mx_src_ac1_name;
	const char *mx_src_ac1_about;
	const char *mx_src_ac2_name;
	const char *mx_src_ac2_about;
	const char *mx_dst_pitch_name;
	const char *mx_dst_pitch_sub;
	const char *mx_dst_filter_name;
	const char *mx_dst_filter_sub;
	const char *mx_dst_amp_name;
	const char *mx_dst_amp_sub;
	const char *mx_dst_lfo_pmod_name;
	const char *mx_dst_lfo_pmod_sub;
	const char *mx_dst_lfo_fmod_name;
	const char *mx_dst_lfo_fmod_sub;
	const char *mx_dst_lfo_amod_name;
	const char *mx_dst_lfo_amod_sub;
	const char *mx_hint_cc_fmt;      // %s %s + row help
	const char *mx_hint_row_fmt;     // %s + row help
	const char *mx_hint_cell_fmt;    // %s %s %s %s %s %s
	const char *mx_dst_sub_fmt;      // %s %s
	// Second-wave overview strings (upstream part-voice rework).
	const char *ov_value_tip_fmt;    // %s %s %s + drag/wheel
	const char *ov_discrete;
	const char *ov_legend_combined;
	const char *ov_legend_filter;
	const char *ov_env_vol_long;     // Attack %.0f, Release %.0f
	const char *ov_env_vol;          // Attack/Decay/Release %.0f
	const char *ov_env_pitch;        // Init/Attack/Release/Rel %.0f
	const char *ov_mod_wheel_hint;   // %d, may contain \n
	const char *ov_param_tip_fmt;    // %s %d %s + drag/wheel
	const char *ov_wheel_fmt;        // %d %.0f
	const char *ov_mw_fmt;           // %d %.0f
	const char *ov_own_fmt;          // %.0f
	// Variation switches, fx panels, flow graphics (upstream part-voice
	// rework). Node names resolve through ps_flow_node().
	const char *ps_var_part_hint;    // %s
	const char *ps_var_ins_hint;     // %s
	const char *ps_var_cap_sys;
	const char *ps_fx_legend_part;
	const char *ps_fx_legend_sys;
	const char *ps_fx_other_part_fmt; // %s: part
	const char *ps_fx_other_none;
	const char *ps_fx_eq_title;      // EQ overlay caption (may contain ±)
	const char *ps_fx_wah_touch_fmt; // %s: top frequency
	const char *ps_fx_wah_lfo_fmt;   // %s: top frequency
	const char *ps_fx_type_fallback;
	const char *ps_fx_details;
	const char *ps_fx_details_hint;  // may contain \n
	const char *ps_fx_viewonly_fmt;  // %s %s
	const char *ps_fx_param_fmt;     // %s %s %s
	const char *ps_node_part;
	const char *ps_node_var;
	const char *ps_node_cho;
	const char *ps_node_rev;
	const char *ps_node_out;
	const char *ps_node_ins_fmt;     // %s: part or OFF
	const char *ps_node_part_hint;
	const char *ps_node_dry_hint;
	const char *ps_var_send_idle_fmt; // %s %d + why idle
	const char *ps_why_0_mine;
	const char *ps_why_0_other;
	const char *ps_why_3_mine;
	const char *ps_why_3_other;
	const char *ps_why_45;
	const char *ps_why_7_mine;
	const char *ps_why_7_other;
	const char *ps_badge_fixed_fmt;  // %s %s %s
	const char *ps_where_dialog;
	const char *ps_where_fader;
	const char *ps_badge_fmt;        // %s %d %s
	const char *ps_pre_parallel;
	const char *ps_pre_parallel_about;
	const char *ps_pre_vcr;
	const char *ps_pre_vcr_about;
	const char *ps_pre_vr;
	const char *ps_pre_vr_about;
	const char *ps_pre_cr;
	const char *ps_pre_cr_about;
	const char *ps_flow_ways;
	const char *ps_flow_ways_hint;   // %s %s, may contain \n
	// Variation send slider + folding voice pane (upstream rework).
	const char *ps_var_send_hint;    // %s %d
	const char *ps_fold_open_hint;   // may contain \n
	const char *ps_fold_close_hint;  // may contain \n
	const char *ps_fold_char1;
	const char *ps_fold_char2;
	const char *cap_range_fmt;      // "%s %s (%d-%d)"
	const char *cap_range1_fmt;     // "%s (%d-%d)"
	const char *cap_count_fmt;      // "%s (%d)"
	const char *cap_bank_fmt;       // "%3d  %s (%d/%d)"
	const char *cap_pgm_fmt;        // "(pp pp)"
	const char *cap_pgm3_fmt;       // "(pp pp pp)"
	const char *cap_insertion_fmt;  // "%s (INSERTION -> %s)"
	// Spectrum backdrop fallback (overview.cpp).
	const char *ov_silent;
	// Sampling window (sampling_editor.cpp) and its toolbar button.
	const char *bar_sampling;
	const char *dlg_wav_desc;
	const char *smp_not_ready;
	const char *smp_input;
	const char *smp_device;
	const char *smp_device_none;
	const char *smp_device_host;
	const char *smp_source;
	const char *smp_trigger;
	const char *smp_trigger_off;
	const char *smp_record;
	const char *smp_name;
	const char *smp_record_start;
	const char *smp_stop;
	const char *smp_nothing;
	const char *smp_added_fmt;
	const char *smp_waiting;
	const char *smp_recording_fmt;
	const char *smp_free_fmt;
	const char *smp_wav_note;
	const char *smp_wav;
	const char *smp_wav_path;
	const char *smp_wav_load;
	const char *smp_wav_fail;
	const char *smp_samples;
	const char *smp_none;
	const char *smp_col_no;
	const char *smp_col_len;
	const char *smp_assign;
	const char *smp_pgm;
	const char *smp_sample;
	const char *smp_sample_none;
	const char *smp_voice_name;
	const char *smp_level;
	const char *smp_pan;
	const char *smp_pan_scaling;
	const char *smp_apply;
	const char *smp_voice_set_fmt;
	const char *smp_select_part1;
	const char *smp_play_hint_fmt;
	const char *smp_col_peak;
	const char *smp_silent;
	const char *smp_wave;
	const char *smp_wave_pick;
	const char *smp_normalize;
	const char *smp_gain_apply;
	const char *smp_gain_done_fmt;
	const char *smp_gain_note;
	const char *smp_trim_auto;
	const char *smp_trim;
	const char *smp_trim_clear;
	const char *smp_trimmed_fmt;
	const char *smp_trim_tip;
	const char *smp_trim_start;
	const char *smp_trim_end;
	const char *smp_zoom;
	const char *smp_zoom_all;
	const char *smp_zoom_sel;
	const char *smp_coarse;
	const char *smp_fine;
	const char *smp_play;
	const char *smp_play_stop;
	const char *smp_play_tip;
	const char *smp_loop;
	const char *smp_loop_tip;
	const char *smp_loop_at;
	const char *smp_trim_note;
	const char *smp_find;
	const char *smp_finding;
	const char *smp_find_min;
	const char *smp_find_tip;
	const char *smp_find_done_fmt;
	const char *smp_find_fail;
	const char *smp_xfade_gain;
	const char *smp_xfade_power;
	const char *smp_xfade_curve_tip;
	const char *smp_audition;
	const char *smp_audition_tip;
	const char *smp_audition_key;
	const char *smp_det_start;
	const char *smp_det_end;
	const char *smp_det_loop;
	const char *smp_det_loop_off;
	const char *smp_det_tip;
	const char *smp_snap;
	const char *smp_snap_tip;
	const char *smp_match;
	const char *smp_match_tip;
	const char *smp_match_done_fmt;
	const char *smp_xfade;
	const char *smp_xfade_tip;
	const char *smp_xfade_done_fmt;
	const char *smp_xfade_fail;
	const char *smp_env;
	const char *smp_env_tip;
	const char *smp_env_attack;
	const char *smp_env_decay1;
	const char *smp_env_level1;
	const char *smp_env_decay2;
	const char *smp_env_level2;
	const char *smp_env_release;
};

// Each language file defines one of these (never included directly).
// Adding a language: append X(xx, "...") to UI_LANG_LIST in ui/lang.h,
// add texts_xx.h defining xx_texts(), include it here. The switch below
// derives from the same list, so a missing table is a compile error and
// tools/check_texts.py verifies the rest.
#include "ui/texts_ja.h"
#include "ui/texts_en.h"

inline const ui_texts &texts()
{
	switch (get_lang()) {
#define X(code, name) \
	case lang::code:  \
		return code##_texts();
		UI_LANG_LIST(X)
#undef X
	default:
		break;
	}
	return en_texts(); // unreachable, and the English fallback
}

// Reading a call site: UI_TEXT(id, "default") is texts().id, with the
// English default spelled out so the code reads without opening the table
// (NSLocalizedString-style, minus the comment). The default must equal
// texts_en.h's entry -- tools/check_texts.py compares them, so the two
// cannot drift. New string: write UI_TEXT once, run the checker, paste
// the struct/ja/en lines it prints, fill in the Japanese.
#define UI_TEXT(id, en_default) (ui::texts().id)

} // namespace ui

#endif // S_MU2000_UI_TEXTS_H
