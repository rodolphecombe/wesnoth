/* $Id$ */
/* $Id$ */
/*
   Copyright (C) 2003 by David White <davidnwhite@verizon.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "global.hpp"
#include "actions.hpp"
#include "display.hpp"
#include "game_errors.hpp"
#include "game_events.hpp"
#include "image.hpp"
#include "language.hpp"
#include "log.hpp"
#include "menu_events.hpp"
#include "preferences.hpp"
#include "replay.hpp"
#include "show_dialog.hpp"
#include "sound.hpp"
#include "unit_display.hpp"
#include "util.hpp"
#include "wassert.hpp"
#include "gettext.hpp"
#include "variable.hpp"
#include "serialization/string_utils.hpp"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <iterator>
#include <set>
#include <string>

#define LOG_NG LOG_STREAM(info, engine)
#define WRN_NG LOG_STREAM(warn, engine)
#define ERR_NG LOG_STREAM(err, engine)
#define LOG_DP LOG_STREAM(info, display)
#define ERR_CF LOG_STREAM(err, config)

namespace {

display* screen = NULL;
gamemap* game_map = NULL;
units_map* units = NULL;
std::vector<team>* teams = NULL;
game_state* state_of_game = NULL;
const game_data* game_data_ptr = NULL;
gamestatus* status_ptr = NULL;
int floating_label = 0;
} //end anonymous namespace

namespace game_events {

bool conditional_passed(const units_map* units,
                        const vconfig cond)
{
	//an 'and' statement means that if the contained statements are false,
	//then it automatically fails
	const vconfig::child_list& and_statements = cond.get_children("and");
	for(vconfig::child_list::const_iterator and_it = and_statements.begin();
			and_it != and_statements.end(); ++and_it) {
		if(!conditional_passed(units,*and_it)) {
			return false;
		}
	}

	//an 'or' statement means that if the contained statements are true,
	//then it automatically passes
	const vconfig::child_list& or_statements = cond.get_children("or");
	for(vconfig::child_list::const_iterator or_it = or_statements.begin();
			or_it != or_statements.end(); ++or_it) {
		if(conditional_passed(units,*or_it)) {
			return true;
		}
	}

	//if the if statement requires we have a certain unit, then
	//check for that.
	const vconfig::child_list& have_unit = cond.get_children("have_unit");

	for(vconfig::child_list::const_iterator u = have_unit.begin(); u != have_unit.end(); ++u) {

		if(units == NULL)
			return false;

		units_map::const_iterator itor;
		for(itor = units->begin(); itor != units->end(); ++itor) {
			if(itor->second.hitpoints() > 0 && game_events::unit_matches_filter(itor, *u)) {
				break;
			}
		}

		if(itor == units->end()) {
			return false;
		}
	}

	//check against each variable statement to see if the variable
	//matches the conditions or not
	const vconfig::child_list& variables = cond.get_children("variable");
	for(vconfig::child_list::const_iterator var = variables.begin(); var != variables.end(); ++var) {
		const vconfig& values = *var;

		const std::string& name = values["name"];
		wassert(state_of_game != NULL);
		const std::string& value = state_of_game->get_variable(name);

		const double num_value = atof(value.c_str());

		const std::string& equals = values["equals"];
		if(values.get_attribute("equals") != "" && value != equals) {
			return false;
		}

		const std::string& numerical_equals = values["numerical_equals"];
		if(values.get_attribute("numerical_equals") != "" && atof(numerical_equals.c_str()) != num_value){
			return false;
		}

		const std::string& not_equals = values["not_equals"];
		if(values.get_attribute("not_equals") != "" && not_equals == value) {
			return false;
		}

		const std::string& numerical_not_equals = values["numerical_not_equals"];
		if(values.get_attribute("numerical_not_equals") != "" && atof(numerical_not_equals.c_str()) == num_value){
			return false;
		}

		const std::string& greater_than = values["greater_than"];
		if(values.get_attribute("greater_than") != "" && atof(greater_than.c_str()) >= num_value){
			return false;
		}

		const std::string& less_than = values["less_than"];
		if(values.get_attribute("less_than") != "" && atof(less_than.c_str()) <= num_value){
			return false;
		}

		const std::string& greater_than_equal_to = values["greater_than_equal_to"];
		if(values.get_attribute("greater_than_equal_to") != "" && atof(greater_than_equal_to.c_str()) > num_value){
			return false;
		}

		const std::string& less_than_equal_to = values["less_than_equal_to"];
		if(values.get_attribute("less_than_equal_to") != "" && atof(less_than_equal_to.c_str()) < num_value) {
			return false;
		}
	}

	const vconfig::child_list& not_statements = cond.get_children("not");
	for(vconfig::child_list::const_iterator not_it = not_statements.begin();
			not_it != not_statements.end(); ++not_it) {
		if(conditional_passed(units,*not_it)) {
			return false;
		}
	}

	return !have_unit.empty() || !variables.empty() || !not_statements.empty() || !and_statements.empty();
}

} //end namespace game_events

namespace {

std::set<std::string> used_items;

const size_t MaxLoop = 1024;

bool events_init() { return screen != NULL; }

struct queued_event {
	queued_event(const std::string& name, const gamemap::location& loc1,
	                                      const gamemap::location& loc2,
										  const config& data)
			: name(name), loc1(loc1), loc2(loc2),data(data) {}

	std::string name;
	gamemap::location loc1;
	gamemap::location loc2;
	config data;
};

std::deque<queued_event> events_queue;

class event_handler
{
public:
	event_handler(const config& cfg) :
		name_(cfg["name"]),
		first_time_only_(utils::string_bool(cfg["first_time_only"],true)),
		disabled_(false),rebuild_screen_(false),
		cfg_(&cfg)
	{}

	void write(config& cfg) const
	{
		if(disabled_)
			return;

		cfg = cfg_.get_config();
	}

	const std::string& name() const { return name_; }

	bool first_time_only() const { return first_time_only_; }

	void disable() { disabled_ = true; }
	bool disabled() const { return disabled_; }

	const vconfig::child_list first_arg_filters()
	{
		return cfg_.get_children("filter");
	}
	const vconfig::child_list first_special_filters()
	{
		return cfg_.get_children("special_filter");
	}

	const vconfig::child_list second_arg_filters()
	{
		return cfg_.get_children("filter_second");
	}
	const vconfig::child_list second_special_filters()
	{
		return cfg_.get_children("special_filter_second");
	}

	bool handle_event(const queued_event& event_info,
			const vconfig cfg = vconfig());
	
	bool& rebuild_screen() {return rebuild_screen_;}
	
private:
	bool handle_event_command(const queued_event& event_info, const std::string& cmd, const vconfig cfg, bool& mutated);

	std::string name_;
	bool first_time_only_;
	bool disabled_;
	bool rebuild_screen_;
	vconfig cfg_;
};

gamemap::location cfg_to_loc(const vconfig cfg,int defaultx = 0, int defaulty = 0)
{
	int x = lexical_cast_default(cfg["x"], defaultx) - 1;
	int y = lexical_cast_default(cfg["y"], defaulty) - 1;

	return gamemap::location(x, y);
}

std::vector<gamemap::location> multiple_locs(const vconfig cfg)
{
	return parse_location_range(cfg["x"],cfg["y"]);
}

std::multimap<std::string,event_handler> events_map;

//this function handles all the different types of actions that can be triggered
//by an event.
bool event_handler::handle_event_command(const queued_event& event_info,
		const std::string& cmd, const vconfig cfg, bool& mutated)
{
	log_scope2(engine, "handle_event_command");
	LOG_NG << "handling command: '" << cmd << "'\n";
	
	bool rval = true;
	//sub commands that need to be handled in a guaranteed ordering
	if(cmd == "command") {
		handle_event(event_info, cfg);
	}

	//allow undo sets the flag saying whether the event has mutated the game to false
	else if(cmd == "allow_undo") {
		mutated = false;
	}
	//change shroud settings for portions of the map
	else if(cmd == "remove_shroud" || cmd == "place_shroud") {
		const bool remove = cmd == "remove_shroud";

		std::string side = cfg["side"];
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		const int side_num = lexical_cast_default<int>(side,1);
		const size_t index = side_num-1;

		if(index < teams->size()) {
			const std::vector<gamemap::location>& locs = multiple_locs(cfg);
			for(std::vector<gamemap::location>::const_iterator j = locs.begin(); j != locs.end(); ++j) {
				if(remove) {
					(*teams)[index].clear_shroud(j->x,j->y);
				} else {
					(*teams)[index].place_shroud(j->x,j->y);
				}
			}
		}

		screen->invalidate_all();
	}


	//teleport a unit from one location to another
	else if(cmd == "teleport") {

		unit_map::iterator u = units->find(event_info.loc1);

		//search for a valid unit filter, and if we have one, look for the matching unit
		const vconfig filter = cfg.child("filter");
		if(!filter.null()) {
			for(u = units->begin(); u != units->end(); ++u){
				if(game_events::unit_matches_filter(u, filter))
					break;
			}
		}

		//we have found a unit that matches the filter
		if(u != units->end()) {
			const gamemap::location dst = cfg_to_loc(cfg);
			if(game_map->on_board(dst)) {
				const gamemap::location vacant_dst = find_vacant_tile(*game_map,*units,dst);
				if(game_map->on_board(vacant_dst)) {
					const int side = u->second.side();

					//note that inserting into a map does not invalidate iterators
					//into the map, so this sequence is fine.
					units->insert(std::pair<gamemap::location,unit>(vacant_dst,u->second));
					units->erase(u);

					if(game_map->is_village(vacant_dst)) {
						get_village(vacant_dst,*teams,side,*units);
					}

					if(utils::string_bool(cfg["clear_shroud"],true)) {
						clear_shroud(*screen,*status_ptr,*game_map,*game_data_ptr,*units,*teams,side-1);
					}
				}
			}
		}
	}

	//remove units from being turned to stone
	else if(cmd == "unstone") {
		const vconfig filter = cfg.child("filter");
		for(unit_map::iterator i = units->begin(); i != units->end(); ++i) {
			if(utils::string_bool(i->second.get_state("stoned"))) {
				if(!filter.null()) {
					if(game_events::unit_matches_filter(i, filter))
						i->second.set_state("stoned","");
				} else {
					i->second.set_state("stoned","");
				}
			}
		}
	}

	//allow a side to recruit a new type of unit
	else if(cmd == "allow_recruit") {
		std::string side = cfg["side"];
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		const int side_num = lexical_cast_default<int>(side,1);
		const size_t index = side_num-1;

		if(index >= teams->size())
			return rval;

		const std::string& type = cfg["type"];

		const std::vector<std::string>& types = utils::split(type);
		for(std::vector<std::string>::const_iterator i = types.begin(); i != types.end(); ++i) {
			(*teams)[index].recruits().insert(*i);
			preferences::encountered_units().insert(*i);

                        player_info *player=state_of_game->get_player((*teams)[index].save_id());
                        if(player) {
                                player->can_recruit.insert(*i);
			}
		}
	}

	//remove the ability to recruit a unit from a certain side
	else if(cmd == "disallow_recruit") {
		std::string side = cfg["side"];
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		const int side_num = lexical_cast_default<int>(side,1);
		const size_t index = side_num-1;

		if(index >= teams->size())
			return rval;

		const std::string& type = cfg["type"];
		const std::vector<std::string>& types = utils::split(type);
		for(std::vector<std::string>::const_iterator i = types.begin(); i != types.end(); ++i) {
			(*teams)[index].recruits().erase(*i);

                        player_info *player=state_of_game->get_player((*teams)[index].save_id());
                        if(player) {
                                player->can_recruit.erase(*i);
			}
		}
	}

	else if(cmd == "set_recruit") {
		std::string side = cfg["side"];
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		const int side_num = lexical_cast_default<int>(side,1);
		const size_t index = side_num-1;

		if(index >= teams->size())
			return rval;

		std::vector<std::string> recruit = utils::split(cfg["recruit"]);
		if(recruit.size() == 1 && recruit.back() == "")
			recruit.clear();

		std::set<std::string>& can_recruit = (*teams)[index].recruits();
		can_recruit.clear();
		std::copy(recruit.begin(),recruit.end(),std::inserter(can_recruit,can_recruit.end()));

                player_info *player=state_of_game->get_player((*teams)[index].save_id());
                if(player) {
                        player->can_recruit = can_recruit;
		}
	}

	else if(cmd == "music") {
		sound::play_music_config(cfg.get_parsed_config());
	}

	else if(cmd == "sound") {
		sound::play_sound(cfg["name"]);
	}

	else if(cmd == "colour_adjust") {
		std::string red = cfg["red"];
		std::string green = cfg["green"];
		std::string blue = cfg["blue"];
		wassert(state_of_game != NULL);
		red = utils::interpolate_variables_into_string(red, *state_of_game);
		green = utils::interpolate_variables_into_string(green, *state_of_game);
		blue = utils::interpolate_variables_into_string(blue, *state_of_game);
		const int r = atoi(red.c_str());
		const int g = atoi(green.c_str());
		const int b = atoi(blue.c_str());
		screen->adjust_colours(r,g,b);
		screen->invalidate_all();
		screen->draw(true,true);
	}

	else if(cmd == "delay") {
		std::string delay_string = cfg["time"];
		wassert(state_of_game != NULL);
		delay_string = utils::interpolate_variables_into_string(delay_string, *state_of_game);
		const int delay_time = atoi(delay_string.c_str());
		::SDL_Delay(delay_time);
	}

	else if(cmd == "scroll") {
		std::string x = cfg["x"];
		std::string y = cfg["y"];
		wassert(state_of_game != NULL);
		x = utils::interpolate_variables_into_string(x, *state_of_game);
		y = utils::interpolate_variables_into_string(y, *state_of_game);
		const int xoff = atoi(x.c_str());
		const int yoff = atoi(y.c_str());
		screen->scroll(xoff,yoff);
		screen->draw(true,true);
	}

	else if(cmd == "scroll_to") {
		std::string x = cfg["x"];
		std::string y = cfg["y"];
		wassert(state_of_game != NULL);
		x = utils::interpolate_variables_into_string(x, *state_of_game);
		y = utils::interpolate_variables_into_string(y, *state_of_game);
		const int xpos = atoi(x.c_str());
		const int ypos = atoi(y.c_str());
		screen->scroll_to_tile(xpos,ypos);
	}

	else if(cmd == "scroll_to_unit") {
		unit_map::const_iterator u;
		for(u = units->begin(); u != units->end(); ++u){
			if(game_events::unit_matches_filter(u,cfg))
				break;
		}

		if(u != units->end()) {
			screen->scroll_to_tile(u->first.x,u->first.y);
		}
	}

	//an award of gold to a particular side
	else if(cmd == "gold") {
		std::string side = cfg["side"];
		std::string amount = cfg["amount"];
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		amount = utils::interpolate_variables_into_string(amount, *state_of_game);
		const int side_num = lexical_cast_default<int>(side,1);
		const int amount_num = atoi(amount.c_str());
		const size_t team_index = side_num-1;
		if(team_index < teams->size()) {
			(*teams)[team_index].spend_gold(-amount_num);
		}
	}

	//modifications of some attributes of a side: gold, income, team name
	else if(cmd == "modify_side") {
		std::cerr << "modifying side...\n";
		std::string side = cfg["side"];
		std::string income = cfg["income"];
		std::string team_name = cfg["team_name"];
		std::string gold = cfg["gold"];
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		income = utils::interpolate_variables_into_string(income, *state_of_game);
		team_name = utils::interpolate_variables_into_string(team_name, *state_of_game);
		gold = utils::interpolate_variables_into_string(gold, *state_of_game);
		const int side_num = lexical_cast_default<int>(side,1);
		const size_t team_index = side_num-1;

		if(team_index < teams->size()) {
			std::cerr << "modifying team: " << team_index << "\n";
			if(!team_name.empty()) {
				std::cerr << "change team to team_name '" << team_name << "'\n";
				(*teams)[team_index].change_team(team_name);
			}

			if(!income.empty()) {
				(*teams)[team_index].set_income(lexical_cast_default<int>(income));
			}

			if(!gold.empty()) {
				(*teams)[team_index].spend_gold((*teams)[team_index].gold()-lexical_cast_default<int>(gold));
			}
		}
	}
	//stores of some attributes of a side: gold, income, team name
	else if(cmd == "store_side") {
		std::string side = cfg["side"];
		std::string var_name = cfg["variable"];
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		var_name = utils::interpolate_variables_into_string(var_name, *state_of_game);
		const int side_num = lexical_cast_default<int>(side,1);
		const size_t team_index = side_num-1;
		if(team_index < teams->size()) {
			state_of_game->get_variable(var_name+".gold") = lexical_cast_default<std::string>((*teams)[team_index].gold(),"");
			state_of_game->get_variable(var_name+".income") = lexical_cast_default<std::string>((*teams)[team_index].income(),"");
			state_of_game->get_variable(var_name+".name") = (*teams)[team_index].name();
			state_of_game->get_variable(var_name+".team_name") = (*teams)[team_index].team_name();
		}
	}
	else if(cmd == "modify_turns") {
		std::string value = cfg["value"];
		std::string add = cfg["add"];
		wassert(state_of_game != NULL);
		value = utils::interpolate_variables_into_string(value, *state_of_game);
		add = utils::interpolate_variables_into_string(add, *state_of_game);
		
		wassert(status_ptr != NULL);
		if(add != "") {
			status_ptr->modify_turns(add);
		} else {
			status_ptr->add_turns(lexical_cast_default<int>(value,0));
		}
		
	}
	//command to store gold into a variable
	else if(cmd == "store_gold") {
		WRN_NG << "[store_gold] tag is now deprecated; use [store_side] instead.\n";
		std::string side = cfg["side"];
		std::string var_name = cfg["variable"];
		if(var_name.empty()) {
			var_name = "gold";
		}
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		var_name = utils::interpolate_variables_into_string(var_name, *state_of_game);
		const int side_num = lexical_cast_default<int>(side,1);
		const size_t team_index = side_num-1;
		if(team_index < teams->size()) {
			char value[50];
			snprintf(value,sizeof(value),"%d",(*teams)[team_index].gold());
			wassert(state_of_game != NULL);
			state_of_game->set_variable(var_name,value);
		}
	}

	//moving a 'unit' - i.e. a dummy unit that is just moving for
	//the visual effect
	else if(cmd == "move_unit_fake") {
		std::string type = cfg["type"];
		std::string side = cfg["side"];
		std::string gender_string = cfg["gender"];
		std::string x = cfg["x"];
		std::string y = cfg["y"];
		wassert(state_of_game != NULL);
		type = utils::interpolate_variables_into_string(type, *state_of_game);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		gender_string = utils::interpolate_variables_into_string(gender_string, *state_of_game);
		x = utils::interpolate_variables_into_string(x, *state_of_game);
		y = utils::interpolate_variables_into_string(y, *state_of_game);
		
		size_t side_num = lexical_cast_default<int>(side,1)-1;
		if (side_num >= teams->size()) side_num = 0;

		const unit_race::GENDER gender = gender_string == "female" ? unit_race::FEMALE : unit_race::MALE;
		const game_data::unit_type_map::const_iterator itor = game_data_ptr->unit_types.find(type);
		if(itor != game_data_ptr->unit_types.end()) {
			wassert(game_data_ptr != NULL);
			wassert(units != NULL);
			wassert(game_map != NULL);
			wassert(status_ptr != NULL);
			unit dummy_unit(game_data_ptr,units,game_map,status_ptr,teams,&itor->second,side_num+1,false,true,gender);
			const std::vector<std::string> xvals = utils::split(x);
			const std::vector<std::string> yvals = utils::split(y);
			std::vector<gamemap::location> path;
			gamemap::location src;
			gamemap::location dst;
			for(size_t i = 0; i != minimum(xvals.size(),yvals.size()); ++i) {
				if(i==0){
					src.x = atoi(xvals[i].c_str())-1;
					src.y = atoi(yvals[i].c_str())-1;
					if (!game_map->on_board(src)) {
						ERR_CF << "invalid move_unit_fake source: " << src << '\n';
						break;
					}
					continue;
				}
				shortest_path_calculator calc(dummy_unit,
						(*teams)[side_num],
						*units,
						*teams,
						*game_map);

				dst.x = atoi(xvals[i].c_str())-1;
				dst.y = atoi(yvals[i].c_str())-1;
				if (!game_map->on_board(dst)) {
					ERR_CF << "invalid move_unit_fake destination: " << dst << '\n';
					break;
				}

				paths::route route = a_star_search(src, dst, 10000, &calc,
				                                   game_map->x(), game_map->y());
				unit_display::move_unit(*screen, *game_map, route.steps, dummy_unit,
				                        *units, *teams);

				src = dst;
			}
		}
	}

	//provide a means of specifying win/loss conditions:
	// [event]
	// name=prestart
	// [objectives]
	//   side=1
	//   summary="Escape the forest alive"
	//   victory_string="Victory:"
	//   defeat_string="Defeat:"
	//   [objective]
	//     condition=win
	//     description="Defeat all enemies"
	//   [/objective]
	//   [objective]
	//     description="Death of Konrad"
	//     condition=lose
	//   [/objective]
	// [/objectives]
	// [/event]
	//instead of the current (but still supported):
	// objectives= _ "
	// Victory:
	// @Move Konrad to the signpost in the north-west
	// Defeat:
	// #Death of Konrad
	// #Death of Delfador
	// #Turns run out"
	//
	// If side is set to 0, the new objectives are added to each player.
	//
	// The new objectives will be automatically displayed, but only to the
	// player whose objectives did change, and only when it's this player's
	// turn.
	else if(cmd == "objectives") {
		const std::string win_str = "@";
		const std::string lose_str = "#";

		const t_string& summary = cfg["summary"];
		const t_string& note = cfg["note"];
		std::string side = cfg["side"];
		bool silent = utils::string_bool(cfg["silent"]);
		wassert(state_of_game != NULL);
		side = utils::interpolate_variables_into_string(side, *state_of_game);
		const size_t side_num = lexical_cast_default<size_t>(side,0);

		if(side_num != 0 && (side_num - 1) >= teams->size()) {
			ERR_NG << "Invalid side: " << cfg["side"] << " in objectives event\n";
			return rval;
		}

		t_string win_string = cfg["victory_string"];
		if(win_string.empty())
			win_string = t_string(N_("Victory:"), "wesnoth");
		t_string lose_string = cfg["defeat_string"];
		if(lose_string.empty())
			lose_string = t_string(N_("Defeat:"), "wesnoth");

		t_string win_objectives;
		t_string lose_objectives;

		const vconfig::child_list objectives = cfg.get_children("objective");
		for(vconfig::child_list::const_iterator obj_it = objectives.begin();
				obj_it != objectives.end(); ++obj_it) {

			const t_string description = (*obj_it)["description"];
			const std::string& condition = (*obj_it)["condition"];
			LOG_NG << condition << " objective: " << description << "\n";
			if(condition == "win") {
				win_objectives += "\n";
				win_objectives += win_str;
				win_objectives += description;
			} else if(condition == "lose") {
				lose_objectives += "\n";
				lose_objectives += lose_str;
				lose_objectives += description;
			} else {
				ERR_NG << "unknown condition '" << condition << "', ignoring\n";
			}
		}

		t_string objs;
		if(!summary.empty())
			objs += "*" + summary + "\n";
		if(!win_objectives.empty()) {
			objs += "*" + win_string + "\n";
			objs += win_objectives + "\n";
		}
		if(!lose_objectives.empty()) {
			objs += "*" + lose_string + "\n";
			objs += lose_objectives + "\n";
		}
		if(!note.empty())
			objs += note + "\n";

		if(side_num == 0) {
			for(std::vector<team>::iterator itor = teams->begin();
					itor != teams->end(); ++itor) {

				itor->set_objectives(objs, silent);
			}
		} else {
			(*teams)[side_num - 1].set_objectives(objs, silent);
		}
	}


	//setting a variable
	else if(cmd == "set_variable") {
		wassert(state_of_game != NULL);

		const std::string& name = utils::interpolate_variables_into_string(
			cfg.get_attribute("name"), *state_of_game);
		t_string& var = state_of_game->get_variable(name);

		const t_string& value = cfg["value"];
		if(value.empty() == false) {
			var = value;
		}

		const std::string& format = utils::interpolate_variables_into_string(
			cfg.get_attribute("format"), *state_of_game);
		if(format.empty() == false) {
			var = format;
		}

		const std::string& to_variable = utils::interpolate_variables_into_string(
			cfg.get_attribute("to_variable"), *state_of_game);
		if(to_variable.empty() == false) {
			var = state_of_game->get_variable(to_variable);
		}

		const std::string& add = cfg["add"];
		if(add.empty() == false) {
			int value = int(atof(var.c_str()));
			value += atoi(add.c_str());
			char buf[50];
			snprintf(buf,sizeof(buf),"%d",value);
			var = buf;
		}

		const std::string& multiply = cfg["multiply"];
		if(multiply.empty() == false) {
			int value = int(atof(var.c_str()));
			value = int(double(value) * atof(multiply.c_str()));
			char buf[50];
			snprintf(buf,sizeof(buf),"%d",value);
			var = buf;
		}

		// random generation works as follows:
		// random=[comma delimited list]
		// Each element in the list will be considered a separate choice,
		// unless it contains "..". In this case, it must be a numerical
		// range. (i.e. -1..-10, 0..100, -10..10, etc)
		const std::string& random = cfg["random"];
		if(random.empty() == false) {
			std::string random_value, word;
			std::vector<std::string> words;
			std::vector<std::pair<size_t,size_t> > ranges;
			int num_choices = 0;
			std::string::size_type pos = 0, pos2 = std::string::npos, tmp;
			std::stringstream ss(std::stringstream::in|std::stringstream::out);
			while (pos2 != random.length()) {
				pos = pos2+1;
				pos2 = random.find(",", pos);

				if (pos2 == std::string::npos)
					pos2 = random.length();

				word = random.substr(pos, pos2-pos);
				words.push_back(word);
				tmp = word.find("..");


				if (tmp == std::string::npos) {
					// treat this element as a string
					ranges.push_back(std::pair<int, int>(0,0));
					num_choices += 1;
				}
				else {
					// treat as a numerical range
					const std::string first = word.substr(0, tmp);
					const std::string second = word.substr(tmp+2,
							random.length());

					size_t low, high;
					ss << first + " " + second;
					ss >> low;
					ss >> high;
					ss.clear();

					if (low > high) {
						tmp = low;
						low = high;
						high = tmp;
					}
					ranges.push_back(std::pair<int, int>(low,high));
					num_choices += (high - low) + 1;
				}
			}

			size_t choice = get_random() % num_choices;
			tmp = 0;
			for(size_t i = 0; i < ranges.size(); i++) {
				tmp += (ranges[i].second - ranges[i].first) + 1;
				if (tmp > choice) {
					if (ranges[i].first == 0 && ranges[i].second == 0) {
						random_value = words[i];
					}
					else {
						tmp = (ranges[i].second - (tmp - choice)) + 1;
						ss << tmp;
						ss >> random_value;
					}
					break;
				}
			}

			var = random_value;
		}
	}

	//conditional statements
	else if(cmd == "if" || cmd == "while") {
		log_scope(cmd);
		const size_t max_iterations = (cmd == "if" ? 1 : MaxLoop);
		const std::string pass = (cmd == "if" ? "then" : "do");
		const std::string fail = (cmd == "if" ? "else" : "");
		for(size_t i = 0; i != max_iterations; ++i) {
			const std::string type = game_events::conditional_passed(
			                              units,cfg) ? pass : fail;

			if(type == "") {
				break;
			}

			//if the if statement passed, then execute all 'then' statements,
			//otherwise execute 'else' statements
			const vconfig::child_list commands = cfg.get_children(type);
			for(vconfig::child_list::const_iterator cmd = commands.begin();
			    cmd != commands.end(); ++cmd) {
				handle_event(event_info, *cmd);
			}
		}
	}

	else if(cmd == "role") {

		//get a list of the types this unit can be
		std::vector<std::string> types = utils::split(cfg["type"]);
		if (types.size() == 0) types.push_back("");

                std::vector<std::string> sides = utils::split(cfg["side"]);

		//iterate over all the types, and for each type, try to find
		//a unit that matches
		std::vector<std::string>::iterator ti;
		for(ti = types.begin(); ti != types.end(); ++ti) {
			config item = cfg.get_config();
			item["type"] = *ti;
			item["role"] = "";
			vconfig filter(&item);

			units_map::iterator itor;
			for(itor = units->begin(); itor != units->end(); ++itor) {
				if(game_events::unit_matches_filter(itor, filter)) {
					itor->second.assign_role(cfg["role"]);
					break;
				}
			}

			if(itor != units->end())
				break;

                        bool found = false;

			if(sides.empty() == false) {
				std::vector<std::string>::const_iterator si;
				for(si = sides.begin(); si != sides.end(); ++si) {
					int side_num = lexical_cast_default<int>(*si,1);

					player_info* player=state_of_game->get_player((*teams)[side_num-1].save_id());

					if(!player)
						continue;

					//iterate over the units, and try to find one that matches
					std::vector<unit>::iterator ui;
					for(ui = player->available_units.begin();
							ui != player->available_units.end(); ++ui) {
						wassert(game_data_ptr != NULL);
						ui->set_game_context(game_data_ptr,units,game_map,status_ptr,teams);
						if(game_events::unit_matches_filter(*ui, filter,gamemap::location())) {
							ui->assign_role(cfg["role"]);
							found=true;
							break;
						}
					}
				}
			} else {
				std::map<std::string, player_info>::iterator pi;
				for(pi=state_of_game->players.begin();
						pi!=state_of_game->players.end(); ++pi) {
					std::vector<unit>::iterator ui;
					//iterate over the units, and try to find one that matches
					for(ui = pi->second.available_units.begin();
							ui != pi->second.available_units.end(); ++ui) {
						wassert(game_data_ptr != NULL);
						ui->set_game_context(game_data_ptr,units,game_map,status_ptr,teams);
						if(game_events::unit_matches_filter(*ui, filter,gamemap::location())) {
							ui->assign_role(cfg["role"]);
							found=true;
							break;
						}
					}
				}
			}

			//if we found a unit, we don't have to keep going.
                        if(found)
				break;
		}
	}

	else if(cmd == "removeitem") {
		gamemap::location loc = cfg_to_loc(cfg);
		if(!loc.valid()) {
			loc = event_info.loc1;
		}

		screen->remove_overlay(loc);
	}

	else if(cmd == "unit_overlay") {
		for(units_map::iterator itor = units->begin(); itor != units->end(); ++itor) {
			if(game_events::unit_matches_filter(itor,cfg)) {
				itor->second.add_overlay(cfg["image"]);
				break;
			}
		}
	}

	else if(cmd == "remove_unit_overlay") {
		for(units_map::iterator itor = units->begin(); itor != units->end(); ++itor) {
			if(game_events::unit_matches_filter(itor,cfg)) {
				itor->second.remove_overlay(cfg["image"]);
				break;
			}
		}
	}

	//hiding units
	else if(cmd == "hide_unit") {
		const gamemap::location loc = cfg_to_loc(cfg);
		unit_map::iterator u = units->find(loc);
		if(u != units->end()) {
			u->second.set_hidden(true);
			screen->invalidate(loc);
			screen->draw();
		}
	}

	else if(cmd == "unhide_unit") {
		const gamemap::location loc = cfg_to_loc(cfg);
		unit_map::iterator u;
		// unhide all for backward compatibility
		for(u =  units->begin(); u != units->end() ; u++) {
			u->second.set_hidden(false);
			screen->invalidate(loc);
			screen->draw();
		}
	}

	//adding new items
	else if(cmd == "item") {
		gamemap::location loc = cfg_to_loc(cfg);
		std::string img = cfg["image"];
		std::string halo = cfg["halo"];
		wassert(state_of_game != NULL);
		img = utils::interpolate_variables_into_string(img, *state_of_game);
		halo = utils::interpolate_variables_into_string(halo, *state_of_game);
		if(!img.empty() || !halo.empty()) {
			screen->add_overlay(loc,img,halo);
			screen->invalidate(loc);
			screen->draw();
		}
	}

	//changing the terrain
	else if(cmd == "terrain") {
		const std::vector<gamemap::location> locs = multiple_locs(cfg);

		for(std::vector<gamemap::location>::const_iterator loc = locs.begin(); loc != locs.end(); ++loc) {
			const std::string& terrain_type = cfg["letter"];
			preferences::encountered_terrains().insert(terrain_type);
			if(terrain_type.size() > 0) {
				const bool old_village = game_map->is_village(*loc);
				const bool new_village = game_map->is_village(terrain_type[0]);

				if(old_village && !new_village) {
					int owner = village_owner(*loc, *teams);
					if(owner != -1) {
						(*teams)[owner].lose_village(*loc);
					}
				}

				game_map->set_terrain(*loc,terrain_type[0]);
			}
		}
		rebuild_screen_ = true;
	}

	//creating a mask of the terrain
	else if(cmd == "terrain_mask") {
		gamemap::location loc = cfg_to_loc(cfg, 1, 1);

		gamemap mask(*game_map);

		try {
			mask.read(cfg["mask"]);
		} catch(gamemap::incorrect_format_exception&) {
			ERR_NG << "terrain mask is in the incorrect format, and couldn't be applied\n";
			return rval;
		}

		game_map->overlay(mask, cfg.get_parsed_config(), loc.x, loc.y);
		screen->recalculate_minimap();
		screen->invalidate_all();
		screen->rebuild_all();
	}

	//if we should spawn a new unit on the map somewhere
	else if(cmd == "unit") {
		wassert(game_data_ptr != NULL);
		wassert(units != NULL);
		wassert(game_map != NULL);
		wassert(status_ptr != NULL);
		unit new_unit(game_data_ptr,units,game_map,status_ptr,teams,cfg.get_parsed_config());
		preferences::encountered_units().insert(new_unit.id());
		gamemap::location loc = cfg_to_loc(cfg);

		if(game_map->on_board(loc)) {
			loc = find_vacant_tile(*game_map,*units,loc);
			const bool show = screen != NULL && !screen->turbo() &&
				!screen->fogged(loc.x,loc.y) && cfg["animate"] != "";
			if (show) {
				screen->draw(true,true);
			}

			units->insert(std::pair<gamemap::location,unit>(loc,new_unit));
			if(game_map->is_village(loc)) {
				get_village(loc,*teams,new_unit.side()-1,*units);
			}

			screen->invalidate(loc);

			unit_map::iterator un = units->find(loc);

			if(show) {
				un->second.set_hidden(true);
				screen->scroll_to_tile(loc.x,loc.y,display::ONSCREEN);
				un->second.set_hidden(false);
				un->second.set_recruited(*screen,un->first);
				while(!un->second.get_animation()->animation_finished()) {
					screen->invalidate(loc);
					screen->draw();
					events::pump();
					if(!screen->turbo()) SDL_Delay(10);

				}
				un->second.set_standing(*screen,un->first);
			}
		} else {
			player_info* const player = state_of_game->get_player((*teams)[new_unit.side()-1].save_id());

			if(player != NULL) {
				player->available_units.push_back(new_unit);
			} else {
				ERR_NG << "Cannot create unit: location is not on the map, and player "
					<< new_unit.side() << " has no recall list.\n";
			}
		}
	}

	//if we should recall units that match a certain description
	else if(cmd == "recall") {
		LOG_NG << "recalling unit...\n";
		for(int index = 0; index < int(teams->size()); ++index) {
			LOG_NG << "for side " << index << "...\n";
			player_info* const player = state_of_game->get_player((*teams)[index].save_id());

			if(player == NULL) {
				ERR_NG << "player not found!\n";
				continue;
			}

			std::vector<unit>& avail = player->available_units;

			for(std::vector<unit>::iterator u = avail.begin(); u != avail.end(); ++u) {
				LOG_NG << "checking unit against filter...\n";
				wassert(game_data_ptr != NULL);
				u->set_game_context(game_data_ptr,units,game_map,status_ptr,teams);
				if(game_events::unit_matches_filter(*u, cfg,gamemap::location())) {
					gamemap::location loc = cfg_to_loc(cfg);
					recruit_unit(*game_map,index+1,*units,*u,loc,utils::string_bool(cfg["show"],true) ? NULL : screen,false,true);
					avail.erase(u);
					break;
				}
			}
		}
	} else if(cmd == "object") {
		const vconfig filter = cfg.child("filter");

		std::string id = cfg["id"];
		wassert(state_of_game != NULL);
		id = utils::interpolate_variables_into_string(id, *state_of_game);

		//if this item has already been used
		if(id != "" && used_items.count(id))
			return rval;

		std::string image = cfg["image"];
		std::string caption = cfg["name"];

		image = utils::interpolate_variables_into_string(image, *state_of_game);
		caption = utils::interpolate_variables_into_string(caption, *state_of_game);

		std::string text;

		gamemap::location loc;
		if(!filter.null()) {
			for(unit_map::const_iterator u = units->begin(); u != units->end(); ++u) {
				if(game_events::unit_matches_filter(u, filter)) {
					loc = u->first;
					break;
				}
			}
		}

		if(loc.valid() == false) {
			loc = event_info.loc1;
		}

		const unit_map::iterator u = units->find(loc);

		std::string command_type = "then";

		if(u != units->end() && (filter.null() || game_events::unit_matches_filter(u, filter))) {
			text = cfg["description"];
			text = utils::interpolate_variables_into_string(text, *state_of_game);

			u->second.add_modification("object", cfg.get_parsed_config());

			screen->select_hex(event_info.loc1);
			screen->invalidate_unit();

			//mark that this item won't be used again
			used_items.insert(id);
		} else {
			text = cfg["cannot_use_message"];
			text = utils::interpolate_variables_into_string(text, *state_of_game);

			command_type = "else";
		}

		if(!utils::string_bool(cfg["silent"])) {
			surface surface(NULL);

			if(image.empty() == false) {
				surface.assign(image::get_image(image,image::UNSCALED));
			}

			//this will redraw the unit, with its new stats
			screen->draw();

			gui::show_dialog(*screen,surface,caption,text);
		}

		const vconfig::child_list commands = cfg.get_children(command_type);
		for(vconfig::child_list::const_iterator cmd = commands.begin();
		    cmd != commands.end(); ++cmd) {
			handle_event(event_info, *cmd);
		}
	}

	//displaying a message on-screen
	else if(cmd == "print") {
		const std::string& text = cfg["text"];
		const int size = lexical_cast_default<int>(cfg["size"],font::SIZE_SMALL);
		const int lifetime = lexical_cast_default<int>(cfg["duration"],50);
		const int red = lexical_cast_default<int>(cfg["red"],0);
		const int green = lexical_cast_default<int>(cfg["green"],0);
		const int blue = lexical_cast_default<int>(cfg["blue"],0);

		SDL_Color colour = {red,green,blue,255};

		//remove any old one.
		if (floating_label)
			font::remove_floating_label(floating_label);

		const std::string& msg = text;
		if(msg != "") {
			const SDL_Rect rect = screen->map_area();
			floating_label = font::add_floating_label(msg,size,colour,
									 rect.w/2,rect.h/2,
			                         0.0,0.0,lifetime,rect,font::CENTER_ALIGN);
		}
	}

	//displaying a message dialog
	else if(cmd == "message") {
		units_map::iterator speaker = units->end();
		if(cfg["speaker"] == "unit") {
			speaker = units->find(event_info.loc1);
		} else if(cfg["speaker"] == "second_unit") {
			speaker = units->find(event_info.loc2);
		} else if(cfg["speaker"] != "narrator") {
			for(speaker = units->begin(); speaker != units->end(); ++speaker){
				if(game_events::unit_matches_filter(speaker,cfg))
					break;
			}
		}

		if(speaker == units->end() && cfg["speaker"] != "narrator") {
			//no matching unit found, so the dialog can't come up
			//continue onto the next message
			WRN_NG << "cannot show message\n";
			return rval;
		}

		if(speaker != units->end()) {
			LOG_NG << "set speaker to '" << speaker->second.description() << "'\n";
		} else {
			LOG_NG << "no speaker\n";
		}

		const std::string& sfx = cfg["sound"];
		if(sfx != "") {
			sound::play_sound(sfx);
		}

		std::string image = cfg["image"];
		std::string caption = cfg["caption"];

		if(speaker != units->end()) {
			LOG_DP << "scrolling to speaker..\n";
			screen->highlight_hex(speaker->first);
			screen->scroll_to_tile(speaker->first.x,speaker->first.y-1);

			if(image.empty()) {
				image = speaker->second.profile();
			}

			if(caption.empty()) {
				caption = speaker->second.description();
				if(caption.empty()) {
					caption = speaker->second.language_name();
				}
			}
			LOG_DP << "done scrolling to speaker...\n";
		}

		std::vector<std::string> options;
		std::vector<vconfig::child_list> option_events;

		const vconfig::child_list menu_items = cfg.get_children("option");
		for(vconfig::child_list::const_iterator mi = menu_items.begin();
				mi != menu_items.end(); ++mi) {

			options.push_back((*mi)["message"]);
			option_events.push_back((*mi).get_children("command"));
		}

		surface surface(NULL);
		if(image.empty() == false) {
			surface.assign(image::get_image(image,image::UNSCALED));
		}

		int option_chosen = -1;

		LOG_DP << "showing dialog...\n";

		//if we're not replaying, or if we are replaying and there is no choice
		//to be made, show the dialog.
		if(get_replay_source().at_end() || options.empty()) {
			const std::string msg = utils::interpolate_variables_into_string(cfg["message"], *state_of_game);
			const SDL_Rect& map_area = screen->map_area();
			option_chosen = gui::show_dialog2(*screen,surface,caption,msg,
		                        options.empty() ? gui::MESSAGE : gui::OK_ONLY,
		                        options.empty() ? NULL : &options,
								NULL, "", NULL, 256, NULL, NULL, -1, map_area.y+4);

			LOG_DP << "showed dialog...\n";

			if (option_chosen == gui::ESCAPE_DIALOG){
				rval = false;
			}

			if(options.empty() == false) {
				recorder.choose_option(option_chosen);
			}
		}

		//otherwise if a choice has to be made, get it from the replay data
		else {
			const config* const action = get_replay_source().get_next_action();
			if(action == NULL || action->get_children("choose").empty()) {
				ERR_NG << "choice expected but none found\n";
				throw replay::error();
			}

			const std::string& val = (*(action->get_children("choose").front()))["value"];
			option_chosen = atol(val.c_str());
		}

		//implement the consequences of the choice
		if(options.empty() == false) {
			wassert(size_t(option_chosen) < menu_items.size());

			vconfig::child_list events = option_events[option_chosen];
			for(vconfig::child_list::const_iterator itor = events.begin();
					itor != events.end(); ++itor) {

				handle_event(event_info, *itor);
			}
		}
	}

	else if(cmd == "kill") {
		unit_map::iterator un = units->begin();
		while(un != units->end()) {
			if(game_events::unit_matches_filter(un,cfg)) {
				if(utils::string_bool(cfg["animate"])) {
					screen->scroll_to_tile(un->first.x,un->first.y);
					unit_display::unit_die(*screen,un->first,un->second);
				}

				if(utils::string_bool(cfg["fire_event"])) {
					gamemap::location loc = un->first;
					game_events::fire("die",loc,un->first);
					un = units->find(loc);
					if(un == units->end()) {
						un = units->begin();
						continue;
					}
				}
				units->erase(un++);
			} else {
				++un;
			}
		}

		//if the filter doesn't contain positional information, then it may match
                //units on all recall lists.
		if(cfg["x"].empty() && cfg["y"].empty()) {
                  std::map<std::string, player_info>& players=state_of_game->players;

                  for(std::map<std::string, player_info>::iterator pi = players.begin();
                      pi!=players.end(); ++pi) {
                        std::vector<unit>& avail_units = pi->second.available_units;
			for(std::vector<unit>::iterator j = avail_units.begin(); j != avail_units.end();) {
				wassert(game_data_ptr != NULL);
				j->set_game_context(game_data_ptr,units,game_map,status_ptr,teams);
				if(game_events::unit_matches_filter(*j, cfg,gamemap::location())) {
                            j = avail_units.erase(j);
                          } else {
                            ++j;
                          }
			}
                  }
		}
	}

	//adding of new events
	else if(cmd == "event") {
		event_handler new_handler(cfg.get_config());
		events_map.insert(std::pair<std::string,event_handler>(new_handler.name(),new_handler));
	}

	//unit serialization to and from variables
	else if(cmd == "store_unit") {
		const config empty_filter;
		vconfig filter = cfg.child("filter");
		if(filter.null())
			filter = &empty_filter;

		const std::string& variable = cfg["variable"];
		const std::string& mode = cfg["mode"];
		bool cleared = false;

		config& vars = state_of_game->variables;
		if(mode != "replace" && mode != "append") {
			vars.clear_children(variable);
		}

		const bool kill_units = utils::string_bool(cfg["kill"]);

		for(unit_map::iterator i = units->begin(); i != units->end();) {
			if(game_events::unit_matches_filter(i,filter) == false) {
				++i;
				continue;
			}

			if(mode == "replace" && !cleared) {
				vars.clear_children(variable);
				cleared = true;
			}
			config& data = vars.add_child(variable);
			i->first.write(data);
			i->second.write(data);

			if(kill_units) {
				units->erase(i++);
			} else {
				++i;
			}
		}

		if(filter["x"].empty() && filter["y"].empty()) {
			std::map<std::string, player_info>& players = state_of_game->players;

			for(std::map<std::string, player_info>::iterator pi = players.begin();
					pi!=players.end(); ++pi) {
				std::vector<unit>& avail_units = pi->second.available_units;
				for(std::vector<unit>::iterator j = avail_units.begin(); j != avail_units.end();) {
				wassert(game_data_ptr != NULL);
				j->set_game_context(game_data_ptr,units,game_map,status_ptr,teams);
				if(game_events::unit_matches_filter(*j, filter,gamemap::location()) == false) {
						++j;
						continue;
					}

					if(mode == "replace" && !cleared) {
						vars.clear_children(variable);
						cleared = true;
					}
					config& data = vars.add_child(variable);
					j->write(data);
					data["x"] = "recall";
					data["y"] = "recall";

					if(kill_units) {
						j = avail_units.erase(j);
					} else {
						++j;
					}
				}
			}
		}
	}

	else if(cmd == "unstore_unit") {
		wassert(state_of_game != NULL);
		const config& var = state_of_game->get_variable_cfg(
			utils::interpolate_variables_into_string(cfg.get_attribute("variable"),
				*state_of_game));

		try {
			wassert(game_data_ptr != NULL);
			wassert(units != NULL);
			wassert(game_map != NULL);
			wassert(status_ptr != NULL);
			const unit u(game_data_ptr,units,game_map,status_ptr,teams,var);
			gamemap::location loc(var);
			if(loc.valid()) {
				if(utils::string_bool(cfg["find_vacant"])) {
					loc = find_vacant_tile(*game_map,*units,loc);
				}

				units->erase(loc);
				units->insert(std::pair<gamemap::location,unit>(loc,u));
				
				const std::string& text = cfg["text"];
				if(!text.empty())
				{
					//Print floating label
					const int red = lexical_cast_default<int>(cfg["red"],0);
					const int green = lexical_cast_default<int>(cfg["green"],0);
					const int blue = lexical_cast_default<int>(cfg["blue"],0);
					{
						screen->float_label(loc,text,red,green,blue);
					}
				}
			} else {
				player_info *player=state_of_game->get_player((*teams)[u.side()-1].save_id());

				if(player) {
					player->available_units.push_back(u);
				} else {
					ERR_NG << "Cannot unstore unit: no recall list for player " << u.side()
						<< " and the map location is invalid.\n";
				}
			}
		} catch(game::load_game_failed& e) {
			ERR_NG << "could not de-serialize unit: '" << e.message << "'\n";
		}
	}

	else if(cmd == "store_starting_location") {
		const int side = lexical_cast_default<int>(cfg["side"]);
		const gamemap::location& loc = game_map->starting_position(side);
		static const t_string default_store = "location";
		const t_string& store = cfg["variable"].empty() ? default_store : cfg["variable"];
		wassert(state_of_game != NULL);
		config &loc_store = state_of_game->get_variable_cfg(store);
		loc.write(loc_store);
		game_map->write_terrain(loc, loc_store);
	}

	else if(cmd == "store_locations") {
		log_scope("store_locations");
		const std::string& variable = cfg["variable"];
		const std::string& terrain = cfg["terrain"];
		const vconfig unit_filter = cfg.child("filter");

		state_of_game->variables.clear_children(variable);

		std::vector<gamemap::location> locs = parse_location_range(cfg["x"],cfg["y"]);
		if(locs.size() > MaxLoop) {
			locs.resize(MaxLoop);
		}

		const size_t radius = minimum<size_t>(MaxLoop,lexical_cast_default<size_t>(cfg["radius"]));
		std::set<gamemap::location> res;
		get_tiles_radius(*game_map, locs, radius, res);

		size_t added = 0;
		for(std::set<gamemap::location>::const_iterator j = res.begin(); j != res.end() && added != MaxLoop; ++j) {
			if (terrain.empty() == false) {
				const gamemap::TERRAIN c = game_map->get_terrain(*j);
				if(std::find(terrain.begin(), terrain.end(), c) == terrain.end())
					continue;
			}
			if (!unit_filter.null()) {
				const unit_map::const_iterator u = units->find(*j);
				if (u == units->end() || !game_events::unit_matches_filter(u, unit_filter))
					continue;
			}
			config &loc_store = state_of_game->variables.add_child(variable);
			j->write(loc_store);
			game_map->write_terrain(*j, loc_store);
			++added;
		}
	}

	//command to take control of a village for a certain side
	else if(cmd == "capture_village") {
		const int side = lexical_cast_default<int>(cfg["side"]);

		//if 'side' is 0, then it will become an invalid index, and so
		//the village will become neutral.
		const size_t team_num = size_t(side-1);

		const std::vector<gamemap::location> locs(multiple_locs(cfg));

		for(std::vector<gamemap::location>::const_iterator i = locs.begin(); i != locs.end(); ++i) {
			if(game_map->is_village(*i)) {
				get_village(*i,*teams,team_num,*units);
			}
		}
	}

	//command to remove a variable
	else if(cmd == "clear_variable") {
		const std::string& name = utils::interpolate_variables_into_string(
			cfg.get_attribute("name"), *state_of_game);
		state_of_game->clear_variable(name);
	}

	else if(cmd == "endlevel") {
		const std::string& next_scenario = cfg["next_scenario"];
		if(next_scenario.empty() == false) {
			state_of_game->scenario = next_scenario;
		}

		const std::string& result = cfg["result"];
		if(result.empty() || result == "victory") {
			const bool bonus = utils::string_bool(cfg["bonus"],true);
			throw end_level_exception(VICTORY,bonus);
		} else if(result == "continue") {
			throw end_level_exception(LEVEL_CONTINUE);
		} else if(result == "continue_no_save") {
			throw end_level_exception(LEVEL_CONTINUE_NO_SAVE);
		} else {
			LOG_NG << "throwing event defeat...\n";
			throw end_level_exception(DEFEAT);
		}
	}

	else if(cmd == "redraw") {
		screen->invalidate_all();
		screen->draw(true,true);
	}

	else if(cmd == "animate_unit") {

		unit_map::iterator u = units->find(event_info.loc1);

		//search for a valid unit filter, and if we have one, look for the matching unit
		const vconfig filter = cfg.child("filter");
		if(!filter.null()) {
			for(u = units->begin(); u != units->end(); ++u){
				if(game_events::unit_matches_filter(u, filter))
					break;
			}
		}

		//we have found a unit that matches the filter
		if(u != units->end() && ! screen->fogged(u->first.x,u->first.y)) {
			screen->highlight_hex(u->first);
			screen->scroll_to_tile(u->first.x,u->first.y);

			u->second.set_extra_anim(*screen,u->first,cfg["flag"]);
			while(!u->second.get_animation()->animation_finished()) {
				screen->invalidate(u->first);
				screen->draw();
				events::pump();
				if(!screen->turbo()) SDL_Delay(10);

			}
			u->second.set_standing(*screen,u->first);
			screen->invalidate(u->first);
			screen->draw();
			events::pump();
		}
	} else if(cmd == "label") {
		const gamemap::location loc(lexical_cast_default<int>(cfg["x"],-1),lexical_cast_default<int>(cfg["y"],-1));
		const std::string& text = cfg["text"];
		screen->labels().set_label(loc,text);
	}
	
	LOG_NG << "done handling command...\n";

	return rval;
}

bool event_handler::handle_event(const queued_event& event_info, const vconfig conf)
{
	vconfig cfg = conf;

	if(cfg.null())
		cfg = cfg_;

	bool mutated = true;

	bool skip_messages = false;
	for(config::all_children_iterator i = cfg.get_config().ordered_begin();
			i != cfg.get_config().ordered_end(); ++i) {

		const std::pair<const std::string*,const config*> item = *i;

		// If the user pressed escape, we skip any message that doesn't
		// require them to make a choice.
		if ((skip_messages) && (*item.first == "message")) {
			if ((item.second)->get_children("option").size() == 0) {
				continue;
			}
		}

		if (!handle_event_command(event_info, *item.first, vconfig(item.second), mutated)) {
			skip_messages = true;
		}
		else {
			skip_messages = false;
		}
	}

	// We do this once event has completed any music alterations
	sound::commit_music_changes();
	return mutated;
}

bool filter_loc_impl(const gamemap::location& loc, const std::string& xloc,
                                                   const std::string& yloc)
{
	if(std::find(xloc.begin(),xloc.end(),',') != xloc.end()) {
		std::vector<std::string> xlocs = utils::split(xloc);
		std::vector<std::string> ylocs = utils::split(yloc);

		const int size = xlocs.size() < ylocs.size()?xlocs.size():ylocs.size();
		for(int i = 0; i != size; ++i) {
			if(filter_loc_impl(loc,xlocs[i],ylocs[i]))
				return true;
		}

		return false;
	}

	if(!xloc.empty()) {
		const std::string::const_iterator dash =
		             std::find(xloc.begin(),xloc.end(),'-');

		if(dash != xloc.end()) {
			const std::string beg(xloc.begin(),dash);
			const std::string end(dash+1,xloc.end());

			const int bot = atoi(beg.c_str()) - 1;
			const int top = atoi(end.c_str()) - 1;

			if(loc.x < bot || loc.x > top)
				return false;
		} else {
			const int xval = atoi(xloc.c_str()) - 1;
			if(xval != loc.x)
				return false;
		}
	}

	if(!yloc.empty()) {
		const std::string::const_iterator dash =
		             std::find(yloc.begin(),yloc.end(),'-');

		if(dash != yloc.end()) {
			const std::string beg(yloc.begin(),dash);
			const std::string end(dash+1,yloc.end());

			const int bot = atoi(beg.c_str()) - 1;
			const int top = atoi(end.c_str()) - 1;

			if(loc.y < bot || loc.y > top)
				return false;
		} else {
			const int yval = atoi(yloc.c_str()) - 1;
			if(yval != loc.y)
				return false;
		}
	}

	return true;
}

bool filter_loc(const gamemap::location& loc, const vconfig cfg)
{
	const std::string& xloc = cfg["x"];
	const std::string& yloc = cfg["y"];

	return filter_loc_impl(loc,xloc,yloc);
}

bool process_event(event_handler& handler, const queued_event& ev)
{
	if(handler.disabled())
		return false;

	unit_map::iterator unit1 = units->find(ev.loc1);
	unit_map::iterator unit2 = units->find(ev.loc2);

	const vconfig::child_list first_filters = handler.first_arg_filters();
	vconfig::child_list::const_iterator ffi;
	for(ffi = first_filters.begin();
	    ffi != first_filters.end(); ++ffi) {

		if(unit1 == units->end() || !game_events::unit_matches_filter(unit1,*ffi)) {
			return false;
		}
	}
	bool special_matches = false;
	const vconfig::child_list first_special_filters = handler.first_special_filters();
	special_matches = first_special_filters.size() ? false : true;
	for(ffi = first_special_filters.begin();
	    ffi != first_special_filters.end(); ++ffi) {

		if(unit1 != units->end() && game_events::matches_special_filter(ev.data.child("first"),*ffi)) {
			special_matches = true;
		}
	}
	if(!special_matches) {
		return false;
	}

	const vconfig::child_list second_filters = handler.second_arg_filters();
	for(vconfig::child_list::const_iterator sfi = second_filters.begin();
	    sfi != second_filters.end(); ++sfi) {
		if(unit2 == units->end() || !game_events::unit_matches_filter(unit2,*sfi)) {
			return false;
		}
	}
	const vconfig::child_list second_special_filters = handler.second_special_filters();
	special_matches = second_special_filters.size() ? false : true;
	for(ffi = second_special_filters.begin();
	    ffi != second_special_filters.end(); ++ffi) {

		if(unit2 != units->end() && game_events::matches_special_filter(ev.data.child("second"),*ffi)) {
			special_matches = true;
		}
	}
	if(!special_matches) {
		return false;
	}

	//the event hasn't been filtered out, so execute the handler
	const bool res = handler.handle_event(ev);
	
	if(handler.rebuild_screen()) {
		handler.rebuild_screen() = false;
		screen->recalculate_minimap();
		screen->invalidate_all();
		screen->rebuild_all();
	}
	
	if(handler.first_time_only()) {
		handler.disable();
	}

	return res;
}

} //end anonymous namespace

namespace game_events {

bool matches_special_filter(const config* cfg, const vconfig filter)
{
	if(!cfg) {
		return false;
	}
	if(filter["weapon"] != "") {
		if(filter["weapon"] != (*cfg)["weapon"]) {
			return false;
		}
	}
	
	const vconfig::child_list& nots = filter.get_children("not");
	for(vconfig::child_list::const_iterator i = nots.begin(); i != nots.end(); ++i) {
		if(matches_special_filter(cfg,*i)) {
			return false;
		}
	}
	return true;
}

bool unit_matches_filter(const unit& u, const vconfig filter,const gamemap::location& loc)
{
	const bool res = u.matches_filter(filter.get_parsed_config(),loc);
	if(res == true) {
		const vconfig::child_list& nots = filter.get_children("not");
		for(vconfig::child_list::const_iterator i = nots.begin(); i != nots.end(); ++i) {
			if(unit_matches_filter(u,*i,loc)) {
				return false;
			}
		}
	}

	return res;
}

bool unit_matches_filter(unit_map::const_iterator itor, const vconfig filter)
{
	const bool res = filter_loc(itor->first,filter) && itor->second.matches_filter(filter.get_parsed_config(),itor->first);
	if(res == true) {
		const vconfig::child_list& nots = filter.get_children("not");
		for(vconfig::child_list::const_iterator i = nots.begin(); i != nots.end(); ++i) {
			if(unit_matches_filter(itor,*i)) {
				return false;
			}
		}
	}

	return res;
}

config::child_list unit_wml_configs;
std::set<std::string> unit_wml_ids;

manager::manager(const config& cfg, display& gui_, gamemap& map_,
                 units_map& units_,
                 std::vector<team>& teams_,
                 game_state& state_of_game_, gamestatus& status,
		 const game_data& game_data_) :
	variable_manager(&state_of_game_)
{
	const config::child_list& events_list = cfg.get_children("event");
	for(config::child_list::const_iterator i = events_list.begin();
	    i != events_list.end(); ++i) {
		event_handler new_handler(**i);
		events_map.insert(std::pair<std::string,event_handler>(
					new_handler.name(), new_handler));
	}
	std::vector<std::string> unit_ids = utils::split(cfg["unit_wml_ids"]);
	for(std::vector<std::string>::const_iterator id_it = unit_ids.begin(); id_it != unit_ids.end(); ++id_it) {
		unit_wml_ids.insert(*id_it);
	}

	teams = &teams_;
	screen = &gui_;
	game_map = &map_;
	units = &units_;
	state_of_game = &state_of_game_;
	game_data_ptr = &game_data_;
	status_ptr = &status;

	used_items.clear();
	
	const std::string& used = cfg["used_items"];
	if(!used.empty()) {
		const std::vector<std::string>& v = utils::split(used);
		for(std::vector<std::string>::const_iterator i = v.begin(); i != v.end(); ++i) {
			used_items.insert(*i);
		}
	}
}

void write_events(config& cfg)
{
	for(std::multimap<std::string,event_handler>::const_iterator i = events_map.begin(); i != events_map.end(); ++i) {
		if(!i->second.disabled()) {
			i->second.write(cfg.add_child("event"));
		}
	}

	std::stringstream used;
	std::set<std::string>::const_iterator u;
	for(u = used_items.begin(); u != used_items.end(); ++u) {
		if(u != used_items.begin())
			used << ",";

		used << *u;
	}

	cfg["used_items"] = used.str();
	std::stringstream ids;
	for(u = unit_wml_ids.begin(); u != unit_wml_ids.end(); ++u) {
		if(u != unit_wml_ids.begin())
			ids << ",";

		ids << *u;
	}

	cfg["unit_wml_ids"] = ids.str();

	if(screen != NULL)
		screen->write_overlays(cfg);
}

manager::~manager() {
	events_queue.clear();
	events_map.clear();
	screen = NULL;
	game_map = NULL;
	units = NULL;
	state_of_game = NULL;
	game_data_ptr = NULL;
	status_ptr = NULL;
	for(config::child_list::iterator d = unit_wml_configs.begin(); d != unit_wml_configs.end(); ++d) {
		delete *d;
	}
	unit_wml_configs.clear();
	unit_wml_ids.clear();
}

void raise(const std::string& event,
           const gamemap::location& loc1,
           const gamemap::location& loc2,
		   const config& data)
{
	if(!events_init())
		return;

	events_queue.push_back(queued_event(event,loc1,loc2,data));
}

bool fire(const std::string& event,
          const gamemap::location& loc1,
          const gamemap::location& loc2,
		  const config& data)
{
	raise(event,loc1,loc2,data);
	return pump();
}

bool pump()
{
	if(!events_init())
		return false;

	bool result = false;
	
	if(units != NULL) {
		for(units_map::const_iterator u_it = units->begin(); u_it != units->end(); ++u_it) {
			if(std::find(unit_wml_ids.begin(),unit_wml_ids.end(),u_it->second.id()) == unit_wml_ids.end()) {
				unit_wml_ids.insert(u_it->second.id());
				config::child_list ev_list = u_it->second.wml_events();
				for(config::child_list::const_iterator new_ev = ev_list.begin(); new_ev != ev_list.end(); ++ new_ev) {
					unit_wml_configs.push_back(new config(**new_ev));
					event_handler new_handler(*unit_wml_configs.back());
					events_map.insert(std::pair<std::string,event_handler>(new_handler.name(), new_handler));
				}
			}
		}
	}

	while(events_queue.empty() == false) {
		queued_event ev = events_queue.front();
		events_queue.pop_front(); //pop now for exception safety
		const std::string& event_name = ev.name;
		typedef std::multimap<std::string,event_handler>::iterator itor;

		//find all handlers for this event in the map
		std::pair<itor,itor> i = events_map.equal_range(event_name);

		//set the variables for the event
		if(i.first != i.second && state_of_game != NULL) {
			char buf[50];
			snprintf(buf,sizeof(buf),"%d",ev.loc1.x+1);
			state_of_game->variables["x1"] = buf;

			snprintf(buf,sizeof(buf),"%d",ev.loc1.y+1);
			state_of_game->variables["y1"] = buf;

			snprintf(buf,sizeof(buf),"%d",ev.loc2.x+1);
			state_of_game->variables["x2"] = buf;

			snprintf(buf,sizeof(buf),"%d",ev.loc2.y+1);
			state_of_game->variables["y2"] = buf;
		}

		while(i.first != i.second) {
			LOG_NG << "processing event '" << event_name << "'\n";
			event_handler& handler = i.first->second;
			if(process_event(handler, ev))
				result = true;
			++i.first;
		}
	}

	return result;
}

} //end namespace game_events
