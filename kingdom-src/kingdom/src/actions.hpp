/* $Id: actions.hpp 40489 2010-01-01 13:16:49Z mordante $ */
/*
   Copyright (C) 2003 - 2010 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2
   or at your option any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 * @file src/actions.hpp
 * Various functions which implement in-game events and commands.
 */

#ifndef ACTIONS_H_INCLUDED
#define ACTIONS_H_INCLUDED

class attack_type;
class game_display;
class replay;
struct combatant;
class team;
struct time_of_day;

#include "global.hpp"
#include "map_location.hpp"
#include "unit.hpp"
#include "unit_map.hpp"

#include <deque>

/** Computes the statistics of a battle between an attacker and a defender unit. */
class battle_context
{
public:
	/** Structure describing the statistics of a unit involved in the battle. */
	struct unit_stats
	{
		const attack_type *weapon;	/**< The weapon used by the unit to attack the opponent, or NULL if there is none. */
		int attack_num;			/**< Index into unit->attacks() or -1 for none. */
		bool is_attacker;		/**< True if the unit is the attacker. */
		bool is_poisoned;		/**< True if the unit is poisoned at the beginning of the battle. */
		bool is_slowed;			/**< True if the unit is slowed at the beginning of the battle. */
		bool is_broken;			/**< True if the unit is broken at the beginning of the battle. */
		bool is_reinforced;		/**< True if the unit is reinforced at the beginning of the battle. */
		bool slows;				/**< Attack slows opponent when it hits. */
		bool breaks;			/**< Attack breaks opponent when it hits. */
		bool drains;			/**< Attack drains opponent when it hits. */
		bool petrifies;			/**< Attack petrifies opponent when it hits. */
		bool plagues;			/**< Attack turns opponent into a zombie when fatal. */
		bool poisons;			/**< Attack poisons opponent when it hits. */
		bool backstab_pos;		/**<
		                         * True if the attacker is in *position* to backstab the defender (this is used to
								 * determine whether to apply the backstab bonus in case the attacker has backstab).
								 */
		bool swarm;				/**< Attack has swarm special. */
		bool firststrike;		/**< Attack has firststrike special. */
		unsigned int experience, max_experience;
		unsigned int level;

		unsigned int rounds;	/**< Berserk special can force us to fight more than one round. */
		unsigned int hp;		/**< Hitpoints of the unit at the beginning of the battle. */
		unsigned int max_hp;	/**< Maximum hitpoints of the unit. */
		unsigned int chance_to_hit;	/**< Effective chance to hit as a percentage (all factors accounted for). */
		int damage;				/**< Effective damage of the weapon (all factors accounted for). */
		unsigned int num_blows;	/**< Effective number of blows, takes swarm into account. */
		unsigned int swarm_min;	/**< Minimum number of blows with swarm (equal to num_blows if swarm isn't used). */
		unsigned int swarm_max;	/**< Maximum number of blows with swarm (equal to num_blows if swarm isn't used). */

		std::string plague_type; /**< The plague type used by the attack, if any. */

		unit_stats(const unit &u, const map_location& u_loc,
				   int u_attack_num, bool attacking,
				   const unit &opp, const map_location& opp_loc,
				   const attack_type *opp_weapon,
				   const unit_map& units);
		~unit_stats();

		/** Dumps the statistics of a unit on stdout. Remove it eventually. */
		void dump() const;
	};

	/**
	 * If no attacker_weapon is given, we select the best one,
	 * based on harm_weight (1.0 means 1 hp lost counters 1 hp damage,
	 * 0.0 means we ignore harm weight).
	 * prev_def is for predicting multiple attacks against a defender.
	 */
	battle_context(const unit_map &units,
				   const unit& attacker, const unit& defender,
				   int attacker_weapon, int defender_weapon, double aggression, const combatant *prev_def = NULL);

	battle_context(const unit_map &units,
				   const unit& attacker, const unit& defender,
				   int attacker_weapon = -1, int defender_weapon = -1);

	/** Used by the AI which caches unit_stats */
	battle_context(const unit_stats &att, const unit_stats &def);

	battle_context(const battle_context &other);
	~battle_context();

	battle_context& operator=(const battle_context &other);

	/** This method returns the statistics of the attacker. */
	const unit_stats& get_attacker_stats() const { return *attacker_stats_; }

	/** This method returns the statistics of the defender. */
	const unit_stats& get_defender_stats() const { return *defender_stats_; }

	/** Get the simulation results. */
	const combatant &get_attacker_combatant(const combatant *prev_def = NULL);
	const combatant &get_defender_combatant(const combatant *prev_def = NULL);

	/** Given this harm_weight, is this attack better than that? */
	bool better_attack(class battle_context &that, double harm_weight);

private:
	bool better_combat(const combatant &us_a, const combatant &them_a,
					   const combatant &us_b, const combatant &them_b,
					   double harm_weight);

	int choose_attacker_weapon(const unit &attacker, const unit &defender,
		const unit_map& units, double harm_weight, int *defender_weapon, const combatant *prev_def);

	int choose_defender_weapon(const unit &attacker, const unit &defender, unsigned attacker_weapon,
		const unit_map& units,
							   const map_location& attacker_loc, const map_location& defender_loc, const combatant *prev_def);

	/** Statistics of the units. */
	unit_stats *attacker_stats_, *defender_stats_;

	/** Outcome of simulated fight. */
	combatant *attacker_combatant_, *defender_combatant_;
};

/** Performs an attack. */
std::pair<map_location, map_location> attack_unit(unit& attacker, unit& defender,
												  int attack_with, int defend_with, bool update_display = true, const config& duel = config::invalid);


