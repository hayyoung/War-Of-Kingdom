/* $Id: multiplayer.cpp 47847 2010-12-05 21:12:23Z shadowmaster $ */
/*
   Copyright (C) 2007 - 2010
   Part of the Battle for Wesnoth Project http://www.wesnoth.org

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "global.hpp"

#include "dialogs.hpp"
#include "gettext.hpp"
#include "game_preferences.hpp"
#include "log.hpp"
#include "gui/dialogs/lobby_main.hpp"
#include "gui/dialogs/message.hpp"
#include "gui/dialogs/mp_connect.hpp"
#include "gui/dialogs/mp_create_game.hpp"
#include "gui/dialogs/mp_side_creator.hpp"
#include "gui/dialogs/mp_side_wait.hpp"
#include "gui/dialogs/mp_login.hpp"
#include "gui/dialogs/preferences.hpp"
#include "gui/widgets/settings.hpp"
#include "gui/widgets/window.hpp"
#include "hash.hpp"
#include "multiplayer.hpp"
#include "multiplayer_error_codes.hpp"
#include "playmp_controller.hpp"
#include "playcampaign.hpp"
#include "formula_string_utils.hpp"
#include "sound.hpp"
#include "resources.hpp"
#include "foreach.hpp"

#include <boost/bind.hpp>

static lg::log_domain log_network("network");
#define LOG_NW LOG_STREAM(info, log_network)

namespace {

class network_game_manager
{
public:
	// Add a constructor to avoid stupid warnings with some versions of GCC
	network_game_manager() {};

	~network_game_manager()
	{
		if(network::nconnections() > 0) {
			LOG_NW << "sending leave_game\n";
			config cfg;
			cfg.add_child("leave_game");
			network::send_data(cfg, 0);
			LOG_NW << "sent leave_game\n";
		}
	};
};

}

static void run_lobby_loop(display& disp, mp::ui& ui)
{
	disp.video().modeChanged();
	bool first = true;
	font::cache_mode(font::CACHE_LOBBY);
	while (ui.get_result() == mp::ui::CONTINUE) {
		if (disp.video().modeChanged() || first) {
			SDL_Rect lobby_pos = create_rect(0
					, 0
					, disp.video().getx()
					, disp.video().gety());
			ui.set_location(lobby_pos);
			first = false;
		}
		// process network data first so user actions can override the result
		// or uptodate data can prevent invalid actions
		// i.e. press cancel while you receive [start_game] or press start game while someone leaves
		ui.process_network();

		events::pump();
		events::raise_process_event();
		events::raise_draw_event();

		disp.flip();
		disp.delay(20);
	}
	font::cache_mode(font::CACHE_GAME);
}

namespace gui2 {
extern void check_response(network::connection res, const config& data);
}

namespace {

enum server_type {
	ABORT_SERVER,
	WESNOTHD_SERVER,
	SIMPLE_SERVER
};

}

static server_type open_connection(game_display& disp, const std::string& original_host)
{
	std::string h = original_host;

	if(h.empty()) {
		gui2::tmp_connect dlg;

		dlg.show(disp.video());
		if(dlg.get_retval() == gui2::twindow::OK) {
			h = preferences::network_host();
		} else {
			return ABORT_SERVER;
		}
	}

	network::connection sock;

	const int pos = h.find_first_of(":");
	std::string host;
	unsigned int port;

	if(pos == -1) {
		host = h;
		port = 15000;
	} else {
		host = h.substr(0, pos);
		port = lexical_cast_default<unsigned int>(h.substr(pos + 1), 15000);
	}

	// shown_hosts is used to prevent the client being locked in a redirect
	// loop.
	typedef std::pair<std::string, int> hostpair;
	std::set<hostpair> shown_hosts;
	shown_hosts.insert(hostpair(host, port));

	config data;
	sock = dialogs::network_connect_dialog(disp,_("Connecting to Server..."),host,port);

	do {

		if (!sock) {
			return ABORT_SERVER;
		}

		data.clear();
		network::connection data_res = dialogs::network_receive_dialog(
				disp,_("Reading from Server..."),data);
		if (!data_res) {
			return ABORT_SERVER;
		}
		gui2::check_response(data_res, data);

		// Backwards-compatibility "version" attribute
		const std::string& version = data["version"];
		if(version.empty() == false && version != game_config::version) {
			utils::string_map i18n_symbols;
			i18n_symbols["version1"] = version;
			i18n_symbols["version2"] = game_config::version;
			const std::string errorstring = vgettext("The server requires version '$version1' while you are using version '$version2'", i18n_symbols);
			throw network::error(errorstring);
		}

		// Check for "redirect" messages
		if (const config &redirect = data.child("redirect"))
		{
			host = redirect["host"].str();
			port =redirect["port"].to_int(15000);

			if(shown_hosts.find(hostpair(host,port)) != shown_hosts.end()) {
				throw network::error(_("Server-side redirect loop"));
			}
			shown_hosts.insert(hostpair(host, port));

			if(network::nconnections() > 0)
				network::disconnect();
			sock = dialogs::network_connect_dialog(disp,_("Connecting to Server..."),host,port);
			continue;
		}

		if(data.child("version")) {
			config cfg;
			config res;
			// cfg["version"] = game_config::version;
			// fake version, in order to login in wesnoth server
			// cfg["version"] = "1.9.10";
			cfg["version"] = "test";
			res.add_child("version", cfg);
			network::send_data(res, 0);
		}

		//if we got a direction to login
		if(data.child("mustlogin")) {

			for(;;) {
				std::string password_reminder = "";

				std::string login = preferences::login();

				config response ;
				config &sp = response.add_child("login") ;
				sp["username"] = login ;

				// Login and enable selective pings -- saves server bandwidth
				// If ping_timeout has a non-zero value, do not enable
				// selective pings as this will cause clients to falsely
				// believe the server has died and disconnect.
				if (preferences::get_ping_timeout()) {
					// Pings required so disable selective pings
					sp["selective_ping"] = false;
				} else {
					// Client is bandwidth friendly so allow
					// server to optimize ping frequency as needed.
					sp["selective_ping"] = true;
				}
				network::send_data(response, 0);

				// Get response for our login request...
				network::connection data_res = network::receive_data(data, 0, 3000);
				if(!data_res) {
					throw network::error(_("Connection timed out"));
				}

				config *error = &data.child("error");

				// ... and get us out of here if the server did not complain
				if (!*error) break;

				do {
					std::string password = preferences::password();

					bool fall_through = (*error)["force_confirmation"].to_bool() ?
						(gui2::show_message(disp.video(), _("Confirm"), (*error)["message"], gui2::tmessage::ok_cancel_buttons) == gui2::twindow::CANCEL) :
						false;

					const bool is_pw_request = !((*error)["password_request"].empty()) && !(password.empty());

					// If the server asks for a password, provide one if we can
					// or request a password reminder.
					// Otherwise or if the user pressed 'cancel' in the confirmation dialog
					// above go directly to the username/password dialog
					if((is_pw_request || !password_reminder.empty()) && !fall_through) {
						if(is_pw_request) {
							if ((*error)["phpbb_encryption"].to_bool())
							{

								// Apparently HTML key-characters are passed to the hashing functions of phpbb in this escaped form.
								// I will do closer investigations on this, for now let's just hope these are all of them.

								// Note: we must obviously replace '&' first, I wasted some time before I figured that out... :)
								for(std::string::size_type pos = 0; (pos = password.find('&', pos)) != std::string::npos; ++pos )
									password.replace(pos, 1, "&amp;");
								for(std::string::size_type pos = 0; (pos = password.find('\"', pos)) != std::string::npos; ++pos )
									password.replace(pos, 1, "&quot;");
								for(std::string::size_type pos = 0; (pos = password.find('<', pos)) != std::string::npos; ++pos )
									password.replace(pos, 1, "&lt;");
								for(std::string::size_type pos = 0; (pos = password.find('>', pos)) != std::string::npos; ++pos )
									password.replace(pos, 1, "&gt;");

								const std::string salt = (*error)["salt"];

								if (salt.length() < 12) {
									throw network::error(_("Bad data received from server"));
								}

								sp["password"] = util::create_hash(util::create_hash(password, util::get_salt(salt),
										util::get_iteration_count(salt)), salt.substr(12, 8));

							} else {
								sp["password"] = password;
							}
						}

						sp["password_reminder"] = password_reminder;

						// Once again send our request...
						network::send_data(response, 0);

						network::connection data_res = network::receive_data(data, 0, 3000);
						if(!data_res) {
							throw network::error(_("Connection timed out"));
						}

						error = &data.child("error");

						// ... and get us out of here if the server is happy now
						if (!*error) break;


					}

					password_reminder = "";

					// Providing a password either was not attempted because we did not
					// have any or failed:
					// Now show a dialog that displays the error and allows to
					// enter a new user name and/or password

					std::string error_message;
					utils::string_map i18n_symbols;
					i18n_symbols["nick"] = login;

					if((*error)["error_code"] == MP_MUST_LOGIN) {
						error_message = _("You must login first.");
					} else if((*error)["error_code"] == MP_NAME_TAKEN_ERROR) {
						error_message = vgettext("The nick '$nick' is already taken.", i18n_symbols);
					} else if((*error)["error_code"] == MP_INVALID_CHARS_IN_NAME_ERROR) {
						error_message = vgettext("The nick '$nick' contains invalid "
								"characters. Only alpha-numeric characters, underscores and "
								"hyphens are allowed.", i18n_symbols);
					} else if((*error)["error_code"] == MP_NAME_TOO_LONG_ERROR) {
						error_message = vgettext("The nick '$nick' is too long. Nicks must "
								"be 20 characters or less.", i18n_symbols);
					} else if((*error)["error_code"] == MP_NAME_RESERVED_ERROR) {
						error_message = vgettext("The nick '$nick' is reserved and cannot be used by players.", i18n_symbols);
					} else if((*error)["error_code"] == MP_NAME_UNREGISTERED_ERROR) {
						error_message = vgettext("The nick '$nick' is not registered on this server.", i18n_symbols)
								+ _(" This server disallows unregistered nicks.");
					} else if((*error)["error_code"] == MP_PASSWORD_REQUEST) {
						error_message = vgettext("The nick '$nick' is registered on this server.", i18n_symbols);
					} else if((*error)["error_code"] == MP_PASSWORD_REQUEST_FOR_LOGGED_IN_NAME) {
						error_message = vgettext("The nick '$nick' is registered on this server.", i18n_symbols)
								+ "\n\n" + _("WARNING: There is already a client using this nick, "
								"logging in will cause that client to be kicked!");
					} else if((*error)["error_code"] == MP_NO_SEED_ERROR) {
						error_message = _("Error in the login procedure (the server had no "
								"seed for your connection).");
					} else if((*error)["error_code"] == MP_INCORRECT_PASSWORD_ERROR) {
						error_message = _("The password you provided was incorrect.");
					} else {
						error_message = (*error)["message"].str();
					}

					gui2::tmp_login dlg(error_message, !((*error)["password_request"].empty()));
					dlg.show(disp.video());

					switch(dlg.get_retval()) {
						//Log in with password
						case gui2::twindow::OK:
							break;
						//Request a password reminder
						case 1:
							password_reminder = "yes";
							break;
						// Cancel
						default: return ABORT_SERVER;
					}

				// If we have got a new username we have to start all over again
				} while(login == preferences::login());

				// Somewhat hacky...
				// If we broke out of the do-while loop above error
				// is still going to be NULL
				if(!*error) break;
			} // end login loop
		}
	} while(!(data.child("join_lobby") || data.child("join_game")));

	if (h != preferences::server_list().front().address)
		preferences::set_network_host(h);

	if (data.child("join_lobby")) {
		return WESNOTHD_SERVER;
	} else {
		return SIMPLE_SERVER;
	}

}


// The multiplayer logic consists in 4 screens:
//
// lobby <-> create <-> connect <-> (game)
//       <------------> wait    <-> (game)
//
// To each of this screen corresponds a dialog, each dialog being defined in
// the multiplayer_(screen) file.
//
// The functions enter_(screen)_mode are simple functions that take care of
// creating the dialogs, then, according to the dialog result, of calling other
// of those screen functions.

static void enter_wait_mode(game_display& disp, const config& game_config, hero_map& heros, hero_map& heros_start, card_map& cards, config& gamelist, bool observe)
{
	mp::ui::result res;
	game_state state;
	network_game_manager m;

	gamelist.clear();
	statistics::fresh_stats();

	gui2::tmp_side_wait dlg(heros, disp, *resources::game_map, game_config, gamelist, observe);
	dlg.show(disp.video());
	switch (dlg.get_legacy_result()) {
	case gui2::tmp_side_wait::PLAY:
		res = mp::ui::PLAY;
		dlg.start_game();
		state = dlg.get_state();
		// lobby may modify hero's side_feature
		heros_start = heros;
		break;
	default:
		res = mp::ui::QUIT;
	}

	switch (res) {
	case mp::ui::PLAY:
		play_game(disp, state, game_config, heros, heros_start, cards, IO_CLIENT,
			preferences::skip_mp_replay() && observe);
		recorder.clear();

		break;
	case mp::ui::QUIT:
	default:
		break;
	}
}

static void enter_create_mode(game_display& disp, const config& game_config, hero_map& heros, hero_map& heros_start, card_map& cards, config& gamelist, mp::controller default_controller, bool local_players_only = false);

static void enter_connect_mode(game_display& disp, const config& game_config, hero_map& heros, hero_map& heros_start,
		card_map& cards,
		config& gamelist, const mp_game_settings& params,
		const int num_turns, mp::controller default_controller, bool local_players_only = false)
{
	mp::ui::result res;
	game_state state;
	const network::manager net_manager(1,1);
	network_game_manager m;

	gamelist.clear();
	statistics::fresh_stats();

	// if (gui2::new_widgets) {
		gui2::tmp_side_creator dlg(heros, disp, *resources::game_map, game_config, gamelist, params, num_turns, default_controller, local_players_only);
		dlg.show(disp.video());
		switch (dlg.get_legacy_result()) {
		case gui2::tmp_side_creator::PLAY:
			res = mp::ui::PLAY;
			dlg.start_game();
			state = dlg.get_state();
			// lobby may modify hero's side_feature
			heros_start = heros;
			break;
		case gui2::tmp_side_creator::CREATE:
			res = mp::ui::CREATE;
			break;
		default:
			res = mp::ui::QUIT;
		}

	switch (res) {
	case mp::ui::PLAY:
		play_game(disp, state, game_config, heros, heros_start, cards, IO_SERVER);
		recorder.clear();

		break;
	case mp::ui::CREATE:
		enter_create_mode(disp, game_config, heros, heros_start, cards, gamelist, default_controller, local_players_only);
		break;
	case mp::ui::QUIT:
	default:
		network::send_data(config("refresh_lobby"), 0);
		break;
	}
}

static void enter_create_mode(game_display& disp, const config& game_config, hero_map& heros, hero_map& heros_start, 
		card_map& cards, config& gamelist, mp::controller default_controller, bool local_players_only)
{
	mp::ui::result res;
	mp_game_settings params;
	int num_turns;

	// if (gui2::new_widgets) {
		gui2::tmp_create_game dlg(disp, game_config);
		dlg.show(disp.video());
		int retval = dlg.get_retval();
		if (retval == gui2::twindow::OK) {
			res = mp::ui::CREATE;
		} else {
			res = mp::ui::QUIT;
		}
		params = dlg.get_parameters();
		num_turns = dlg.num_turns();

		// network::send_data(config("refresh_lobby"), 0);

	switch (res) {
	case mp::ui::CREATE:
		enter_connect_mode(disp, game_config, heros, heros_start, cards, gamelist, params, num_turns, default_controller, local_players_only);
		break;
	case mp::ui::QUIT:
	default:
		//update lobby content
		network::send_data(config("refresh_lobby"), 0);
		break;
	}
}

static void do_preferences_dialog(game_display& disp, const config& game_config)
{
	const preferences::display_manager disp_manager(&disp);
	gui2::show_preferences_dialog(disp);
	return;

	preferences::show_preferences_dialog(disp,game_config);

	/**
	 * The screen size might have changed force an update of the size.
	 *
	 * @todo This might no longer be needed when gui2 is done.
	 */
	const SDL_Rect rect = screen_area();
	preferences::set_resolution(disp.video(), rect.w, rect.h);

	gui2::settings::gamemap_width += rect.w - gui2::settings::screen_width ;
	gui2::settings::gamemap_height += rect.h - gui2::settings::screen_height ;
	gui2::settings::screen_width = rect.w;
	gui2::settings::screen_height = rect.h;
}

