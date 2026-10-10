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
// The texts themselves are in locale/<code>/*.json (one folder per language);
// tools/locale_tool.py generates one table per language from them
// (texts_<code>.h, each a function returning ui_texts with one designated
// .field per member, in struct order). To add a string, add the member here,
// its line in locale/en and locale/ja, and run `python tools/locale_tool.py gen`.
// To add a language, see locale/README.md: no source file is edited.
//
// tools/check_texts.py checks all of this: every table must define exactly
// the struct's field set, every *_fmt must carry the same printf sequences in
// every language, and the generated files must be what locale/ produces.
//
// Header-only on purpose: no build file on any platform needs a new source.

#ifndef S_MU2000_UI_TEXTS_H
#define S_MU2000_UI_TEXTS_H

#pragma once

#include "ui/lang.h"
#include "ui/locale_file.h"

#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace ui {

struct ui_texts {
	// Bottom strip tabs (panel.cpp)
	const char *tab_panel;
	const char *tab_editor;
	const char *tab_effects;
	// Front page hint (panel.cpp)
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
	const char *engine_restarting;
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
	const char *menu_in_e;
	const char *menu_unused;
	const char *menu_no_devices;
	const char *menu_ain_title;
	const char *menu_no_ain;
	const char *menu_audio_title;
	const char *menu_audio_default;
	const char *menu_no_audio;
	// iOS-only menu rows (Bluetooth/network setup, ROM import). Localized like
	// every other row even though no desktop front end offers them, so their text
	// is in locale/<code>/menus.json like the rest - the comment here becomes the
	// note a translator reads.
	const char *menu_bt_title;
	const char *menu_bt_connect;
	const char *menu_bt_advertise;
	const char *menu_net_midi;
	const char *menu_roms_title;
	const char *menu_roms_install;
	const char *audio_switch_failed_fmt; // %s: device and backend error
	const char *menu_out_mu;
	const char *menu_thru_a;
	const char *menu_thru_b;
	const char *menu_open_list;
	const char *menu_open_editor;
	const char *menu_native_fx;
	const char *menu_native_engine;
	const char *menu_factory;
	const char *menu_restart;
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
	const char *dlg_roms_installed;
	const char *dlg_cancel;
	// .syx file notes, shown in the master editor (produced by the
	// pc_window backends and master_editor.cpp).
	const char *note_exported_fmt;   // %zu
	const char *note_export_fail;
	const char *note_imported;
	const char *note_import_fail;
	// iOS only: a picked file cannot be kept where it is (the grant dies with
	// the process), so a copy lands in the app's own Documents folder, which
	// Files shows as "On My iPhone > S-MU2000". %s is the folder name
	const char *note_kept_fmt;
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
	const char *ps_out_label_ctrl;
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
	const char *ps_board_note;
	const char *sb_use_board_fmt;
	const char *sb_use_board_tip;
	const char *sb_back;
	const char *sb_back_tip;
	const char *sb_user_no_programs;
	const char *sb_note;
	const char *sb_wave;
	const char *sb_user_empty;
	const char *sb_env;
	const char *sb_user_note;
	const char *sb_tab;
	const char *fce_set_tip;
	const char *fce_wave;
	const char *fce_w_square;
	const char *fce_w_triangle;
	const char *fce_w_noise;
	const char *fce_w_metal;
	const char *fce_wave_tip;
	const char *fce_duty_len;
	const char *fce_duty_len_tip;
	const char *fce_duty_tip;
	const char *fce_duty_frames;
	const char *fce_frames_tip;
	const char *fce_volume;
	const char *fce_tri_note;
	const char *fce_decay;
	const char *fce_decay_fmt;
	const char *fce_decay_off;
	const char *fce_decay_tip;
	const char *fce_floor;
	const char *fce_floor_off;
	const char *fce_floor_tip;
	const char *fce_release;
	const char *fce_release_fmt;
	const char *fce_release_tip;
	const char *fce_pitch;
	const char *fce_arp_len;
	const char *fce_arp_len_tip;
	const char *fce_arp_tip;
	const char *fce_arp_frames;
	const char *fce_sweep;
	const char *fce_sweep_fmt;
	const char *fce_sweep_tip;
	const char *fce_sweep_frames;
	const char *fce_frames_fmt;
	const char *fce_sweep_frames_tip;
	const char *fce_vib;
	const char *fce_vib_fmt;
	const char *fce_off;
	const char *fce_vib_tip;
	const char *fce_vib_delay;
	const char *fce_vib_delay_tip;
	const char *fce_note;
	const char *fce_cc_fmt;
	const char *fce_cc_tip;
	const char *fce_cc_drop;
	const char *fce_cc_drop_tip;
	const char *fce_cc_assign;
	const char *fce_cc_assign_tip;
	const char *fce_cc_duty_fmt;
	const char *fce_cc_arp_fmt;
	const char *fce_cc_defaults;
	const char *fce_cc_assign_note;
	const char *fme_default_set;
	const char *fme_set_tip;
	const char *fme_new_set;
	const char *fme_name_taken;
	const char *fme_program_fmt;
	const char *fme_reset_voice;
	const char *fme_reset_tip;
	const char *fme_copy;
	const char *fme_paste;
	const char *fme_alg;
	const char *fme_alg_tip;
	const char *fme_feedback;
	const char *fme_feedback_tip;
	const char *fme_drop;
	const char *fme_drop_tip;
	const char *fme_noise;
	const char *fme_noise_tip;
	const char *fme_op_carrier;
	const char *fme_op_mod;
	const char *fme_ratio;
	const char *fme_ratio_tip;
	const char *fme_level;
	const char *fme_level_car_tip;
	const char *fme_level_mod_tip;
	const char *fme_attack;
	const char *fme_decay;
	const char *fme_decay_tip;
	const char *fme_sustain;
	const char *fme_release;
	const char *fme_save_fail;
	const char *fme_drum_note;
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
	const char *me_board_title;
	const char *me_board_open;
	const char *me_board_open_tip;
	const char *me_board_no_parts;
	const char *me_board_none;
	const char *me_board_fc;
	const char *me_board_fc16;
	const char *me_board_dls;
	const char *me_board_user;
	const char *me_board_user16;
	const char *me_board_fm16;
	const char *me_board_user_none;
	const char *me_board_user_tip;
	const char *me_board_user_empty;
	const char *me_board_user_count_fmt;
	const char *me_board_parts;
	const char *me_board_parts_note;
	const char *me_bp_ch;
	const char *me_bp_voice;
	const char *me_bp_bank;
	const char *me_bp_program;
	const char *me_bp_volume;
	const char *me_bp_pan;
	const char *me_bp_reverb;
	const char *me_bp_chorus;
	const char *me_bp_level;
	const char *me_bp_drum;
	const char *me_bp_insert;
	const char *me_bp_variation;
	const char *me_bp_insert_tip;
	const char *ov_board_row_tip;
	const char *ov_board_sub_fmt;
	const char *ov_board_ins_none;
	const char *ov_board_no_voices;
	const char *me_board_dls_open;
	const char *me_board_dls_path;
	const char *me_board_dls_load;
	const char *me_board_dls_tip;
	const char *me_board_dls_error_fmt;
	const char *me_board_dls_empty;
	const char *me_board_dls_loaded_fmt;
	const char *dlg_dls_desc;
	const char *me_board_port_e;
	const char *me_board_tip;
	const char *me_board_part;
	const char *me_board_booting;
	const char *me_board_idle;
	const char *me_board_one_multi;
	const char *me_board_unknown;
	const char *me_board_off;
	const char *me_board_known;
	const char *me_board_extra_head;
	const char *me_board_extra_note;
	const char *me_board_extra_tip;
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
	const char *bar_player;
	const char *bar_board;
	const char *ply_idle;
	const char *ply_prev;
	const char *ply_play;
	const char *ply_pause;
	const char *ply_stop;
	const char *ply_next;
	const char *ply_loop_none;
	const char *ply_loop_all;
	const char *ply_loop_one;
	const char *ply_loop_shuffle;
	const char *ply_loop_tip;
	const char *ply_beat_fmt;
	const char *ply_chasing;
	const char *ply_paused;
	const char *ply_col_song;
	const char *ply_col_len;
	const char *ply_add;
	const char *ply_clear;
	const char *ply_path;
	const char *ply_add_path;
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
	const char *smp_rom_waves;
	const char *smp_rom_wave_find;
	const char *smp_rom_wave_tip;
	const char *smp_tab_library;
	const char *lib_saved;
	const char *lib_cannot_write;
	const char *lib_cat_all;
	const char *lib_cat_none;
	const char *lib_find;
	const char *lib_rescan;
	const char *lib_col_name;
	const char *lib_col_cat;
	const char *lib_col_waves;
	const char *lib_save_head;
	const char *lib_from_fmt;
	const char *lib_name;
	const char *lib_category;
	const char *lib_category_hint;
	const char *lib_memo;
	const char *lib_save;
	const char *lib_save_tip;
	const char *lib_over_title;
	const char *lib_over_text;
	const char *lib_sel_head;
	const char *lib_empty;
	const char *lib_pick;
	const char *lib_apply;
	const char *lib_exists;
	const char *lib_delete;
	const char *lib_delete_fmt;
	const char *lib_el_sample;
	const char *lib_el_fmt;
	const char *lib_el_off_fmt;
	const char *lib_samples_fmt;
	const char *lib_no_samples;
	const char *lib_load_head;
	const char *lib_to;
	const char *lib_to_emu;
	const char *lib_to_hw;
	const char *lib_number;
	const char *lib_load;
	const char *lib_loaded_fmt;
	const char *lib_load_tip;
	const char *lib_open_voice;
	const char *lib_audition_no;
	const char *lib_audition_tip;
	const char *lib_send;
	const char *lib_send_no;
	const char *lib_send_tip;
	const char *lib_no_out;
	const char *lib_hw_note;
	const char *smp_tab_presets;
	const char *rw_voice_tip;
	const char *pv_find;
	const char *pv_count_fmt;
	const char *pv_col_name;
	const char *pv_col_els;
	const char *pv_bank_fmt;
	const char *pv_inner_fmt;
	const char *pv_el_fmt;
	const char *pv_row_play;
	const char *pv_row_wave;
	const char *pv_wave_tip;
	const char *pv_row_keys;
	const char *pv_row_vel;
	const char *pv_row_level;
	const char *pv_row_pan;
	const char *pv_row_pitch;
	const char *pv_row_filter;
	const char *pv_row_aeg;
	const char *pv_row_aeg_lv;
	const char *pv_row_lfo;
	const char *pv_row_lfo_depth;
	const char *pv_row_delay;
	const char *pv_all;
	const char *pv_only_fmt;
	const char *pv_play_tip;
	const char *pv_vel;
	const char *pv_silent;
	const char *pv_map_tip_fmt;
	const char *pv_map_note;
	const char *pv_copy_fmt;
	const char *pv_copy_tip;
	const char *smp_tab_romwave;
	const char *smp_rom_wave_browse;
	const char *smp_rom_wave_browse_tip;
	const char *rw_fmt_packed;
	const char *rw_find;
	const char *rw_kind_all;
	const char *rw_kind_voice;
	const char *rw_kind_drum;
	const char *rw_kind_unused;
	const char *rw_count_fmt;
	const char *rw_col_used;
	const char *rw_unused;
	const char *rw_summary_fmt;
	const char *rw_noname;
	const char *rw_voices;
	const char *rw_drums;
	const char *rw_none;
	const char *rw_inner_tip;
	const char *rw_kit_more_fmt;
	const char *rw_kit_fmt;
	const char *rw_col_keys;
	const char *rw_col_base;
	const char *rw_col_len;
	const char *rw_col_loop;
	const char *rw_col_fmt;
	const char *rw_loop_back;
	const char *rw_loop_yes;
	const char *rw_loop_no;
	const char *rw_play_tip;
	const char *rw_auto;
	const char *rw_auto_tip;
	const char *rw_use_fmt;
	const char *rw_use_tip;
	const char *rw_zone_fmt;
	const char *rw_loading;
	const char *rw_zoom_loop_fmt;
	const char *rw_zoom_head_fmt;
	const char *smp_sx_save;
	const char *smp_sx_send;
	const char *smp_sx_tip;
	const char *smp_sx_sending_fmt;
	const char *smp_sx_stop;
	const char *smp_mem_save;
	const char *smp_mem_send;
	const char *smp_mem_tip;
	const char *smp_mem_warn;
	const char *smp_mem_time_fmt;
	const char *smp_syx_load;
	const char *smp_syx_tip;
	const char *smp_syx_warn;
	const char *smp_syx_done_fmt;
	const char *smp_syx_none;
	const char *smp_element;
	const char *smp_element_on;
	const char *smp_element_tip;
	const char *smp_filter;
	const char *smp_cutoff;
	const char *smp_resonance;
	const char *smp_hpf;
	const char *smp_vel_curve;
	const char *smp_vel_curve_tip;
	const char *smp_lfo_wave;
	const char *smp_lfo_saw;
	const char *smp_lfo_tri;
	const char *smp_lfo_sh;
	const char *smp_lfo_init;
	const char *smp_lfo_init_tip;
	const char *smp_lfo_speed;
	const char *smp_lfo_delay;
	const char *smp_lfo_pitch;
	const char *smp_lfo_filter;
	const char *smp_lfo_amp;
	const char *smp_peg;
	const char *smp_peg_depth;
	const char *smp_peg_tip;
	const char *smp_feg;
	const char *smp_feg_tip;
	const char *smp_eg_attack;
	const char *smp_eg_decay1;
	const char *smp_eg_decay2;
	const char *smp_eg_release;
	const char *smp_eg_l0;
	const char *smp_eg_l1;
	const char *smp_eg_l2;
	const char *smp_eg_l3;
	const char *smp_eg_l4;
	const char *smp_key_range;
	const char *smp_vel_range;
	const char *smp_tab_make;
	const char *smp_tab_prepare;
	const char *smp_tab_record;
	const char *smp_tab_process;
	const char *smp_tab_voice;
	const char *smp_make;
	const char *smp_make_basic;
	const char *smp_make_bars;
	const char *smp_make_draw;
	const char *smp_make_noise;
	const char *smp_make_fc;
	const char *smp_make_fm;
	const char *smp_make_fc_p12;
	const char *smp_make_fc_p25;
	const char *smp_make_fc_p50;
	const char *smp_make_fc_p75;
	const char *smp_make_fc_tri;
	const char *smp_make_fc_noise;
	const char *smp_make_fc_noise_short;
	const char *smp_make_fc_note;
	const char *smp_make_fm_epiano;
	const char *smp_make_fm_bell;
	const char *smp_make_fm_brass;
	const char *smp_make_fm_organ;
	const char *smp_make_fm_bass;
	const char *smp_make_fm_clav;
	const char *smp_make_fm_c;
	const char *smp_make_fm_m;
	const char *smp_make_fm_index;
	const char *smp_make_fm_fb;
	const char *smp_make_fm_tip;
	const char *smp_make_presets;
	const char *smp_make_organ;
	const char *smp_make_unison;
	const char *smp_make_vowel;
	const char *smp_make_sync;
	const char *smp_make_fold;
	const char *smp_make_bars_random;
	const char *smp_make_draw_steps;
	const char *smp_make_draw_bits;
	const char *smp_make_draw_chip_tip;
	const char *smp_make_noise_white;
	const char *smp_make_noise_pink;
	const char *smp_make_noise_brown;
	const char *smp_make_organ_full;
	const char *smp_make_organ_jazz;
	const char *smp_make_organ_gospel;
	const char *smp_make_organ_flute;
	const char *smp_make_organ_reed;
	const char *smp_make_organ_note;
	const char *smp_make_uni_voices;
	const char *smp_make_uni_detune;
	const char *smp_make_uni_note;
	const char *smp_make_vowel_pos;
	const char *smp_make_vowel_note;
	const char *smp_make_sync_ratio;
	const char *smp_make_sync_note;
	const char *smp_make_fold_gain;
	const char *smp_make_fold_bias;
	const char *smp_make_fold_note;
	const char *smp_make_lofi;
	const char *smp_make_lofi_tip;
	const char *smp_make_view_multi_fmt;
	const char *smp_make_pwm;
	const char *smp_make_pluck;
	const char *smp_make_drum;
	const char *smp_make_pwm_center;
	const char *smp_make_pwm_depth;
	const char *smp_make_pwm_sweeps;
	const char *smp_make_pwm_note;
	const char *smp_make_pluck_sustain;
	const char *smp_make_pluck_bright;
	const char *smp_make_length;
	const char *smp_make_pluck_again;
	const char *smp_make_pluck_note;
	const char *smp_make_drum_kick;
	const char *smp_make_drum_snare;
	const char *smp_make_drum_tom;
	const char *smp_make_drum_hat;
	const char *smp_make_drum_clap;
	const char *smp_make_drum_cowbell;
	const char *smp_make_drum_tune;
	const char *smp_make_drum_decay;
	const char *smp_make_drum_tone;
	const char *smp_make_drum_note;
	const char *smp_make_view_once_fmt;
	const char *smp_make_added_once_fmt;
	const char *smp_make_pd;
	const char *smp_make_bell;
	const char *smp_make_pd_reso;
	const char *smp_make_pd_amount;
	const char *smp_make_pd_ratio;
	const char *smp_make_pd_note;
	const char *smp_make_bell_tubular;
	const char *smp_make_bell_glass;
	const char *smp_make_bell_gong;
	const char *smp_make_bell_marimba;
	const char *smp_make_bell_ratio;
	const char *smp_make_bell_decay;
	const char *smp_make_bell_note;
	const char *smp_make_vowel_morph;
	const char *smp_make_vowel_to;
	const char *smp_make_sine;
	const char *smp_make_saw;
	const char *smp_make_square;
	const char *smp_make_tri;
	const char *smp_make_pulse;
	const char *smp_make_bars_one;
	const char *smp_make_bars_all;
	const char *smp_make_bars_odd;
	const char *smp_make_bars_tip;
	const char *smp_make_draw_sine;
	const char *smp_make_draw_flat;
	const char *smp_make_draw_tip;
	const char *smp_make_noise_note;
	const char *smp_make_max_h;
	const char *smp_make_max_h_tip;
	const char *smp_make_level;
	const char *smp_make_view_note;
	const char *smp_make_play_tip;
	const char *smp_make_add;
	const char *smp_make_added_fmt;
	const char *smp_make_add_tip;
	const char *smp_make_assign_fmt;
	const char *smp_make_assign_tip;
	const char *smp_ub_title;
	const char *smp_ub_new;
	const char *smp_ub_intro;
	const char *smp_ub_board_name;
	const char *smp_ub_name_empty;
	const char *smp_ub_name_taken;
	const char *smp_ub_board_name_tip;
	const char *smp_ub_program;
	const char *smp_ub_replace;
	const char *smp_ub_put;
	const char *smp_ub_put_tip;
	const char *smp_ub_col_wave;
	const char *smp_ub_col_attack;
	const char *smp_ub_col_decay;
	const char *smp_ub_col_sustain;
	const char *smp_ub_col_release;
	const char *smp_ub_loop_fmt;
	const char *smp_ub_once_fmt;
	const char *smp_ub_remove;
	const char *smp_ub_table_tip;
	const char *smp_ub_save_fail;
	const char *smp_ub_footer_fmt;
	const char *smp_ub_how;
	const char *smp_tab_edit;
	const char *smp_tab_card;
	const char *smp_card;
	const char *smp_card_read_fail;
	const char *smp_card_loaded;
	const char *smp_card_slot;
	const char *smp_card_none;
	const char *smp_card_image;
	const char *smp_card_open;
	const char *smp_card_path;
	const char *smp_card_open_path;
	const char *smp_card_reload;
	const char *smp_card_reading;
	const char *smp_card_empty;
	const char *smp_card_col_file;
	const char *smp_card_col_size;
	const char *smp_card_no_waves;
	const char *smp_card_yes;
	const char *smp_card_play_tip;
	const char *smp_card_key;
	const char *smp_card_insert;
	const char *smp_card_insert_tip;
	const char *smp_card_loading;
	const char *smp_card_load;
	const char *smp_card_load_tip;
	const char *smp_card_load_warn;
	const char *smp_card_write_fail;
	const char *smp_card_too_big;
	const char *smp_card_m2a_tip;
	const char *smp_card_m2a_warn;
	const char *dlg_card_or_m2a_desc;
	const char *settings_general;
	const char *settings_language;
	const char *settings_resampler;
	const char *settings_sinc;
	const char *settings_nearest;
	const char *settings_title;
	const char *settings_audio;
	const char *settings_midi;
	const char *settings_midi_inputs;
	const char *settings_midi_outputs;
	const char *settings_disconnected;
	const char *settings_emulation;
	const char *settings_native_fx;
	const char *settings_thin_bends;
	const char *settings_native_engine;
	const char *settings_native_fx_tip;
	const char *settings_thin_bends_tip;
	const char *settings_native_engine_tip;
	const char *settings_exclusive;
	const char *settings_rate;
	const char *settings_auto;
	const char *settings_left;
	const char *settings_right;
	const char *settings_buffer;
	const char *settings_latency;
	const char *settings_opening;
	const char *settings_volume;
	const char *settings_dc;
	const char *settings_limiter;
	const char *settings_reset;
	const char *settings_panic;
};