/**
 * Given the location of a village, will return the 0-based index
 * of the team that currently owns it, and -1 if it is unowned.
 */
int village_owner(const map_location& loc, const std::vector<team>& teams);

/**
 * Makes it so the village at the given location is owned by the given side.
 * Returns true if getting the village triggered a mutating event.
 */
bool get_village(const map_location& loc, int side, int *time_bonus = NULL);

/**
 * Resets resting for all units on this side: should be called after calculate_healing().
 * @todo FIXME: Try moving this to unit::new_turn, then move it above calculate_healing().
 */
void reset_resting(unit_map& units, int side);

/**
 * Calculates healing for all units for the given side.
 * Should be called at the beginning of a side's turn.
 */
void calculate_healing(int side, bool update_display);

/**
 * Returns the advanced version of unit (with traits and items retained).
 */
unit* get_advanced_unit(const unit* u, const std::string &advance_to);

/**
 * Function which will advance the unit at @a loc to 'advance_to'.
 * Note that 'loc' is not a reference, because if it were a reference,
 * we couldn't safely pass in a reference to the item in the map
 * that we're going to delete, since deletion would invalidate the reference.
 */
void advance_unit(map_location loc, const std::string &advance_to);

/**
 * function which tests if the unit at loc is currently affected by leadership.
 * (i.e. has a higher-level 'leadership' unit next to it).
 * If it does, then the location of the leader unit will be returned,
 * Otherwise map_location::null_location will be returned.
 * If 'bonus' is not NULL, the % bonus will be stored in it.
 */
map_location under_leadership(const unit_map& units,
                                   const map_location& loc, int* bonus=NULL);

/**
 * Returns the amount that a unit's damage should be multiplied by
 * due to the current time of day.
 */
int combat_modifier(const map_location &loc,
	unit_type::ALIGNMENT alignment, bool is_fearless);

/** Records information to be able to undo a movement. */
struct undo_action {
	undo_action(const unit& u,
		const std::vector<map_location>& rt,
		int sm, int timebonus = 0, int orig = -1, int pos = -1) :
			route(rt),
			starting_moves(sm),
			original_village_owner(orig),
			recall_pos(pos),
			// affected_unit(u),
			countdown_time_bonus(timebonus)
			{}

	std::vector<map_location> route;
	int starting_moves;
	int original_village_owner;
	int recall_pos; // set to RECRUIT_POS for an undo-able recruit
	// unit affected_unit;
	int countdown_time_bonus;
};

typedef std::deque<undo_action> undo_list;

/**
 * function which moves a unit along the sequence of locations given by steps.
 * If the unit cannot make it completely along the path this turn,
 * a goto order will be set.
 * If move_recorder is not NULL, the move will be recorded in it.
 * If undos is not NULL, undo information will be added.
 */
size_t move_unit(move_unit_spectator* move_spectator,
	const std::vector<map_location> &steps,
				replay* move_recorder, undo_list* undos,
				bool show_move,
				map_location *next_unit = NULL,
				bool continue_move = false, bool should_clear_shroud=true, bool is_replay=false);

std::pair<map_location, map_location> attack_enemy(unit* attacker, unit* defender, int att_weapon, int def_weapon);

/** Function which recalculates the fog. */
void recalculate_fog(int side);

/**
 * Function which will clear shroud away for the @a side
 * based on current unit positions.
 * Returns true if some shroud is actually cleared away.
 */
bool clear_shroud(int side);

/**
 * Function to apply pending shroud changes in the undo stack.
 */
void apply_shroud_changes(undo_list &undos, int side);

/**
 * Will return true iff the unit @a u has any possible moves
 * it can do (including attacking etc).
 */
bool unit_can_move(const unit &u);

/**
 * Will return true if the unit @a u has any possible moves or attacks
  */
bool unit_can_action(const unit &u);

/**
 * Function to check if an attack will satisfy the requirements for backstab.
 * Input:
 * - the location from which the attack will occur,
 * - the defending unit location,
 * - the list of units on the map and
 * - the list of teams.
 * The defender and opposite units should be in place already.
 * The attacking unit doesn't need to be, but if it isn't,
 * an external check should be made to make sure the opposite unit
 * isn't also the attacker.
 */
bool backstab_check(const map_location& attacker_loc,
	const map_location& defender_loc,
	const unit_map& units, const std::vector<team>& teams);

void refresh_card_button(const team& t, game_display& disp);
void get_random_card(team& t, game_display& disp, unit_map& units, hero_map& heros);
void erase_random_card(team& t, game_display& disp, unit_map& units, hero_map& heros);

void unit_die(unit_map& units, unit& die, void* a_info_p = NULL, int die_activity = 0, int a_side = HEROS_INVALID_SIDE);

void calculate_wall_tiles(unit_map& units, gamemap& map, artifical& owner, std::vector<map_location*>& keeps, std::vector<map_location*>& wall_vacants, std::vector<map_location*>& keep_vacants);

size_t calculate_keeps(unit_map& units, const artifical& owner);

artifical* find_city_for_wall(unit_map& units, const map_location& loc);

SDL_Rect extend_rectangle(const gamemap& map, const SDL_Rect& src, int radius);

SDL_Rect extend_rectangle(const gamemap& map, int x, int y, int radius);

void post_duel(unit& left_u, hero& left, unit& right_u, hero& right, int percentage);

int calculate_exploiture(const hero& h1, const hero& h2, const hero& h3, int artificals = 0);

void do_recruit(unit_map& units, hero_map& heros, team& current_team, const unit_type* ut, std::vector<const hero*>& v, artifical& city, int cost_exponent, bool human);

#endif