static void enter_lobby_mode(game_display& disp, const config& game_config, hero_map& heros, hero_map& heros_start, 
		card_map& cards, config& gamelist)
{


	mp::ui::result res;

	while (true) {
		const config &cfg = game_config.child("lobby_music");
		if (cfg) {
			foreach (const config &i, cfg.child_range("music")) {
				sound::play_music_config(i);
			}
			sound::commit_music_changes();
		} else {
			sound::empty_playlist();
			sound::stop_music();
		}
		lobby_info li(game_config);

		// Force a black background
		const Uint32 color = SDL_MapRGBA(disp.video().getSurface()->format
				, 0
				, 0
				, 0
				, 255);

		sdl_fill_rect(disp.video().getSurface(), NULL, color);

		{
			gui2::tlobby_main dlg(game_config, li, disp);
			dlg.set_preferences_callback(
				boost::bind(do_preferences_dialog,
					boost::ref(disp), boost::ref(game_config)));
			dlg.show(disp.video());
			//ugly kludge for launching other dialogs like the old lobby
			switch (dlg.get_legacy_result()) {
				case gui2::tlobby_main::CREATE:
					res = mp::ui::CREATE;
					break;
				case gui2::tlobby_main::JOIN:
					res = mp::ui::JOIN;
					break;
				case gui2::tlobby_main::OBSERVE:
					res = mp::ui::OBSERVE;
					break;
				default:
					res = mp::ui::QUIT;
			}
		}

		switch (res) {
		case mp::ui::JOIN:
			try {
				enter_wait_mode(disp, game_config, heros, heros_start, cards, gamelist, false);
			} catch(config::error& error) {
				if(!error.message.empty()) {
					gui2::show_error_message(disp.video(), error.message);
				}
				//update lobby content
				network::send_data(config("refresh_lobby"), 0);
			}
			break;
		case mp::ui::OBSERVE:
			try {
				enter_wait_mode(disp, game_config, heros, heros_start, cards, gamelist, true);
			} catch(config::error& error) {
				if(!error.message.empty()) {
					gui2::show_error_message(disp.video(), error.message);
				}
			}
			// update lobby content unconditionally because we might have left only after the
			// game ended in which case we ignored the gamelist and need to request it again
			network::send_data(config("refresh_lobby"), 0);
			break;
		case mp::ui::CREATE:
			try {
				enter_create_mode(disp, game_config, heros, heros_start, cards, gamelist, mp::CNTR_NETWORK);
			} catch(config::error& error) {
				if (!error.message.empty())
					gui2::show_error_message(disp.video(), error.message);
				//update lobby content
				network::send_data(config("refresh_lobby"), 0);
			}
			break;
		case mp::ui::QUIT:
			return;
		case mp::ui::PREFERENCES:
			{
				do_preferences_dialog(disp, game_config);
				//update lobby content
				network::send_data(config("refresh_lobby"), 0);
			}
			break;
		default:
			return;
		}
	}
}

namespace mp {

void start_local_game(game_display& disp, const config& game_config, hero_map& heros, hero_map& heros_start,
		card_map& cards, mp::controller default_controller)
{
	const rand_rng::set_random_generator generator_setter(&recorder);
	config gamelist;
	playmp_controller::set_replay_last_turn(0);
	preferences::set_message_private(false);
	enter_create_mode(disp, game_config, heros, heros_start, cards, gamelist, default_controller, true);
}

void start_client(game_display& disp, const config& game_config, hero_map& heros, hero_map& heros_start,
		card_map& cards, const std::string& host)
{
	const rand_rng::set_random_generator generator_setter(&recorder);
	const network::manager net_manager(1,1);

	config gamelist;
	server_type type = open_connection(disp, host);

	switch(type) {
	case WESNOTHD_SERVER:
		enter_lobby_mode(disp, game_config, heros, heros_start, cards, gamelist);
		break;
	case SIMPLE_SERVER:
		playmp_controller::set_replay_last_turn(0);
		preferences::set_message_private(false);
		enter_wait_mode(disp, game_config, heros, heros_start, cards, gamelist, false);
		break;
	case ABORT_SERVER:
		break;
	}
}

}