// Each language has one table function, <code>_texts(), in ui/texts_<code>.h.
// Those files are generated from locale/<code>/*.json by tools/locale_tool.py
// (do not edit them; tools/check_texts.py fails when they and locale/ disagree).
#include "ui/texts_tables.inc"

namespace texts_detail {

// Every member of ui_texts by name (generated from the struct above), so that a
// translation file can address a text by its key.
struct field {
	const char *name;
	const char *ui_texts::*member;
};
inline constexpr field FIELDS[] = {
#define X(f) { #f, &ui_texts::f },
#include "ui/texts_fields.inc"
#undef X
};

inline const ui_texts &built_in(lang l)
{
	switch (l) {
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

// The tables the program shows: the built-in ones, with the texts of any
// translation files found in the settings folder laid over them.
//
//   <settings folder>/locale/<code>/*.json   (on Windows: %LOCALAPPDATA%/S-MU2000/locale/en/)
//
// Same files as locale/<code>/ in the repository. This is how a translation is
// tried without building: copy a language's folder there, change the texts,
// start the program again. Only keys that exist are taken, and a text whose
// printf conversions differ from the built-in one is refused (it would crash
// where it is formatted); both are said on stderr. Read once, at the first use.
struct tables {
	ui_texts t[NLANG];
	std::deque<std::string> keep;         // the overriding texts (the tables point into these)
	tables()
	{
		for (int i = 0; i < NLANG; i++)
			t[i] = built_in(lang(i));
		const std::string base = smu2000::config_dir();
		if (base.empty())
			return;
		for (int i = 0; i < NLANG; i++) {
			const std::string dir = smu2000::join(smu2000::join(base, "locale"), LANG_CODES[i]);
			if (!smu2000::is_dir(dir))
				continue;
			int taken = 0;
			for (const smu2000::dir_entry &e : smu2000::list_dir(dir)) {
				if (e.name.size() < 6 || e.name.compare(e.name.size() - 5, 5, ".json") != 0)
					continue;
				std::string text;
				std::vector<std::pair<std::string, std::string>> pairs;
				int line = 0;
				if (!locale_file::read(smu2000::join(dir, e.name), text)) {
					std::fprintf(stderr, "locale/%s/%s: cannot read\n", LANG_CODES[i], e.name.c_str());
					continue;
				}
				if (!locale_file::parse(text, pairs, line))
					std::fprintf(stderr, "locale/%s/%s: line %d: not understood (the texts above it are used)\n",
					             LANG_CODES[i], e.name.c_str(), line);
				for (auto &kv : pairs) {
					const field *f = nullptr;
					for (const field &x : FIELDS)
						if (kv.first == x.name) {
							f = &x;
							break;
						}
					if (!f) {
						std::fprintf(stderr, "locale/%s/%s: \"%s\" is not a text of this version\n",
						             LANG_CODES[i], e.name.c_str(), kv.first.c_str());
						continue;
					}
					if (locale_file::printf_shape(kv.second.c_str()) != locale_file::printf_shape(t[i].*(f->member))) {
						std::fprintf(stderr, "locale/%s/%s: \"%s\" must keep the same %%d / %%s as the original; not used\n",
						             LANG_CODES[i], e.name.c_str(), kv.first.c_str());
						continue;
					}
					keep.push_back(std::move(kv.second));
					t[i].*(f->member) = keep.back().c_str();
					taken++;
				}
			}
			if (taken)
				std::fprintf(stderr, "locale/%s: %d text(s) taken from %s\n", LANG_CODES[i], taken, dir.c_str());
		}
	}
};

} // namespace texts_detail

inline const ui_texts &texts()
{
	static const texts_detail::tables all;
	const int i = int(get_lang());
	return all.t[i >= 0 && i < NLANG ? i : int(lang::en)];
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
