/*
	Copyright (C) 2014 - 2025
	by Chris Beck <render787@gmail.com>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

/**
 * This namespace provides handlers which play the sounds / notificaitons
 * for various mp server events, depending on the preference configuration.
 */

#pragma once

#include<string>
#include<vector>

namespace mp::ui_alerts {

	// Functions called when such an event occurs
	void player_joins(bool is_lobby);
	void player_leaves(bool is_lobby);
	void game_created(const std::string & scenario, const std::string & name);
	void public_message(bool is_lobby, const std::string & sender, const std::string & message);
	void friend_message(bool is_lobby, const std::string & sender, const std::string & message);
	void private_message(bool is_lobby, const std::string & sender, const std::string & message);
	void server_message(bool is_lobby, const std::string & sender, const std::string & message);
	void ready_for_start();
	void game_has_begun();

	void turn_changed(const std::string & player);
}
