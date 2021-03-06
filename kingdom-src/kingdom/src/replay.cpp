/* $Id: replay.cpp 42123 2010-04-13 22:02:24Z crab $ */
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
 *  @file replay.cpp
 *  Replay control code.
 *
 *  See http://www.wesnoth.org/wiki/ReplayWML for more info.
 */

#include "global.hpp"

#include "dialogs.hpp"
#include "foreach.hpp"
#include "game_display.hpp"
#include "game_end_exceptions.hpp"
#include "game_events.hpp"
#include "game_preferences.hpp"
#include "gamestatus.hpp"
#include "log.hpp"
#include "map_label.hpp"
#include "map_location.hpp"
#include "play_controller.hpp"
#include "replay.hpp"
#include "resources.hpp"
#include "rng.hpp"
#include "statistics.hpp"
#include "wesconfig.h"
#include "artifical.hpp"
#include "formula_string_utils.hpp"
#include "filesystem.hpp"
#include "wml_exception.hpp"

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filter/gzip.hpp>

command_pool::command_pool()
	: use_gzip_(true)
	, cache_(NULL)
{
	pool_data_ = (uint8_t*)malloc(POOL_ALLOC_DATA_SIZE);
	pool_data_size_ = POOL_ALLOC_DATA_SIZE;
	pool_data_vsize_ = 0;
	pool_data_gzip_size_ = 0;
	pool_pos_ = (unsigned int*)malloc(POOL_ALLOC_POS_SIZE * sizeof(unsigned int));
	pool_pos_size_ = POOL_ALLOC_POS_SIZE;
	pool_pos_vsize_ = 0;

	current_gzip_seg_.min = current_gzip_seg_.max = -1;
}

command_pool::command_pool(const command_pool& that)
{
	use_gzip_ = that.use_gzip_;

	VALIDATE(use_gzip_, "command_pool::operator=, invalid use_gzip_ falg");

	// pool data
	pool_data_size_ = that.pool_data_size_;
	pool_data_vsize_ = that.pool_data_vsize_;
	pool_data_gzip_size_ = that.pool_data_gzip_size_;
	if (pool_data_size_) {
		pool_data_ = (uint8_t*)malloc(pool_data_size_);
	}
	if (pool_data_gzip_size_) {
		memcpy(pool_data_, that.pool_data_, pool_data_gzip_size_);
	}
	// pool pos
	pool_pos_size_ = that.pool_pos_size_;
	pool_pos_vsize_ = that.pool_pos_vsize_;
	if (pool_pos_size_) {
		pool_pos_ = (unsigned int*)malloc(pool_pos_size_ * sizeof(unsigned int));
	}
	if (pool_pos_vsize_) {
		memcpy(pool_pos_, that.pool_pos_, pool_pos_vsize_ * sizeof(unsigned int));
	}
	// cache_
	if (pool_data_vsize_) {
		VALIDATE(false, "command_pool::command_pool, pool_data_vsize_ isn't zero.");
		cache_ = (unsigned char*)malloc(POOL_ALLOC_CACHE_SIZE);
		memcpy(cache_, that.cache_, pool_data_vsize_);
	} else {
		cache_ = NULL;
	}
}

command_pool::~command_pool()
{
	if (pool_data_) {
		free(pool_data_);
	}
	if (pool_pos_) {
		free(pool_pos_);
	}
	if (cache_) {
		free(cache_);
	}
}

command_pool& command_pool::operator=(const command_pool& that)
{
	VALIDATE(use_gzip_ && that.use_gzip_, "command_pool::operator=, invalid use_gzip_ falg");

	use_gzip_ = that.use_gzip_;

	if (pool_data_size_ < that.pool_data_size_) {
		free(pool_data_);
		pool_data_size_ = that.pool_data_size_;
		pool_data_ = (uint8_t*)malloc(pool_data_size_);
	}
	pool_data_vsize_ = that.pool_data_vsize_;
	pool_data_gzip_size_ = that.pool_data_gzip_size_;
	if (pool_data_gzip_size_) {
		memcpy(pool_data_, that.pool_data_, pool_data_gzip_size_);
	}

	if (pool_pos_size_ < that.pool_pos_size_) {
		free(pool_pos_);
		pool_pos_size_ = that.pool_pos_size_;
		pool_pos_ = (unsigned int*)malloc(pool_pos_size_ * sizeof(unsigned int));
	}
	pool_pos_vsize_ = that.pool_pos_vsize_;
	if (pool_pos_vsize_) {
		memcpy(pool_pos_, that.pool_pos_, pool_pos_vsize_ * sizeof(unsigned int));
	}
	// cache
	if (pool_data_vsize_) {
		VALIDATE(false, "command_pool::command_pool, pool_data_vsize_ isn't zero.");
		if (!cache_) {
			cache_ = (unsigned char*)malloc(POOL_ALLOC_CACHE_SIZE);
		}
		memcpy(cache_, that.cache_, pool_data_vsize_);
	}

	return *this;
}

void command_pool::read(unsigned char* mem)
{
	int pool_data_size, pool_data_gzip_size, pool_pos_size, pool_pos_vsize;
	int* ptr = (int*)mem;

	pool_data_size = *ptr;
	ptr = ptr + 1;
	pool_data_gzip_size = *ptr;
	ptr = ptr + 1;
	pool_pos_size = *ptr;
	ptr = ptr + 1;
	pool_pos_vsize = *ptr;
	ptr = ptr + 1;
	
	if (pool_data_size_ < pool_data_size) {
		free(pool_data_);
		pool_data_size_ = pool_data_size;
		pool_data_ = (uint8_t*)malloc(pool_data_size);
	}
	// pool_data_vsize_ = pool_data_vsize;
	pool_data_gzip_size_ = pool_data_gzip_size;
	if (pool_data_gzip_size_) {
		memcpy(pool_data_, (unsigned char*)ptr, pool_data_gzip_size_);
	}

	if (pool_pos_size_ < pool_pos_size) {
		free(pool_pos_);
		pool_pos_size_ = pool_pos_size;
		pool_pos_ = (unsigned int*)malloc(pool_pos_size_ * sizeof(unsigned int));
	}
	pool_pos_vsize_ = pool_pos_vsize;
	if (pool_pos_vsize_) {
		memcpy(pool_pos_, (unsigned char*)ptr + pool_data_gzip_size_, pool_pos_vsize_ * sizeof(unsigned int));
	}
}

void command_pool::clear()
{
	pool_data_vsize_ = 0;
	pool_pos_vsize_ = 0;

	pool_data_gzip_size_ = 0;
	current_gzip_seg_.min = current_gzip_seg_.max = -1;
}

void command_pool::set_use_gzip(bool val) 
{ 
	use_gzip_ = val; 
}

// #define command_addr(pool_pos_index)	((command_pool::command*)(pool_data_ + pool_pos_[pool_pos_index]))

void command_pool::load_for_queue(int pool_pos_index, gzip_segment_t& seg, int& pool_data_gzip_size, unsigned char* dest)
{
	int start;
	// ungzip necessary data to cache
	if (pool_pos_index < current_gzip_seg_.min) {
		// search back
		start = 0;
	} else if (seg.max != -1 && pool_pos_index > seg.max) {
		// search forword
		start = pool_data_gzip_size;
	} else {
		// search from 0 to max
		start = 0;
	}

	memcpy(&seg, pool_data_ + start, sizeof(gzip_segment_t));
	while (pool_pos_index < seg.min || pool_pos_index > seg.max) {
		start += sizeof(gzip_segment_t) + seg.len;
		memcpy(&seg, pool_data_ + start, sizeof(gzip_segment_t));
	}
	pool_data_gzip_size = start;
	gzip_codec(pool_data_ + pool_data_gzip_size + sizeof(gzip_segment_t), seg.len, false, dest);
}

command_pool::command* command_pool::command_addr(int pool_pos_index, bool queue)
{
	if (use_gzip_) {
		if (!cache_) {
			cache_ = (unsigned char*)malloc(POOL_ALLOC_CACHE_SIZE);
		}
		if (queue) {
			if (pool_pos_index < current_gzip_seg_.min || pool_pos_index > current_gzip_seg_.max) {
				load_for_queue(pool_pos_index, current_gzip_seg_, pool_data_gzip_size_, cache_);
			}
		} else {
			// ensure current_gzip_seg.min/max is valid.
			if (current_gzip_seg_.min == -1) {
				current_gzip_seg_.min = pool_pos_vsize_ - 1;
				current_gzip_seg_.max = INT_MAX;
			}
		}
		return (command_pool::command*)(cache_ + pool_pos_[pool_pos_index]);
	} else {
		return (command_pool::command*)(pool_data_ + pool_pos_[pool_pos_index]);
	}
}

command_pool::command* command_pool::command_addr2(int pool_pos_index, gzip_segment_t& seg, int& pool_data_gzip_size, unsigned char* cache)
{
	if (pool_pos_index < seg.min || pool_pos_index > seg.max) {
		load_for_queue(pool_pos_index, seg, pool_data_gzip_size, cache);
	}
	return (command_pool::command*)(cache + pool_pos_[pool_pos_index]);
}

// 返回下一个可以写的地址，同时已把这下个计入在了有效包中
command_pool::command* command_pool::add_command()
{
	if (!use_gzip_) {
		if (pool_data_size_ - pool_data_vsize_ < POOL_COMMAND_RESERVE_BYTES) {
			pool_data_size_ += POOL_ALLOC_DATA_SIZE;
			uint8_t* tmp = (uint8_t*)malloc(pool_data_size_);
			memcpy(tmp, pool_data_, pool_data_vsize_);
			free(pool_data_);
			pool_data_ = tmp;
		}
	}
	
	pool_pos_vsize_ ++;
	if (pool_pos_vsize_ > pool_pos_size_) {
		pool_pos_size_ += POOL_ALLOC_POS_SIZE;
		unsigned int* tmp = (unsigned int*)malloc(pool_pos_size_ * sizeof(unsigned int));
		memcpy(tmp, pool_pos_, (pool_pos_vsize_ - 1) * sizeof(unsigned int));
		free(pool_pos_);
		pool_pos_ = tmp;
	}

	pool_pos_[pool_pos_vsize_ - 1] = pool_data_vsize_;

	return command_addr(pool_pos_vsize_ - 1, false);
}

int command_pool::gzip_codec(unsigned char* data, int data_len, bool encode, unsigned char* to)
{
	int chunk_size = 8192;

	if (!data_len) {
		return 0;
	}

	int len = 0;
	int result_len = 0;
	try {
		std::stringstream gangplank;
		std::iostream ifile(gangplank.rdbuf());
		ifile.write((char*)data, data_len);

		boost::iostreams::filtering_stream<boost::iostreams::input> in;
		if (encode) {
			in.push(boost::iostreams::gzip_compressor());
		} else {
			in.push(boost::iostreams::gzip_decompressor());
		}
		in.push(ifile);

		if (encode) {
			uint8_t* dest = pool_data_ + pool_data_gzip_size_ + sizeof(gzip_segment_t);
			do {
				if (pool_data_size_ - pool_data_gzip_size_ - (int)sizeof(gzip_segment_t) - result_len - len < chunk_size) {
					pool_data_size_ += POOL_ALLOC_DATA_SIZE;
					uint8_t* tmp = (uint8_t*)malloc(pool_data_size_);
					// must copy necessary data. not forget this encoded!
					memcpy(tmp, pool_data_, pool_data_gzip_size_ + sizeof(gzip_segment_t) + result_len + len);
					free(pool_data_);
					// update dest
					dest = tmp + (dest - pool_data_);
					pool_data_ = tmp;
				}
				if (len > 0) {
					result_len += len;
					// pool_data_gzip_size_ += len;
					dest = dest + len;
					if (len < chunk_size) {
						break;
					}
				}
			} while (len = in.read((char*)dest, chunk_size).gcount());

		} else {
			uint8_t* dest = to;
			while (len = in.read((char*)dest, chunk_size).gcount()) {
				result_len += len;
				if (len != chunk_size) {
					break;
				}
				dest = dest + len;
			}
		}

	} catch(io_exception& e) {
		VALIDATE(false, std::string("IO error: ") + e.what());
	}
	return result_len;
}

static lg::log_domain log_replay("replay");
#define DBG_REPLAY LOG_STREAM(debug, log_replay)
#define LOG_REPLAY LOG_STREAM(info, log_replay)
#define WRN_REPLAY LOG_STREAM(warn, log_replay)
#define ERR_REPLAY LOG_STREAM(err, log_replay)

static lg::log_domain log_random("random");
#define DBG_RND LOG_STREAM(debug, log_random)
#define LOG_RND LOG_STREAM(info, log_random)
#define WRN_RND LOG_STREAM(warn, log_random)
#define ERR_RND LOG_STREAM(err, log_random)

std::string replay::last_replay_error;

//functions to verify that the unit structure on both machines is identical

static void verify(const unit_map& units, const config& cfg) {
	std::stringstream errbuf;
	LOG_REPLAY << "verifying unit structure...\n";

	const size_t nunits = lexical_cast_default<size_t>(cfg["num_units"]);
	if(nunits != units.size()) {
		errbuf << "SYNC VERIFICATION FAILED: number of units from data source differ: "
			   << nunits << " according to data source. " << units.size() << " locally\n";

		std::set<map_location> locs;
		foreach (const config &u, cfg.child_range("unit"))
		{
			const map_location loc(u, resources::state_of_game);
			locs.insert(loc);

			if(units.count(loc) == 0) {
				errbuf << "data source says there is a unit at "
					   << loc << " but none found locally\n";
			}
		}

		for(unit_map::const_iterator j = units.begin(); j != units.end(); ++j) {
			if(locs.count(j->get_location()) == 0) {
				errbuf << "local unit at " << j->get_location()
					   << " but none in data source\n";
			}
		}
		replay::process_error(errbuf.str());
		errbuf.clear();
	}

	foreach (const config &un, cfg.child_range("unit"))
	{
		const map_location loc(un, resources::state_of_game);
		const unit_map::const_iterator u = units.find(loc);
		if(u == units.end()) {
			errbuf << "SYNC VERIFICATION FAILED: data source says there is a '"
				   << un["type"] << "' (side " << un["side"] << ") at "
				   << loc << " but there is no local record of it\n";
			replay::process_error(errbuf.str());
			errbuf.clear();
		}

		config cfg;
		u->write(cfg);

		bool is_ok = true;
		static const std::string fields[] = {"type","hitpoints","experience","side",""};
		for(const std::string* str = fields; str->empty() == false; ++str) {
			if (cfg[*str] != un[*str]) {
				errbuf << "ERROR IN FIELD '" << *str << "' for unit at "
					   << loc << " data source: '" << un[*str]
					   << "' local: '" << cfg[*str] << "'\n";
				is_ok = false;
			}
		}

		if(!is_ok) {
			errbuf << "(SYNC VERIFICATION FAILED)\n";
			replay::process_error(errbuf.str());
			errbuf.clear();
		}
	}

	LOG_REPLAY << "verification passed\n";
}

// FIXME: this one now has to be assigned with set_random_generator
// from play_level or similar.  We should surely hunt direct
// references to it from this very file and move it out of here.
replay recorder;

replay::replay() 
	: cfg_()
	, build_cfg_()
	, pos_(0)
	, current_(NULL)
	, skip_(false)
	, message_locations()
	, expected_advancements_()
{}

replay::replay(const command_pool& pool) 
	: command_pool(pool)
	, cfg_()
	, build_cfg_()
	, pos_(0)
	, current_(NULL)
	, skip_(false)
	, message_locations()
	, expected_advancements_()
{
}


// [write] network receiver. push cfg to local pool
void replay::append(const config& cfg)
{
	config::const_child_itors cmds = cfg.child_range("command");
	// foreach (const config& t, cmds) {
	foreach (config t, cmds) {
		if (!t.empty()) {
			// <attention 2>
			if (t.child("speak")) {
				t["type"] = str_cast(command_pool::SPEAK);
			}

			command_pool::command* cmd = command_pool::add_command();
			config_2_command(t, cmd);
		}
	}
}

void replay::process_error(const std::string& msg)
{
	ERR_REPLAY << msg;

	resources::controller->process_oos(msg); // might throw end_level_exception(QUIT)
}

void replay::throw_error(const std::string& msg)
{
	ERR_REPLAY << msg;
	last_replay_error = msg;
	if (!game_config::ignore_replay_errors) throw replay::error(msg);
}

void replay::set_skip(bool skip)
{
	skip_ = skip;
}

bool replay::is_skipping() const
{
	return skip_;
}

void replay::add_start()
{
	config* const cmd = add_command(true);
	(*cmd)["type"] = command_pool::START;
	cmd->add_child("start");
}

void replay::add_recruit(const std::string& type, const map_location& loc, std::vector<const hero*>& troop_heros, int cost, bool human)
{
	config* const cmd = add_command();

	(*cmd)["type"] = command_pool::RECRUIT;

	config val;

	val["type"] = type;
	std::stringstream str;
	// heros_army
	str << troop_heros.front()->number_;
	for (std::vector<const hero*>::const_iterator itor = troop_heros.begin() + 1; itor != troop_heros.end(); ++ itor) {
		str << "," << (*itor)->number_;
	}
	val["heros"] = str.str();
	val["cost"] = cost;
	val["human"] = human? 1: 0;

	loc.write(val);

	cmd->add_child("recruit",val);
}

void replay::add_move_heros(const map_location& src, const map_location& dst, std::set<size_t>& heros)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::MOVE_HEROS);

	config val;

	std::stringstream str;
	// move heros
	std::set<size_t>::const_iterator itor = heros.begin();
	str << *itor;
	for (++ itor; itor != heros.end(); ++ itor) {
		str << "," << *itor;
	}
	val["heros"] = str.str();

	str.str("");
	str << src;
	val["src"] = str.str();

	str.str("");
	str << dst;
	val["dst"] = str.str();

	cmd->add_child("move_heros",val);
}

void replay::add_interior(int type, const std::vector<hero*>& officials)
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::INTERIOR;

	config val;

	std::stringstream str;
	// official heros
	if (!officials.empty()) {
		std::vector<hero*>::const_iterator itor = officials.begin();
		int number = (*itor)->number_;
		str << number;
		for (++ itor; itor != officials.end(); ++ itor) {
			number = (*itor)->number_;
			str << "," << number;
		}
	}
	val["officials"] = str.str();

	val["type"] = type;

	cmd->add_child("interior", val);
}

void replay::add_belong_to(const unit* u, const artifical* to, bool loyalty)
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::BELONG_TO;

	config val;

	val["layer"] = u->base()? unit_map::BASE: unit_map::OVERLAY;
	val["loyalty"] = loyalty? 1: 0;
	std::stringstream str;
	str << u->get_location();
	val["src"] = str.str();

	str.str("");
	str << to->get_location();
	val["dst"] = str.str();

	cmd->add_child("belong_to", val);
}

/*
content of build packet
--------------------------
[command]
	[build]
		artifical="25,4"
		builder="26,3"
		type="market"
	[/build]
	[random]
		value="17398"
	[/random]
	[random]
		value="13234"
	[/random]
[/command]
-------------------------
*/
void replay::add_build_begin(const std::string& type)
{
	build_cfg_.clear();
	set_random(&build_cfg_);

	config val;
	val["type"] = type;
	build_cfg_.add_child("build", val);
}

void replay::add_build_cancel()
{
	set_random(current_);
}

void replay::add_build(const unit_type* type, const map_location& city_loc, const map_location& builder_loc, const map_location& art_loc, int cost)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::BUILD);

	config build;

	build["type"] = type->id();

	std::stringstream str;
	str << city_loc;
	build["city"] = str.str();

	str.str("");
	str << builder_loc;
	build["builder"] = str.str();

	str.str("");
	str << art_loc;
	build["artifical"] = str.str();

	build["cost"] = str_cast(cost);

	cmd->add_child("build", build);
}

void replay::add_expedite(int unit_index, const std::vector<map_location>& steps)
{
	if (steps.empty()) { // no move, nothing to record
		return;
	}

	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::EXPEDITE);

	config expedite;

	expedite["value"] = str_cast(unit_index);

	write_locations(steps, expedite);

	cmd->add_child("expedite", expedite);
}

void replay::add_disband(int value, const map_location& loc)
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::DISBAND;

	config val;

	val["value"] = value;

	loc.write(val);

	cmd->add_child("disband", val);
}

void replay::add_armory(const map_location& loc, const std::vector<size_t>& diff)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::ARMORY);

	config val;

	std::stringstream str;
	size_t index;
	unit* u;
	std::vector<size_t> captains;

	// rearmed reside troops
	artifical* city = resources::units->city_from_loc(loc);
	std::vector<unit*>& reside_troops = city->reside_troops();

	std::vector<size_t>::const_iterator itor = diff.begin();
	index = *itor;
	u = reside_troops[index];
	captains.push_back(u->master().number_);
	if (u->second().valid()) captains.push_back(u->second().number_);
	if (u->third().valid()) captains.push_back(u->third().number_);
	str << index << "," << captains.size() << "," << captains[0];
	if (captains.size() > 1) {
		str << "," << captains[1];
	}
	if (captains.size() > 2) {
		str << "," << captains[2];
	}
	for (++ itor; itor != diff.end(); ++ itor) {
		index = *itor;
		u = reside_troops[index];
		captains.clear();
		captains.push_back(u->master().number_);
		if (u->second().valid()) captains.push_back(u->second().number_);
		if (u->third().valid()) captains.push_back(u->third().number_);
		str << "," << index << "," << captains.size() << "," << captains[0];
		if (captains.size() > 1) {
			str << "," << captains[1];
		}
		if (captains.size() > 2) {
			str << "," << captains[2];
		}
	}
	val["diff"] = str.str();

	str.str("");
	str << loc;
	val["city"] = str.str();

	cmd->add_child("armory", val);
}

void replay::add_diplomatism(const std::string& type, int first_side, int second_side, bool alignment, int emissary, int target_side, int strategy)
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::DIPLOMATISM;

	config val;

	val["type"] = type;
	val["alignment"] = alignment? 1: 0;
	val["hero"] = emissary;
	val["first"] = first_side;
	val["second"] = second_side;
	val["target"] = target_side;
	val["strategy"] = strategy;

	cmd->add_child("diplomatism", val);
}

void replay::add_final_battle(artifical* human, artifical* ai)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::FINAL_BATTLE);

	config val;

	val["human"] = human->cityno();
	val["ai"] = ai? ai->cityno(): 0;

	cmd->add_child("final_battle", val);
}

void replay::add_card(int side, size_t number, bool dialog)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::ADD_CARD);

	config val;

	val["side"] = side;
	val["number"] = (int)number;
	val["dialog"] = dialog? 1: 0;

	cmd->add_child("add_card", val);
}

void replay::erase_card(int side, int index, const map_location& loc, std::vector<std::pair<int, unit*> >& touched, bool dialog)
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::ERASE_CARD;

	config val;

	std::stringstream str;
	// touched heros
	if (!touched.empty()) {
		std::vector<std::pair<int, unit*> >::const_iterator it = touched.begin();
		str << it->first << "," << it->second->get_location().x << "," << it->second->get_location().y;
		for (++ it; it != touched.end(); ++ it) {
			str << "," << it->first << "," << it->second->get_location().x << "," << it->second->get_location().y;
		}
	}
	val["touched"] = str.str();

	val["side"] = side;
	val["index"] = index;
	val["dialog"] = dialog? 1: 0;
	loc.write(val);

	cmd->add_child("erase_card", val);
}

void replay::add_hero_field(int number, int side_feature)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::HERO_FIELD);

	config val;

	val["number"] = number;
	val["side_feature"] = side_feature;

	cmd->add_child("hero_field", val);
}

void replay::add_countdown_update(int value, int team)
{
	config* const cmd = add_command();
	config val;

	val["value"] = lexical_cast_default<std::string>(value);
	val["team"] = lexical_cast_default<std::string>(team);

	cmd->add_child("countdown_update",val);
}


void replay::add_movement(const std::vector<map_location>& steps)
{
	if(steps.empty()) { // no move, nothing to record
		return;
	}

	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::MOVEMENT);

	config move;
	write_locations(steps, move);

	cmd->add_child("move",move);
}

void replay::add_attack(const unit& a, const unit& b,
	int att_weapon, int def_weapon, const std::string& attacker_type_id,
	const std::string& defender_type_id, int attacker_lvl,
	int defender_lvl, const size_t turn, const time_of_day t)
{
	const map_location& a_loc = a.get_location();
	const map_location& b_loc = b.get_location();

	add_pos("attack", a_loc, b_loc);
	config &cfg = current_->child("attack");
	(*current_)["type"] = command_pool::ATTACK;

	cfg["weapon"] = str_cast(att_weapon);
	cfg["defender_weapon"] = str_cast(def_weapon);
	cfg["layer"] = a.base()? unit_map::BASE: unit_map::OVERLAY;
	cfg["defender_layer"] = b.base()? unit_map::BASE: unit_map::OVERLAY;
	cfg["attacker_lvl"] = str_cast(attacker_lvl);
	cfg["defender_lvl"] = str_cast(defender_lvl);
	cfg["turn"] = str_cast(turn);
	// cfg["tod"] = t.id;
}

void replay::add_duel(const hero& left, const hero& right, int percentage)
{

	config& duel = current_->child("attack").add_child("duel");
	duel["left"] = left.number_;
	duel["right"] = right.number_;
	duel["percentage"] = percentage;
}

void replay::add_seed(const char* child_name, rand_rng::seed_t seed)
{
	LOG_REPLAY << "Setting seed for child type " << child_name << ": " << seed << "\n";
	current_->child(child_name)["seed"] = lexical_cast<std::string>(seed);
}

void replay::add_pos(const std::string& type,
                     const map_location& a, const map_location& b)
{
	config* const cmd = add_command();

	config move, src, dst;
	a.write(src);
	b.write(dst);

	move.add_child("source",src);
	move.add_child("destination",dst);
	cmd->add_child(type,move);
}

void replay::add_value(const std::string& type, int value)
{
	config* const cmd = add_command();

	config val;

	char buf[100];
	snprintf(buf,sizeof(buf),"%d",value);
	val["value"] = buf;

	cmd->add_child(type,val);
}

void replay::choose_option(int index)
{
	add_value("choose",index);
	(*current_)["type"] = str_cast(command_pool::CHOOSE);
}

void replay::text_input(std::string input)
{
	config* const cmd = add_command();

	config val;
	val["text"] = input;

	cmd->add_child("input",val);
}

void replay::set_random_value(const std::string& choice)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::RANDOM_NUMBER);
	config val;
	val["value"] = choice;
	cmd->add_child("random_number",val);
}

void replay::add_label(const terrain_label* label)
{
	assert(label);
	config* const cmd = add_command(false);

	config val;

	label->write(val);

	cmd->add_child("label",val);
}

void replay::user_input(const std::string &name, const config &input)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::INPUT);

	cmd->add_child(name, input);
}

void replay::clear_labels(const std::string& team_name)
{
	config* const cmd = add_command();
	(*cmd)["type"] = str_cast(command_pool::CLEAR_LABELS);

	config val;
	val["team_name"] = team_name;
	cmd->add_child("clear_labels",val);
}

void replay::add_rename(const std::string& name, const map_location& loc)
{
	config* const cmd = add_command(false);
	(*cmd)["async"] = "yes"; // Not undoable, but depends on moves/recruits that are
	config val;
	loc.write(val);
	val["name"] = name;
	cmd->add_child("rename", val);
}

void replay::init_side()
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::INIT_SIDE;
	cmd->add_child("init_side");
}

void replay::end_turn()
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::END_TURN;
	cmd->add_child("end_turn");
}

void replay::init_ai()
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::INIT_AI;
	cmd->add_child("init_ai");
}

void replay::add_event(int type, const map_location& loc)
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::EVENT;

	config val;
	val["type"] = type;
	loc.write(val);
	cmd->add_child("event", val);
}

void replay::add_assemble_treasure(const std::map<int, int>& diff)
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::ASSEMBLE_TREASURE;

	config val;
	std::stringstream str;
	// diff
	std::map<int, int>::const_iterator it = diff.begin();
	str << it->first << "," << it->second;
	for (++ it; it != diff.end(); ++ it) {
		str << "," << it->first << "," << it->second;
	}
	val["diff"] = str.str();

	cmd->add_child("assemble_treasure", val);
}

void replay::add_rpg_exchange(const std::set<size_t>& checked_human, size_t checked_ai)
{
	config* const cmd = add_command();
	(*cmd)["type"] = command_pool::RPG_EXCHANGE;

	config val;

	std::stringstream str;
	std::set<size_t>::const_iterator itor = checked_human.begin();
	int number = *itor;
	str << number;
	for (++ itor; itor != checked_human.end(); ++ itor) {
		number = *itor;
		str << "," << number;
	}
	val["human"] = str.str();
	val["ai"] = (int)checked_ai;

	cmd->add_child("rpg_exchange", val);
}

void replay::add_log_data(const std::string &key, const std::string &var)
{

}

void replay::add_log_data(const std::string &category, const std::string &key, const std::string &var)
{
/*
	config& ulog = cfg_.child_or_add("upload_log");
	config& cat = ulog.child_or_add(category);
	cat[key] = var;
*/
}

void replay::add_log_data(const std::string &category, const std::string &key, const config &c)
{
/*	
	config& ulog = cfg_.child_or_add("upload_log");
	config& cat = ulog.child_or_add(category);
	cat.add_child(key,c);
*/
}

void replay::add_expected_advancement(const map_location& loc)
{
	expected_advancements_.push_back(loc);
}

const std::deque<map_location>& replay::expected_advancements() const
{
	return expected_advancements_;
}

void replay::pop_expected_advancement()
{
	expected_advancements_.pop_front();
}

void replay::add_advancement(const map_location& loc)
{
	config* const cmd = add_command();

	(*cmd)["type"] = str_cast(command_pool::ADVANCEMENT_UNIT);

	config val;
	loc.write(val);
	cmd->add_child("advance_unit",val);
}

void replay::add_chat_message_location()
{
	message_locations.push_back(pos_-1);
}

void replay::speak(const config& cfg)
{
	config* const cmd = add_command();
	if(cmd != NULL) {
		cmd->add_child("speak",cfg);
		(*cmd)["type"] = str_cast(command_pool::SPEAK);
		add_chat_message_location();
	}
}

void replay::add_chat_log_entry(const config &cfg, std::ostream &str) const
{
	if (!cfg) return;

	if (!preferences::parse_should_show_lobby_join(cfg["id"], cfg["message"])) return;
	if (preferences::is_ignored(cfg["id"])) return;
	const std::string& team_name = cfg["team_name"];
	if(team_name == "") {
		str << "<" << cfg["id"] << "> ";
	} else {
		str << "*" << cfg["id"] << "* ";
	}
	str << cfg["message"] << "\n";
}

// [write]
void replay::remove_command(int index)
{
	if (!pos_ || index != -1) {
		// only allow remove last command
		posix_print_mb("replay::remove_command, only allow remove last command!");
		return;
	}
	cfg_.clear();
	// notice: this pos_ is -1 from current used.
	pos_ = pool_pos_vsize_;

	index = pos_;

	std::vector<int>::reverse_iterator loc_it;
	for (loc_it = message_locations.rbegin(); loc_it != message_locations.rend() && index < *loc_it;++loc_it)
	{
		--(*loc_it);
	}
}

// cached message log
std::stringstream message_log;


std::string replay::build_chat_log()
{
/*
	config cmd;
	std::vector<int>::iterator loc_it;
	int last_location = 0;
	for (loc_it = message_locations.begin(); loc_it != message_locations.end(); ++loc_it) {
		last_location = *loc_it;
		if (last_location != pos_ - 1) {
			cmd.clear();
			command_2_config(command_addr(last_location), cmd); 
		} else {
			cmd = cfg_;
		}
		const config &speak = cmd.child("speak");
		add_chat_log_entry(speak, message_log);

	}
	message_locations.clear();
*/
	return message_log.str();
}

// [read] network
config replay::get_data_range(int cmd_start, int cmd_end, DATA_TYPE data_type)
{
	config res;
	if ((data_type != ALL_DATA) || (cmd_start >= cmd_end)) {
		return res;
	}
	int cmd_end_from_pool = (cmd_end > pool_pos_vsize_)? cmd_end - 1: pool_pos_vsize_;
	while (cmd_start < cmd_end_from_pool) {
		if (!(command_addr(cmd_start)->flags & 1)) {
			config& cmd_cfg = res.add_child("command");
			command_2_config(command_addr(cmd_start), cmd_cfg);
			cmd_cfg["type"] = str_cast(command_addr(cmd_start)->type);
		}
		++ cmd_start;
	}
	if (cmd_end > pool_pos_vsize_) {
		if (cfg_["sent"] != "yes") {
			res.add_child("command", cfg_);
		}
	}

	return res;
}

void replay::undo()
{
	if (const config &child = cfg_.child("move")) {
		// A unit's move is being undone.
		// Repair unsynced cmds whose locations depend on that unit's location.
		const std::vector<map_location> steps =
				parse_location_range(resources::game_map, child["x"], child["y"]);

		if (steps.empty()) {
			replay::process_error("undo(), trying to undo a move using an empty path");
			return; // nothing to do, I suppose.
		}
	}
	int type = cfg_["type"].to_int();
	if (cfg_.empty()) {
		replay::process_error("undo(), unexpected cfg_, cfg_ is empty");
	} else if ((type != command_pool::EXPEDITE) && (type != command_pool::MOVEMENT)) {
		replay::process_error("undo(), unexpected type: " + cfg_["type"].str());
	}

	remove_command(-1);
	current_ = NULL;
	set_random(NULL);
}

// Only for network sender. Other scenario don't use it, use pool_pos_vsize_ directly.
int replay::ncommands()
{
	return pool_pos_vsize_ + (cfg_.empty()? 0: 1);
}

config* replay::add_command(bool update_random_context)
{
	// 把一条命令加入恢复池
	if (!cfg_.empty()) {
		command_pool::command* cmd = command_pool::add_command();
		config_2_command(cfg_, cmd);
		if (cfg_["sent"] == "yes") {
			cmd->flags = 1;
		}
	}

	pos_ = pool_pos_vsize_ + 1;
	cfg_.clear();
	current_ = &cfg_;
	if (update_random_context)
		set_random(current_);

	return current_;
}

/*
START = 1, INIT_SIDE, END_TURN, RECRUIT, EXPEDITE, DISBAND, MOVE_HEROS, BUILD, 
		COUNTDOWN_UPDATE, MOVEMENT, ATTACK, SEED, LABLE, RENAME, EVENT, 
		UNIT_CHECKSUM, CHECKSUM_CHECK, EXPECTED_ADVANCEMENT
*/
void replay::config_2_command(const config& cfg, command_pool::command* cmd)
{
	int type = lexical_cast_default<int>(cfg["type"], 0);
	std::vector<map_location> steps;
	size_t i, size;

	if (type == command_pool::START) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			const config& random = cfg.child("random", i);
			*ptr = random["value"].to_int();
			ptr = ptr + 1;
		}
		const config& child = cfg.child("start");
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::INIT_SIDE) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			const config& random = cfg.child("random", i);
			*ptr = random["value"].to_int();
			ptr = ptr + 1;

			const config& results = random.child("results", 0);
			if (!results) {
				*ptr = INT_MAX;
				ptr = ptr + 1;
				continue;
			}
			// [results].choose
			*ptr = lexical_cast_default<int>(results["choose"], 0);
			ptr = ptr + 1;
		}
		const config& child = cfg.child("init_side");
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::END_TURN) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("end_turn");
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::INIT_AI) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("init_ai");
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::RECRUIT) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("recruit");
		// heros
		const std::vector<std::string> heros = utils::split(child["heros"]);
		size = *ptr = heros.size();
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = lexical_cast_default<int>(heros[i]);
			ptr = ptr + 1;
		}
		*ptr = lexical_cast_default<int>(child["human"]);
		ptr = ptr + 1;
		*ptr = lexical_cast_default<int>(child["cost"]);
		ptr = ptr + 1;
		*ptr = lexical_cast_default<int>(child["x"]) - 1;
		ptr = ptr + 1; 
		*ptr = lexical_cast_default<int>(child["y"]) - 1;
		ptr = ptr + 1;
		// type="market"
		size = *ptr = child["type"].str().size();
		ptr = ptr + 1;
		memcpy(ptr, child["type"].str().c_str(), size);
		*((char*)ptr + size) = '\0';
		ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::EXPEDITE) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("expedite");
		// value
		*ptr = lexical_cast_default<int>(child["value"]);
		ptr = ptr + 1;
		// x=/y=
		const std::string x = child.get("x")->str();
		const std::string y = child.get("y")->str();
		steps = parse_location_range(resources::game_map, x, y);
		*ptr = steps.size();
		ptr = ptr + 1;
		for (std::vector<map_location>::iterator itor = steps.begin(); itor != steps.end(); ++ itor) {
			*ptr = itor->x;
			ptr = ptr + 1; 
			*ptr = itor->y;
			ptr = ptr + 1;
		}
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::MOVEMENT) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("move");
		// x=/y=
		const std::string x = child.get("x")->str();
		const std::string y = child.get("y")->str();
		steps = parse_location_range(resources::game_map, x, y);
		*ptr = steps.size();
		ptr = ptr + 1;
		for (std::vector<map_location>::iterator itor = steps.begin(); itor != steps.end(); ++ itor) {
			*ptr = itor->x;
			ptr = ptr + 1; 
			*ptr = itor->y;
			ptr = ptr + 1;
		}
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::ATTACK) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			const config& random = cfg.child("random", i);
			*ptr = random["value"].to_int();
			ptr = ptr + 1; 
			const config& results = random.child("results", 0);
			if (!results) {
				*ptr = INT_MAX;
				ptr = ptr + 1;
				continue;
			}
			// [results].chance
			*ptr = lexical_cast_default<int>(results["chance"], 0);
			ptr = ptr + 1;
			// [results].damage
			*ptr = lexical_cast_default<int>(results["damage"], 0);
			ptr = ptr + 1;
			// [results].dies
			*ptr = (results["dies"] == "yes")? 1: 0;
			ptr = ptr + 1;
			// [results].hits
			*ptr = (results["hits"] == "yes")? 1: 0;
			ptr = ptr + 1;
		}
		const config& child = cfg.child("attack");
		if (const config& duel = child.child("duel")) {
			*ptr = duel["left"].to_int();
			ptr = ptr + 1; 
			*ptr = duel["percentage"].to_int();
			ptr = ptr + 1; 
			*ptr = duel["right"].to_int();
			ptr = ptr + 1;
		} else {
			// indicate no [duel] block.
			*ptr = INT_MAX;
			ptr = ptr + 1; 
		}
		// attacker_lvl
		*ptr = lexical_cast_default<int>(child["attacker_lvl"]);
		ptr = ptr + 1;
		// defender_layer
		*ptr = lexical_cast_default<int>(child["defender_layer"]);
		ptr = ptr + 1;
		// defender_lvl
		*ptr = lexical_cast_default<int>(child["defender_lvl"]);
		ptr = ptr + 1; 
		// defender_weapon
		*ptr = lexical_cast_default<int>(child["defender_weapon"]);
		ptr = ptr + 1;
		// layer
		*ptr = lexical_cast_default<int>(child["layer"]);
		ptr = ptr + 1;
		// seed
		*ptr = lexical_cast_default<int>(child["seed"]);
		ptr = ptr + 1;
		// turn
		*ptr = lexical_cast_default<int>(child["turn"]);
		ptr = ptr + 1;
		// weapon
		*ptr = lexical_cast_default<int>(child["weapon"]);
		ptr = ptr + 1;
		// [source].x/y
		const config& source = child.child("source", 0);
		*ptr = lexical_cast_default<int>(source["x"]) - 1;
		ptr = ptr + 1; 
		*ptr = lexical_cast_default<int>(source["y"]) - 1;
		ptr = ptr + 1;
		// [destination].x/y
		const config& destination = child.child("destination", 0);
		*ptr = lexical_cast_default<int>(destination["x"]) - 1;
		ptr = ptr + 1; 
		*ptr = lexical_cast_default<int>(destination["y"]) - 1;
		ptr = ptr + 1;
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::DISBAND) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("disband");
		// value
		*ptr = lexical_cast_default<int>(child["value"]);
		ptr = ptr + 1;
		// x/y
		*ptr = lexical_cast_default<int>(child["x"]) - 1;
		ptr = ptr + 1; 
		*ptr = lexical_cast_default<int>(child["y"]) - 1;
		ptr = ptr + 1;
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::EVENT) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("event");
		// type
		*ptr = lexical_cast_default<int>(child["type"]);
		ptr = ptr + 1;
		// x/y
		*ptr = lexical_cast_default<int>(child["x"]) - 1;
		ptr = ptr + 1; 
		*ptr = lexical_cast_default<int>(child["y"]) - 1;
		ptr = ptr + 1;
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::ASSEMBLE_TREASURE) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("assemble_treasure");
		// diff="0,1,2,3,4,5"
		const std::vector<std::string> diff = utils::split(child["diff"]);
		size = *ptr = diff.size();
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			*ptr = lexical_cast_default<int>(diff[i]);
			ptr = ptr + 1;
		}
		// other field-vars
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::RPG_EXCHANGE) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("rpg_exchange");
		// human
		const std::vector<std::string> heros = utils::split(child["human"]);
		size = *ptr = heros.size();
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = lexical_cast_default<int>(heros[i]);
			ptr = ptr + 1;
		}
		*ptr = lexical_cast_default<int>(child["ai"]);
		ptr = ptr + 1;
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::BUILD) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("build");
		// city="19,2"
		std::vector<std::string> coor = utils::split(child["city"]);
		for (i = 0; i < 2; i ++) {
			*ptr = lexical_cast_default<int>(coor[i]);
			ptr = ptr + 1;
		}
		// artifical="46,5"
		coor = utils::split(child["artifical"]);
		for (i = 0; i < 2; i ++) {
			*ptr = lexical_cast_default<int>(coor[i]);
			ptr = ptr + 1;
		}
		// builder="45,6"
		coor = utils::split(child["builder"]);
		for (i = 0; i < 2; i ++) {
			*ptr = lexical_cast_default<int>(coor[i]);
			ptr = ptr + 1;
		}
		*ptr = lexical_cast_default<int>(child["cost"]);
		ptr = ptr + 1;
		// type="market"
		size = *ptr = child["type"].str().size();
		ptr = ptr + 1;
		memcpy(ptr, child["type"].str().c_str(), size);
		*((char*)ptr + size) = '\0';
		ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::ARMORY) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("armory");
		// city="47,2"
		std::vector<std::string> coor = utils::split(child["city"]);
		for (i = 0; i < 2; i ++) {
			*ptr = lexical_cast_default<int>(coor[i]);
			ptr = ptr + 1;
		}
		// diff="0,1,2,3,4"
		const std::vector<std::string> diff = utils::split(child["diff"]);
		size = *ptr = diff.size();
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = lexical_cast_default<int>(diff[i]);
			ptr = ptr + 1;
		}
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::MOVE_HEROS) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("move_heros");
		// dst="47,2"
		std::vector<std::string> coor = utils::split(child["dst"]);
		for (i = 0; i < 2; i ++) {
			*ptr = lexical_cast_default<int>(coor[i]);
			ptr = ptr + 1;
		}
		// heros="0,1,2,3,4"
		const std::vector<std::string> heros = utils::split(child["heros"]);
		size = *ptr = heros.size();
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = lexical_cast_default<int>(heros[i]);
			ptr = ptr + 1;
		}
		// src="58,16"
		coor = utils::split(child["src"]);
		for (i = 0; i < 2; i ++) {
			*ptr = lexical_cast_default<int>(coor[i]);
			ptr = ptr + 1;
		}		
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::INTERIOR) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("interior");
		// officials="0,1,2"
		const std::vector<std::string> heros = utils::split(child["officials"]);
		size = *ptr = heros.size();
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = lexical_cast_default<int>(heros[i]);
			ptr = ptr + 1;
		}
		// type="0"
		*ptr = lexical_cast_default<int>(child["type"]);
		ptr = ptr + 1;
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::BELONG_TO) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("belong_to");
		// dst="47,2"
		std::vector<std::string> coor = utils::split(child["dst"]);
		for (i = 0; i < 2; i ++) {
			*ptr = lexical_cast_default<int>(coor[i]);
			ptr = ptr + 1;
		}
		// layer="0"
		*ptr = lexical_cast_default<int>(child["layer"]);
		ptr = ptr + 1;
		// loyalty="1"
		*ptr = lexical_cast_default<int>(child["loyalty"]);
		ptr = ptr + 1;
		// src="58,16"
		coor = utils::split(child["src"]);
		for (i = 0; i < 2; i ++) {
			*ptr = lexical_cast_default<int>(coor[i]);
			ptr = ptr + 1;
		}		
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::RANDOM_NUMBER) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("random_number");
		// value="gxx word: 3"
		size = *ptr = child["value"].str().size();
		ptr = ptr + 1;
		memcpy(ptr, child["value"].str().c_str(), size);
		*((char*)ptr + size) = '\0';
		ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::CHOOSE) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("choose");
		// value
		*ptr = lexical_cast_default<int>(child["value"]);
		ptr = ptr + 1;
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::SPEAK) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("speak");
		// side = 2
		*ptr = lexical_cast_default<int>(child["side"], 0);
		ptr = ptr + 1;
		// team_name
		size = *ptr = child["team_name"].str().size();
		ptr = ptr + 1;
		if (size) {
			memcpy(ptr, child["team_name"].str().c_str(), size);
			*((char*)ptr + size) = '\0';
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		// id="server"
		size = *ptr = child["id"].str().size();
		ptr = ptr + 1;
		if (size) {
			memcpy(ptr, child["id"].str().c_str(), size);
			*((char*)ptr + size) = '\0';
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		// message="ancientcc has left the game."
		size = *ptr = child["message"].str().size();
		ptr = ptr + 1;
		if (size) {
			memcpy(ptr, child["message"].str().c_str(), size);
			*((char*)ptr + size) = '\0';
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::ADVANCEMENT_UNIT) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("advance_unit");
		*ptr = lexical_cast_default<int>(child["x"]) - 1;
		ptr = ptr + 1; 
		*ptr = lexical_cast_default<int>(child["y"]) - 1;
		ptr = ptr + 1;
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::CLEAR_LABELS) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("clear_labels");
		// team_name
		size = *ptr = child["team_name"].str().size();
		ptr = ptr + 1;
		if (size) {
			memcpy(ptr, child["team_name"].str().c_str(), size);
			*((char*)ptr + size) = '\0';
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::DIPLOMATISM) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("diplomatism");
		// type="ally"
		size = *ptr = child["type"].str().size();
		ptr = ptr + 1;
		memcpy(ptr, child["type"].str().c_str(), size);
		*((char*)ptr + size) = '\0';
		ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		// alignment = 1
		*ptr = lexical_cast_default<int>(child["alignment"]);
		ptr = ptr + 1;
		// first=1
		*ptr = lexical_cast_default<int>(child["first"]);
		ptr = ptr + 1;
		// hero=102
		*ptr = lexical_cast_default<int>(child["hero"]);
		ptr = ptr + 1;
		// second=2
		*ptr = lexical_cast_default<int>(child["second"]);
		ptr = ptr + 1;
		// strategy=0
		*ptr = lexical_cast_default<int>(child["strategy"]);
		ptr = ptr + 1;
		// target=2
		*ptr = lexical_cast_default<int>(child["target"]);
		ptr = ptr + 1;
		// other field-vars
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::FINAL_BATTLE) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("final_battle");
		// human=1
		*ptr = child["human"].to_int();
		ptr = ptr + 1;
		// ai=2
		*ptr = child["ai"].to_int();
		ptr = ptr + 1;
		// other field-vars
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::INPUT) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("input");
		// value=1
		*ptr = child["value"].to_int();
		ptr = ptr + 1;
		// other field-vars
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::ADD_CARD) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("add_card");
		// side=1
		*ptr = child["side"].to_int();
		ptr = ptr + 1;
		// number=2
		*ptr = child["number"].to_int();
		ptr = ptr + 1;
		// dialog=2
		*ptr = child["dialog"].to_int();
		ptr = ptr + 1;
		// other field-vars
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::ERASE_CARD) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("erase_card");
		// heros="0,1,2,3,4"
		const std::vector<std::string> heros = utils::split(child["touched"]);
		size = *ptr = heros.size();
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = lexical_cast_default<int>(heros[i]);
			ptr = ptr + 1;
		}
		// side=1
		*ptr = child["side"].to_int();
		ptr = ptr + 1;
		// index=2
		*ptr = child["index"].to_int();
		ptr = ptr + 1;
		// dialog=2
		*ptr = child["dialog"].to_int();
		ptr = ptr + 1;
		// x/y
		*ptr = child["x"].to_int() - 1;
		ptr = ptr + 1; 
		*ptr = child["y"].to_int() - 1;
		ptr = ptr + 1;
		// other field-vars
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else if (type == command_pool::HERO_FIELD) {
		cmd->type = (command_pool::TYPE)type;
		int* ptr = (int*)(cmd + 1);
		size = *ptr = cfg.child_count("random");
		ptr = ptr + 1; 
		for (i = 0; i < size; i ++) {
			*ptr = cfg.child("random", i)["value"].to_int();
			ptr = ptr + 1; 
		}
		const config& child = cfg.child("hero_field");
		// number=1
		*ptr = child["number"].to_int();
		ptr = ptr + 1;
		// side_feature=2
		*ptr = child["side_feature"].to_int();
		ptr = ptr + 1;
		// other field-vars
		cmd->flags = 0;
		pool_data_vsize_ += sizeof(command_pool::command) + (int)((uint8_t*)ptr - (uint8_t*)(cmd + 1));

	} else {
		std::stringstream str;
		str << "config_2_command, unknown type(str): " << cfg["type"].str() << "; exist subconfig";
		foreach (const config::any_child &value, cfg.all_children_range()) {
			str << " [" << value.key << "]";
		}
		replay::process_error(str.str());
	}
	
}

void replay::command_2_config(command_pool::command* cmd, config& cfg)
{
	std::stringstream str, xstr, ystr;
	size_t i, size;
	map_location loc;

	if (cmd->type == command_pool::START) {
		config& child = cfg.add_child("start");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}

	} else if (cmd->type == command_pool::INIT_SIDE) {
		config& child = cfg.add_child("init_side");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
			if (*ptr == INT_MAX) {
				ptr = ptr + 1;
				continue;
			}
			config& result_cfg = random_cfg.add_child("results");
			result_cfg["choose"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}

	} else if (cmd->type == command_pool::END_TURN) {
		config& child = cfg.add_child("end_turn");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}

	} else if (cmd->type == command_pool::INIT_AI) {
		config& child = cfg.add_child("init_ai");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}

	} else if (cmd->type == command_pool::RECRUIT) {
		config& child = cfg.add_child("recruit");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		size = *ptr;
		ptr = ptr + 1;
		// heros
		str << *ptr;
		ptr = ptr + 1;
		for (i = 1; i < size; i ++) {
			str << "," << *ptr;
			ptr = ptr + 1;
		}
		child["heros"] = str.str();
		child["human"] = lexical_cast<std::string>(*ptr);;
		ptr = ptr + 1;
		child["cost"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		loc.x = *ptr;
		ptr = ptr + 1;
		loc.y = *ptr;
		ptr = ptr + 1;
		loc.write(child);

		size = *ptr;
		ptr = ptr + 1;
		child["type"] = (char*)ptr;

	} else if (cmd->type == command_pool::EXPEDITE) {
		config& child = cfg.add_child("expedite");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// value
		child["value"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// x=/y=
		size = *ptr;
		ptr = ptr + 1;
		xstr << *ptr + 1;
		ptr = ptr + 1;
		ystr << *ptr + 1;
		ptr = ptr + 1;
		for (i = 1; i < size; i ++) {
			xstr << "," << *ptr + 1;
			ptr = ptr + 1;
			ystr << "," << *ptr + 1;
			ptr = ptr + 1;
		}
		child["x"] = xstr.str();
		child["y"] = ystr.str();

	} else if (cmd->type == command_pool::MOVEMENT) {
		config& child = cfg.add_child("move");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// x=/y=
		size = *ptr;
		ptr = ptr + 1;
		xstr << *ptr + 1;
		ptr = ptr + 1;
		ystr << *ptr + 1;
		ptr = ptr + 1;
		for (i = 1; i < size; i ++) {
			xstr << "," << *ptr + 1;
			ptr = ptr + 1;
			ystr << "," << *ptr + 1;
			ptr = ptr + 1;
		}
		child["x"] = xstr.str();
		child["y"] = ystr.str();

	} else if (cmd->type == command_pool::ATTACK) {
		config& child = cfg.add_child("attack");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
			if (*ptr == INT_MAX) {
				ptr = ptr + 1;
				continue;
			}
			config& result_cfg = random_cfg.add_child("results");
			result_cfg["chance"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
			result_cfg["damage"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
			result_cfg["dies"] = *ptr? "yes": "no";
			ptr = ptr + 1;
			result_cfg["hits"] = *ptr? "yes": "no";
			ptr = ptr + 1;
		}
		// [duel]
		if (*ptr != INT_MAX) {
			config& duel = child.add_child("duel");
			duel["left"] = *ptr;
			ptr = ptr + 1;
			duel["percentage"] = *ptr;
			ptr = ptr + 1;
			duel["right"] = *ptr;
			ptr = ptr + 1;
		} else {
			ptr = ptr + 1;
		}

		child["attacker_lvl"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		child["defender_layer"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		child["defender_lvl"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		child["defender_weapon"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		child["layer"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		child["seed"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		child["turn"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		child["weapon"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		config& source = child.add_child("source");
		loc.x = *ptr;
		ptr = ptr + 1;
		loc.y = *ptr;
		ptr = ptr + 1;
		loc.write(source);
		config& destination = child.add_child("destination");
		loc.x = *ptr;
		ptr = ptr + 1;
		loc.y = *ptr;
		ptr = ptr + 1;
		loc.write(destination);

	} else if (cmd->type == command_pool::DISBAND) {
		config& child = cfg.add_child("disband");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		child["value"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		loc.x = *ptr;
		ptr = ptr + 1;
		loc.y = *ptr;
		ptr = ptr + 1;
		loc.write(child);

	} else if (cmd->type == command_pool::EVENT) {
		config& child = cfg.add_child("event");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		child["type"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		loc.x = *ptr;
		ptr = ptr + 1;
		loc.y = *ptr;
		ptr = ptr + 1;
		loc.write(child);

	} else if (cmd->type == command_pool::ASSEMBLE_TREASURE) {
		config& child = cfg.add_child("assemble_treasure");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// diff
		str.str("");
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			if (i == 0) {
				str << *ptr;
			} else {
				str << "," << *ptr;
			}
			ptr = ptr + 1;
		}
		child["diff"] = str.str();

	} else if (cmd->type == command_pool::RPG_EXCHANGE) {
		config& child = cfg.add_child("rpg_exchange");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		size = *ptr;
		ptr = ptr + 1;
		// heros
		str << *ptr;
		ptr = ptr + 1;
		for (i = 1; i < size; i ++) {
			str << "," << *ptr;
			ptr = ptr + 1;
		}
		child["human"] = str.str();
		child["ai"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

	} else if (cmd->type == command_pool::BUILD) {
		config& child = cfg.add_child("build");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// city
		str << *ptr;
		ptr = ptr + 1;
		str << "," << *ptr;
		ptr = ptr + 1;
		child["city"] = str.str();
		// artifical
		str.str("");
		str << *ptr;
		ptr = ptr + 1;
		str << "," << *ptr;
		ptr = ptr + 1;
		child["artifical"] = str.str();
		// builder
		str.str("");
		str << *ptr;
		ptr = ptr + 1;
		str << "," << *ptr;
		ptr = ptr + 1;
		child["builder"] = str.str();
		child["cost"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

		size = *ptr;
		ptr = ptr + 1;
		child["type"] = (char*)ptr;
	} else if (cmd->type == command_pool::ARMORY) {
		config& child = cfg.add_child("armory");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// city
		str << *ptr;
		ptr = ptr + 1;
		str << "," << *ptr;
		ptr = ptr + 1;
		child["city"] = str.str();
		// diff
		str.str("");
		size = *ptr;
		ptr = ptr + 1;
		str << *ptr;
		ptr = ptr + 1;
		for (i = 1; i < size; i ++) {
			str << "," << *ptr;
			ptr = ptr + 1;
		}
		child["diff"] = str.str();

	} else if (cmd->type == command_pool::MOVE_HEROS) {
		config& child = cfg.add_child("move_heros");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// dst
		str << *ptr;
		ptr = ptr + 1;
		str << "," << *ptr;
		ptr = ptr + 1;
		child["dst"] = str.str();
		// heros
		str.str("");
		size = *ptr;
		ptr = ptr + 1;
		str << *ptr;
		ptr = ptr + 1;
		for (i = 1; i < size; i ++) {
			str << "," << *ptr;
			ptr = ptr + 1;
		}
		child["heros"] = str.str();
		// src
		str.str("");
		str << *ptr;
		ptr = ptr + 1;
		str << "," << *ptr;
		ptr = ptr + 1;
		child["src"] = str.str();

	} else if (cmd->type == command_pool::INTERIOR) {
		config& child = cfg.add_child("interior");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// officials
		str.str("");
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			if (i == 0) {
				str << *ptr;
			} else {
				str << "," << *ptr;
			}
			ptr = ptr + 1;
		}
		child["officials"] = str.str();
		// type
		child["type"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

	} else if (cmd->type == command_pool::BELONG_TO) {
		config& child = cfg.add_child("belong_to");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// dst
		str << *ptr;
		ptr = ptr + 1;
		str << "," << *ptr;
		ptr = ptr + 1;
		child["dst"] = str.str();
		// layer
		str.str("");
		child["layer"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// loyalty
		str.str("");
		child["loyalty"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// src
		str.str("");
		str << *ptr;
		ptr = ptr + 1;
		str << "," << *ptr;
		ptr = ptr + 1;
		child["src"] = str.str();

	} else if (cmd->type == command_pool::RANDOM_NUMBER) {
		config& child = cfg.add_child("random_number");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// value
		size = *ptr;
		ptr = ptr + 1;
		child["value"] = (char*)ptr;

	} else if (cmd->type == command_pool::CHOOSE) {
		config& child = cfg.add_child("choose");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		child["value"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

	} else if (cmd->type == command_pool::SPEAK) {
		config& child = cfg.add_child("speak");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// side
		if (*ptr) {
			child["side"] = lexical_cast<std::string>(*ptr);
		}
		ptr = ptr + 1;
		// team_name
		size = *ptr;
		ptr = ptr + 1;
		if (size) {
			child["team_name"] = (char*)ptr;
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		// id
		size = *ptr;
		ptr = ptr + 1;
		if (size) {
			child["id"] = (char*)ptr;
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		// message
		size = *ptr;
		ptr = ptr + 1;
		if (size) {
			child["message"] = (char*)ptr;
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		
	} else if (cmd->type == command_pool::ADVANCEMENT_UNIT) {
		config& child = cfg.add_child("advance_unit");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		loc.x = *ptr;
		ptr = ptr + 1;
		loc.y = *ptr;
		ptr = ptr + 1;
		loc.write(child);

	} else if (cmd->type == command_pool::CLEAR_LABELS) {
		config& child = cfg.add_child("clear_labels");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// team_name
		size = *ptr;
		ptr = ptr + 1;
		if (size) {
			child["team_name"] = (char*)ptr;
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		
	} else if (cmd->type == command_pool::DIPLOMATISM) {
		config& child = cfg.add_child("diplomatism");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// type=ally
		size = *ptr;
		ptr = ptr + 1;
		if (size) {
			child["type"] = (char*)ptr;
			ptr = ptr + (size + 1 + 3) / 4; // 边界取4整
		}
		// alignment=1
		child["alignment"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// first=1
		child["first"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// hero=102
		child["hero"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// second=2
		child["second"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// strategy=0
		child["strategy"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// target=2
		child["target"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

	} else if (cmd->type == command_pool::FINAL_BATTLE) {
		config& child = cfg.add_child("final_battle");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// human=1
		child["human"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// ai=2
		child["ai"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

	} else if (cmd->type == command_pool::INPUT) {
		config& child = cfg.add_child("input");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// value=1
		child["value"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

	} else if (cmd->type == command_pool::ADD_CARD) {
		config& child = cfg.add_child("add_card");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// side=1
		child["side"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// number=2
		child["number"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// dialog=true
		child["dialog"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

	} else if (cmd->type == command_pool::ERASE_CARD) {
		config& child = cfg.add_child("erase_card");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// heros
		str.str("");
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			if (i == 0) {
				str << *ptr;
			} else {
				str << "," << *ptr;
			}
			ptr = ptr + 1;
		}
		child["touched"] = str.str();
		// side=1
		child["side"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// index=2
		child["index"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		child["dialog"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		loc.x = *ptr;
		ptr = ptr + 1;
		loc.y = *ptr;
		ptr = ptr + 1;
		loc.write(child);

	} else if (cmd->type == command_pool::HERO_FIELD) {
		config& child = cfg.add_child("hero_field");
		int* ptr = (int*)(cmd + 1);
		size = *ptr;
		ptr = ptr + 1;
		for (i = 0; i < size; i ++) {
			config& random_cfg = cfg.add_child("random");
			random_cfg["value"] = lexical_cast<std::string>(*ptr);
			ptr = ptr + 1;
		}
		// number=1
		child["number"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;
		// side_feature=2
		child["side_feature"] = lexical_cast<std::string>(*ptr);
		ptr = ptr + 1;

	} else {
		std::stringstream str;
		str << "command_2_config, unknown type(int): " << cmd->type;
		replay::process_error(str.str());
	}
}

void replay::pool_2_config(config& cfg)
{
	unsigned char* cache = (unsigned char*)malloc(POOL_ALLOC_CACHE_SIZE);
	gzip_segment_t seg;
	int pool_data_gzip_size = 0;

	seg.min = seg.max = -1;
	for (int i = 0; i < pool_pos_vsize_; i ++) {
		config& cmd = cfg.add_child("command");
		// command_2_config(command_addr(i), cmd);
		command_2_config(command_addr2(i, seg, pool_data_gzip_size, cache), cmd);
	}
	if (!cfg_.empty()) {
		cfg.add_child("command", cfg_);
		// cfg.get_children("command")[pos_ - 1]->remove_attribute("type");
		// cfg.get_children("command")[pos_ - 1]->remove_attribute("sent");
	}
	delete cache;
}

void replay::get_replay_data(command_pool& that)
{
	if (!cfg_.empty()) {
		command_pool::command* cmd = command_pool::add_command();
		config_2_command(cfg_, cmd);
		cfg_.clear();

		// if has a undoable command, it must be send over network before "reset" current_gzip_seg_.
		// of course, should disable "undo" button on ui.
		play_controller& controller = *resources::controller;
		if (!controller.undo_stack().empty()) {
			controller.sync_undo();
		}
	}
	if (use_gzip_ && pool_data_vsize_) {
		// encode pool_data
		gzip_segment_t seg;
		seg.min = current_gzip_seg_.min;
		seg.max = pool_pos_vsize_ - 1;
		
		seg.len = gzip_codec(cache_, pool_data_vsize_, true);
		pool_data_vsize_ = 0;
		// once recallocate, gzip_codec maybe modify pool_data_, so dest must locate after gzip_codec. 
		uint8_t* dest = pool_data_ + pool_data_gzip_size_;
		pool_data_gzip_size_ += sizeof(gzip_segment_t) + seg.len;
		current_gzip_seg_.min = current_gzip_seg_.max = -1;

		memcpy(dest, &seg, sizeof(gzip_segment_t));
	}

	that = *(dynamic_cast<command_pool*>(this));
}

void replay::start_replay()
{
	pos_ = 0;
}

// [read].replay
void replay::revert_action()
{
	if (pos_ > 0)
		--pos_;
}

// [read].replay
config* replay::get_next_action()
{
	if (pos_ >= pool_pos_vsize_)
		return NULL;

	cfg_.clear();
	command_2_config(command_addr(pos_), cfg_);
	current_ = &cfg_;
	set_random(current_);
	++ pos_;
	return current_;
}

// [read].replay
void replay::pre_replay()
{
	if ((rng::random() == NULL) && (pool_pos_vsize_ > 0)){
		if (at_end()) {
			add_command(true);
		} else {
			cfg_.clear();
			command_2_config(command_addr(pos_), cfg_);
			set_random(&cfg_);
		}
	}
}

bool replay::at_end() const
{
	return pos_ >= pool_pos_vsize_;
}

void replay::set_to_end()
{
	pos_ = pool_pos_vsize_;
	current_ = NULL;
	set_random(NULL);
}

// [write]
void replay::clear()
{
	message_locations.clear();
	message_log.str(std::string());
	cfg_ = config();
	pos_ = 0;
	current_ = NULL;
	set_random(NULL);
	skip_ = 0;
	command_pool::clear();
}

// [read]
bool replay::empty()
{
	return pool_pos_vsize_? false: true;
}

void replay::add_config(const config& cfg, MARK_SENT mark)
{
	foreach (const config &cmd_cfg, cfg.child_range("command")) {
		if (!cfg_.empty()) {
			command_pool::command* cmd = command_pool::add_command();
			config_2_command(cfg_, cmd);
			if (cfg_["sent"] == "yes") {
				cmd->flags = 1;
			}
		}
		cfg_ = cmd_cfg;
		if (cfg_.child("speak")) {
			// <attention 2>
			cfg_["type"] = str_cast(command_pool::SPEAK);

			pos_ = pool_pos_vsize_ + 1;
			add_chat_message_location();
		}
		if (mark == MARK_AS_SENT) {
			cfg_["sent"] = "yes";
		}
	}
}

namespace {

replay* replay_src = NULL;

struct replay_source_manager
{
	replay_source_manager(replay* o) : old_(replay_src)
	{
		replay_src = o;
	}

	~replay_source_manager()
	{
		replay_src = old_;
	}

private:
	replay* const old_;
};

}

replay& get_replay_source()
{
	if(replay_src != NULL) {
		return *replay_src;
	} else {
		return recorder;
	}
}

bool do_replay(int side_num, replay *obj)
{
	log_scope("do replay");

	const replay_source_manager replaymanager(obj);

//	replay& replayer = (obj != NULL) ? *obj : recorder;

	if (!get_replay_source().is_skipping()){
		resources::screen->recalculate_minimap();
	}

	const rand_rng::set_random_generator generator_setter(&get_replay_source());

	update_locker lock_update(resources::screen->video(),get_replay_source().is_skipping());
	return do_replay_handle(side_num, "");
}

bool do_replay_handle(int side_num, const std::string &do_untill)
{
	//a list of units that have promoted from the last attack
	std::deque<map_location> advancing_units;

	team& current_team = (*resources::teams)[side_num - 1];
	unit_map& units = *resources::units;
	hero_map& heros = *resources::heros;


	for(;;) {
		const config *cfg = get_replay_source().get_next_action();

		//do we need to recalculate shroud after this action is processed?

		bool fix_shroud = false;
		if (cfg)
		{
			DBG_REPLAY << "Replay data:\n" << *cfg << "\n";
		}
		else
		{
			DBG_REPLAY << "Replay data at end\n";
		}

		//if there is nothing more in the records
		if(cfg == NULL) {
			//replayer.set_skip(false);
			return false;
		}

		//if we are expecting promotions here`
		if (!get_replay_source().expected_advancements().empty()) {
			//if there is a promotion, we process it and go onto the next command
			//but if this isn't a promotion, we just keep waiting for the promotion
			//command -- it may have been mixed up with other commands such as messages
			if (const config &child = cfg->child("choose")) {
				const int val = lexical_cast_default<int>(child["value"]);
				map_location loc = get_replay_source().expected_advancements().front();
				dialogs::animate_unit_advancement(loc, val);
				get_replay_source().pop_expected_advancement();

				//if there are no more advancing units, then we check for victory,
				//in case the battle that led to advancement caused the end of scenario
				if(advancing_units.empty()) {
					resources::controller->check_victory();
				}
				continue;
			}
		}

		// We return if caller wants it for this tag
		if (!do_untill.empty()
			&& cfg->child(do_untill) != NULL)
		{
			get_replay_source().revert_action();
			return false;
		}

		config::all_children_itors ch_itors = cfg->all_children_range();
		//if there is an empty command tag, create by pre_replay() or a start tag
		if (ch_itors.first == ch_itors.second || cfg->child("start"))
		{
			//do nothing
		}
		else if (const config &child = cfg->child("speak"))
		{
			const std::string &team_name = child["team_name"];
			const std::string &speaker_name = child["id"];
			const std::string &message = child["message"];
			//if (!preferences::parse_should_show_lobby_join(speaker_name, message)) return;
			bool is_whisper = (speaker_name.find("whisper: ") == 0);
			get_replay_source().add_chat_message_location();
			if (!get_replay_source().is_skipping() || is_whisper) {
				const int side = lexical_cast_default<int>(child["side"],0);
				resources::screen->add_chat_message(time(NULL), speaker_name, side, message,
						(team_name.empty() ? events::chat_handler::MESSAGE_PUBLIC
						: events::chat_handler::MESSAGE_PRIVATE),
						preferences::message_bell());
			}
		}
		else if (const config &child = cfg->child("label"))
		{
			terrain_label label(resources::screen->labels(), child);

			resources::screen->labels().set_label(label.location(),
						label.text(),
						label.team_name(),
						label.color());
		}
		else if (const config &child = cfg->child("clear_labels"))
		{
			resources::screen->labels().clear(std::string(child["team_name"]), child["force"].to_bool());
		}
		else if (const config &child = cfg->child("rename"))
		{
			
		}

		else if (cfg->child("init_side"))
		{
			resources::controller->do_init_side(side_num - 1);
		}

		//if there is an end turn directive
		else if (cfg->child("end_turn"))
		{
			if (const config &child = cfg->child("verify")) {
				verify(*resources::units, child);
			}

			return true;
		}
		else if (cfg->child("init_ai"))
		{
			std::vector<mr_data> mrs;
			units.calculate_mrs_data(mrs, side_num);
		}
		else if (const config &child = cfg->child("recruit")) {
			int cost = lexical_cast_default<int>(child["cost"]);

			map_location loc(child, resources::state_of_game);

			const unit_type* ut = unit_types.find(child["type"].str());
			if (!ut) {
				std::stringstream errbuf;
				errbuf << "recruiting illegal unit: '" << child["type"].str() << "'\n";
				replay::process_error(errbuf.str());
			}

			artifical* city = resources::units->city_from_loc(loc);
			if (!city) {
				std::stringstream errbuf;
				errbuf << "cannot recruit unit: " << loc << "\n";
				replay::process_error(errbuf.str());
			}

			const std::vector<std::string> heros_army = utils::split(child["heros"]);
			std::vector<const hero*> v;
			for (std::vector<std::string>::const_iterator itor = heros_army.begin(); itor != heros_army.end(); ++ itor) {
				int number = lexical_cast_default<int>(*itor);
				hero& h = heros[number];
				v.push_back(&h);
				city->hero_go_out(h);
				if (h.official_ == hero_official_commercial) {
					current_team.erase_commercial(&h);
				}
			}

			type_heros_pair pair(ut, v);
			unit* new_unit = new unit(*resources::units, heros, pair, city->cityno(), true, false);
			// 必须正确以下这两个变量，否则此个回合将不能移动、攻击（虽然是回放，但移动要靠移动力、攻击要靠攻击数，这些都是要遵循的）
			new_unit->set_movement(new_unit->total_movement());
			new_unit->set_attacks(new_unit->attacks_total());
			current_team.spend_gold(cost);

			if (child["human"].to_int()) {
				new_unit->set_human(true);
			}
			city->troop_come_into(new_unit, -1, false);
			
			resources::screen->invalidate(loc);

			statistics::recruit_unit(*new_unit);

			LOG_REPLAY << "-> " << (current_team.gold()) << "\n";
			fix_shroud = !get_replay_source().is_skipping();

			map_location loc2(MAGIC_RESIDE, city->reside_troops().size() - 1);
			game_events::fire("post_recruit", loc, loc2);

		} else if (const config &child = cfg->child("armory")) {
			std::vector<std::string> vector_str = utils::split(child["city"]);
			map_location src(lexical_cast_default<int>(vector_str[0]) - 1, lexical_cast_default<int>(vector_str[1]) - 1);
			artifical* city = resources::units->city_from_loc(src);

			const std::vector<std::string> diff = utils::split(child["diff"]);
			hero_map& heros = *resources::heros;

			// move heros from src_city to dst_city
			std::vector<unit*>& reside_troops = city->reside_troops();
			std::vector<hero*>& fresh_heros = city->fresh_heros();
			std::vector<hero*> previous_captains, current_captains;

			for (std::vector<std::string>::const_iterator itor = diff.begin(); itor != diff.end(); ) {
				size_t index = lexical_cast_default<size_t>(*itor);
				++ itor;
				size_t size = lexical_cast_default<size_t>(*itor);
				++ itor;
				unit& u = *reside_troops[index];
				previous_captains.push_back(&u.master());
				if (u.second().valid()) {
					previous_captains.push_back(&u.second());
				}
				if (u.third().valid()) {
					previous_captains.push_back(&u.third());
				}
				std::vector<hero*> captains;
				for (size_t i = 0; i < size; i ++) {
					captains.push_back(&heros[lexical_cast_default<size_t>(*itor)]);
					current_captains.push_back(captains.back());
					++ itor;
				}
				u.replace_captains(captains);
			}
			std::copy(previous_captains.begin(), previous_captains.end(), std::back_inserter(fresh_heros));
			for (std::vector<hero*>::const_iterator itor = current_captains.begin(); itor != current_captains.end(); ++ itor) {
				std::vector<hero*>::iterator itor1 = std::find(fresh_heros.begin(), fresh_heros.end(), *itor);
				fresh_heros.erase(itor1);
			}
			for (std::vector<hero*>::iterator h = fresh_heros.begin(); h != fresh_heros.end(); ++ h) {
				(*h)->status_ = hero_status_idle;
			}
			std::sort(fresh_heros.begin(), fresh_heros.end(), compare_leadership);

			// commercial officials
			bool commercial_changed = false;
			// earse_commercial maybe modify commerical, there should use copy.
			std::vector<hero*> commercial = current_team.commercials();
			for (size_t i = 0; i < commercial.size(); i ++) {
				hero* h = commercial[i];
				if (h->city_ != city->cityno()) {
					continue;
				}
				if (std::find(fresh_heros.begin(), fresh_heros.end(), h) == fresh_heros.end()) {
					current_team.erase_commercial(h);
					commercial_changed = true;
				}
			}
			if (commercial_changed) {
				unit::commercial_exploiture_ = current_team.commercial_exploiture();
			}

			resources::screen->invalidate(city->get_location());

			game_events::fire("post_armory", city->get_location());

		} else if (const config &child = cfg->child("move_heros")) {
			std::vector<std::string> vector_str = utils::split(child["src"]);
			map_location src(lexical_cast_default<int>(vector_str[0]) - 1, lexical_cast_default<int>(vector_str[1]) - 1);
			artifical* src_city = resources::units->city_from_loc(src);

			vector_str = utils::split(child["dst"]);
			map_location dst(lexical_cast_default<int>(vector_str[0]) - 1, lexical_cast_default<int>(vector_str[1]) - 1);
			artifical* dst_city = resources::units->city_from_loc(dst);

			const std::vector<std::string> checked_heros = utils::split(child["heros"]);
			// hero_map& heros = *resources::heros;

			// move heros from src_city to dst_city
			std::vector<hero*>& src_heros = src_city->fresh_heros();
			std::vector<hero*>& dst_heros = dst_city->finish_heros();
			size_t has_erase_heros = 0;

			for (std::vector<std::string>::const_iterator itor = checked_heros.begin(); itor != checked_heros.end(); ++ itor) {
				size_t index = lexical_cast_default<size_t>(*itor);
				hero* h = src_heros[index - has_erase_heros];
				dst_city->move_into(*h);
				// 注意: 后面那个括号一定要加
				// 例: heros原来有7项, 删了四项后剩三项, has_erase_heros=4, 使得heors.begin() + *itor已是非法
				src_heros.erase(src_heros.begin() + (index - has_erase_heros));
				has_erase_heros ++;
			}
			resources::screen->invalidate(src_city->get_location());
			resources::screen->invalidate(dst_city->get_location());

			game_events::fire("post_moveheros", dst_city->get_location(), src_city->get_location());

		} else if (const config &child = cfg->child("interior")) {
			const std::vector<std::string> official_heros = utils::split(child["officials"]);
			int type = child["type"].to_int(-1);

			std::vector<hero*> officials;

			for (std::vector<std::string>::const_iterator itor = official_heros.begin(); itor != official_heros.end(); ++ itor) {
				size_t index = lexical_cast_default<size_t>(*itor);
				officials.push_back(&heros[index]);
			}
			if (type == department::commercial) {
				current_team.set_commercials(officials);			
			}

		} else if (const config &child = cfg->child("belong_to")) {
			int layer = child["layer"].to_int();
			bool loyalty = child["loyalty"].to_int()? true: false;
			std::vector<std::string> vector_str = utils::split(child["src"]);
			map_location src(lexical_cast_default<int>(vector_str[0]) - 1, lexical_cast_default<int>(vector_str[1]) - 1);
			unit* u = &*resources::units->find(src, (layer == unit_map::OVERLAY)? true: false);

			vector_str = utils::split(child["dst"]);
			map_location dst(lexical_cast_default<int>(vector_str[0]) - 1, lexical_cast_default<int>(vector_str[1]) - 1);
			artifical* to = resources::units->city_from_loc(dst);

			to->unit_belong_to(u, loyalty);
			resources::screen->invalidate(to->get_location());

		} else if (const config &child = cfg->child("build")) {
			hero_map& heros = *resources::heros;
			std::vector<std::string> vector_str = utils::split(child["builder"]);
			map_location builder_loc(lexical_cast_default<int>(vector_str[0]) - 1, lexical_cast_default<int>(vector_str[1]) - 1);
			unit& builder = *resources::units->find(builder_loc);

			int cost = child["cost"].to_int();

			vector_str = utils::split(child["artifical"]);
			map_location art_loc(lexical_cast_default<int>(vector_str[0]) - 1, lexical_cast_default<int>(vector_str[1]) - 1);

			vector_str = utils::split(child["city"]);
			map_location city_loc(lexical_cast_default<int>(vector_str[0]) - 1, lexical_cast_default<int>(vector_str[1]) - 1);

			const unit_type* ut = unit_types.find(child["type"]);
			artifical* city = resources::units->city_from_loc(city_loc);

			std::vector<const hero*> v;
			if (ut->master() != HEROS_INVALID_NUMBER) {
				v.push_back(&heros[ut->master()]);
			} else {
				v.push_back(&city->master());
			}
			type_heros_pair pair(ut, v);
			artifical new_unit(*resources::units, heros, pair, city->cityno(), true);

			current_team.spend_gold(cost);

			resources::units->add(art_loc, &new_unit);

			// 建筑武将本回合结束动作
			builder.set_movement(0);
			builder.set_attacks(0);

			fix_shroud = !get_replay_source().is_skipping();
			game_events::fire("post_build", art_loc, builder_loc);

		} else if (const config &child = cfg->child("disband")) {
			const std::string& disband_index = child["value"];
			const int index = lexical_cast_default<int>(disband_index);

			map_location loc(child, resources::state_of_game);

			if (index >= 0) {
				artifical* disband_city = resources::units->city_from_loc(loc);

				std::vector<unit*>& reside_troops = disband_city->reside_troops();
				unit& u = *reside_troops[index];
				// sort_units(reside_troops);

				if (index >= int(reside_troops.size())) {
					replay::process_error("illegal disband\n");
				}

				// add gold to the gold amount
				current_team.spend_gold(-1 * u.cost() * 2 / 3);
				// add hero to the hero list in city
				u.master().status_ = hero_status_idle;
				disband_city->fresh_heros().push_back(&u.master());
				if (u.second().valid()) {
					u.second().status_ = hero_status_idle;
					disband_city->fresh_heros().push_back(&u.second());
				}
				if (u.third().valid()) {
					u.third().status_ = hero_status_idle;
					disband_city->fresh_heros().push_back(&u.third());
				}
				// erase this unit from reside troop
				disband_city->troop_go_out(index);
			} else {
				artifical* art = unit_2_artifical(&*resources::units->find(loc, (index == -1 * unit_map::OVERLAY)? true: false));
				resources::units->erase(art);

				// loc及周围所有格子置为无效
				size_t size = (sizeof(adjacent_1) / sizeof(map_offset)) >> 1;
				map_offset* adjacent_ptr = adjacent_1[loc.x & 0x1];
				for (size_t i = 0; i < size; i ++) {
					resources::screen->invalidate(map_location(loc.x + adjacent_ptr[i].x, loc.y + adjacent_ptr[i].y));
				}
			}

			resources::screen->invalidate(loc);
			game_events::fire("post_disband", loc);

		} else if (const config &child = cfg->child("event")) {
			int type = child["type"].to_int();

			map_location loc(child, NULL);

			if (type == replay::EVENT_ENCOURAGE) {
				unit& u = *resources::units->find(loc);
				hero* h2 = u.can_encourage();
				u.do_encourage(u.master(), *h2);

			} else if (type == replay::EVENT_RPG_INDEPENDENCE) {
				resources::controller->rpg_independence(true);
			}
			resources::screen->invalidate(loc);

		} else if (const config &child = cfg->child("assemble_treasure")) {
			const std::vector<std::string> diff_vstr = utils::split(child["diff"]);

			std::vector<size_t>& holded_treasures = current_team.holded_treasures();
			std::map<int, int> diff;
			std::set<unit*> diff_troops;
			for (std::vector<std::string>::const_iterator it = diff_vstr.begin(); it != diff_vstr.end(); ++ it) {
				int number = lexical_cast_default<int>(*it);
				hero& h = heros[number];
				if (h.treasure_ != HEROS_NO_TREASURE) {
					holded_treasures.push_back(h.treasure_);
					h.treasure_ = HEROS_NO_TREASURE;
				}
				++ it;
				int t0 = lexical_cast_default<int>(*it);
				diff[number] = t0;
			}
			std::sort(holded_treasures.begin(), holded_treasures.end());

			for (std::map<int, int>::const_iterator it = diff.begin(); it != diff.end(); ++ it) {
				hero& h = heros[it->first];
				h.treasure_ = it->second;
				if (h.treasure_ != HEROS_NO_TREASURE) {
					std::vector<size_t>::iterator it2 = std::find(holded_treasures.begin(), holded_treasures.end(), h.treasure_);
					holded_treasures.erase(it2);
				}

				unit* u = find_unit(units, h);
				if (!u->is_artifical()) {
					diff_troops.insert(u);
				}
			}
			for (std::set<unit*>::iterator it = diff_troops.begin(); it != diff_troops.end(); ++ it) {
				unit& u = **it;
				u.adjust();
			}
			
		} else if (const config &child = cfg->child("rpg_exchange")) {
			const std::vector<std::string> human_pairs = utils::split(child["human"]);
			std::vector<size_t> v;
			for (std::vector<std::string>::const_iterator itor = human_pairs.begin(); itor != human_pairs.end(); ++ itor) {
				int number = lexical_cast_default<int>(*itor);
				v.push_back(number);
			}
			int ai_pair = child["ai"].to_int();
			resources::controller->rpg_exchange(v, ai_pair);
		}
		else if (const config &child = cfg->child("countdown_update"))
		{
			const std::string &num = child["value"];
			const int val = lexical_cast_default<int>(num);
			const std::string &tnum = child["team"];
			const int tval = lexical_cast_default<int>(tnum);
			if (tval <= 0  || tval > int(resources::teams->size())) {
				std::stringstream errbuf;
				errbuf << "Illegal countdown update \n"
					<< "Received update for :" << tval << " Current user :"
					<< side_num << "\n" << " Updated value :" << val;

				replay::process_error(errbuf.str());
			} else {
				(*resources::teams)[tval - 1].set_countdown_time(val);
			}
		} else if (cfg->child("move") || cfg->child("expedite")) {
			const config* child;
			if (cfg->child("move")) {
				child = &cfg->child("move");
			} else {
				child = &cfg->child("expedite");
			}

			const std::string x = *child->get("x");
			const std::string y = *child->get("y");
			std::vector<map_location> steps = parse_location_range(resources::game_map, x, y);

			if (steps.empty()) {
				WRN_REPLAY << "Warning: Missing path data found in [move]\n";
				continue;
			}

			map_location src = steps.front();
			map_location dst = steps.back();

			if (src == dst) {
				WRN_REPLAY << "Warning: Move with identical source and destination. Skipping...";
				continue;
			}

			unit_map::iterator u = resources::units->find(dst);
			if (u.valid() && !unit_is_city(&*u)) {
				std::stringstream errbuf;
				errbuf << "destination already occupied: "
				       << dst << '\n';
				replay::process_error(errbuf.str());
			}
			u = resources::units->find(src);
			if (!u.valid()) {
				std::stringstream errbuf;
				errbuf << "unfound location for source of movement: "
				       << src << " -> " << dst << '\n';
				replay::process_error(errbuf.str());
			}

			artifical* expedite_city = NULL;
			int expedite_index = -1;
			if (cfg->child("expedite")) {
				const std::string& recall_num = *child->get("value");
				expedite_index = lexical_cast_default<int>(recall_num);

				expedite_city = resources::units->city_from_loc(src);

				std::vector<unit*>& reside_troops = expedite_city->reside_troops();
				// sort_units(reside_troops);

				if (expedite_index >= 0 && expedite_index < int(expedite_city->reside_troops().size())) {
					// 1、银袍法师移动到一个城市
					// 2、银袍法师在该回合内又移出城市。这次移动时发现剩余移动力是0，致使以下的move_unit失败！！
					// ——我怀疑这就是个bug，这里先做个记号，等AI代码时一并改的掉。
					// 暂时强制恢复所有移动力，使得底下的move_unit一定成功
					// reside_troops[expedite_index].set_movement(reside_troops[expedite_index].total_movement());

					statistics::recall_unit(*reside_troops[expedite_index]);
					resources::units->set_expediting(expedite_city, expedite_index);
					resources::screen->place_expedite_city(*expedite_city);
				} else {
					replay::process_error("illegal recall\n");
				}
				// fix_shroud = !get_replay_source().is_skipping();
				
			}

			// bool show_move = preferences::show_ai_moves() || !(current_team.is_ai() || current_team.is_network_ai()); 
			bool show_move = true;
			::move_unit(NULL, steps, NULL, NULL, show_move, NULL, true, true, true);

			unit& builder = *resources::units->find(dst);

			if (cfg->child("expedite")) {
				resources::screen->remove_expedite_city();
				resources::units->set_expediting();
				expedite_city->troop_go_out(expedite_index);

				resources::screen->invalidate(steps.front());
			}

			//NOTE: The AI fire sighetd event whem moving in the FoV of team 1
			// (supposed to be the human player in SP)
			// That's ugly but let's try to make the replay works like that too
			if (side_num != 1 && resources::teams->front().fog_or_shroud() && !resources::teams->front().fogged(dst)
					 && (current_team.is_ai() || current_team.is_network_ai()))
			{
				// the second parameter is impossible to know
				// and the AI doesn't use it too in the local version
				game_events::fire("sighted",dst);
			}

			game_events::fire("sitdown", dst, map_location());
		}

		else if (const config &child = cfg->child("attack"))
		{
			const config &destination = child.child("destination");
			const config &source = child.child("source");

			if (!destination || !source) {
				replay::process_error("no destination/source found in attack\n");
			}

			//we must get locations by value instead of by references, because the iterators
			//may become invalidated later
			const map_location src(source, resources::state_of_game);
			const map_location dst(destination, resources::state_of_game);

			const std::string &weapon = child["weapon"];
			const int weapon_num = lexical_cast_default<int>(weapon);
			
			int layer = child["layer"].to_int();
			int defender_layer = child["defender_layer"].to_int();

			const std::string &def_weapon = child["defender_weapon"];
			int def_weapon_num = -1;
			if (def_weapon.empty()) {
				// Let's not gratuitously destroy backwards compat.
				ERR_REPLAY << "Old data, having to guess weapon\n";
			} else {
				def_weapon_num = lexical_cast_default<int>(def_weapon);
			}

			unit_map::iterator u = resources::units->find(src, (layer == unit_map::OVERLAY)? true: false);
			if (!u.valid()) {
				replay::process_error("unfound location for source of attack\n");
			}

			if(size_t(weapon_num) >= u->attacks().size()) {
				replay::process_error("illegal weapon type in attack\n");
			}

			unit_map::iterator tgt = resources::units->find(dst, (defender_layer == unit_map::OVERLAY)? true: false);

			if (!tgt.valid()) {
				std::stringstream errbuf;
				errbuf << "unfound defender for attack: " << src << " -> " << dst << '\n';
				replay::process_error(errbuf.str());
			}

			if (def_weapon_num >=
					static_cast<int>(tgt->attacks().size())) {

				replay::process_error("illegal defender weapon type in attack\n");
			}

			rand_rng::seed_t seed = lexical_cast<rand_rng::seed_t>(child["seed"]);
			rand_rng::set_seed(seed);

			std::pair<map_location, map_location> to_locs = attack_unit(*u, *tgt, weapon_num, def_weapon_num, !get_replay_source().is_skipping(), child.child("duel"));

			u = resources::units->find(to_locs.first);
			tgt = resources::units->find(to_locs.second);

			if (u.valid() && u->advances()) {
				get_replay_source().add_expected_advancement(to_locs.first);
			}
			
			if (tgt.valid() && tgt->advances()) {
				get_replay_source().add_expected_advancement(to_locs.second);
			}

			//check victory now if we don't have any advancements. If we do have advancements,
			//we don't check until the advancements are processed.
			if(get_replay_source().expected_advancements().empty()) {
				resources::controller->check_victory();
			}
			fix_shroud = !get_replay_source().is_skipping();
		}
		else if (const config &child = cfg->child("fire_event"))
		{
			foreach (const config &v, child.child_range("set_variable")) {
				resources::state_of_game->set_variable(v["name"], v["value"]);
			}
			const std::string &event = child["raise"];
			//exclude these events here, because in a replay proper time of execution can't be
			//established and therefore we fire those events inside play_controller::init_side
			if ((event != "side turn") && (event != "turn 1") && (event != "new turn") && (event != "turn refresh")){
				if (const config &source = child.child("source")) {
					game_events::fire(event, map_location(source, resources::state_of_game));
				} else {
					game_events::fire(event);
				}
			}

		}
		else if (const config &child = cfg->child("advance_unit"))
		{
			const map_location loc(child, resources::state_of_game);
			get_replay_source().add_expected_advancement(loc);

		} 
		else if (const config &child = cfg->child("diplomatism"))
		{
			int emissary = child["hero"].to_int();
			int my_side = child["first"].to_int();
			int to_ally_side = child["second"].to_int();
			int strategy_index = child["strategy"].to_int(-1);
			int target_side = child["target"].to_int(-1);
			
			resources::controller->do_ally(true, my_side, to_ally_side, emissary, target_side, strategy_index);

		} 
		else if (const config &child = cfg->child("final_battle")) 
		{
			resources::controller->final_battle(side_num, child["human"].to_int(-1), child["ai"].to_int(-1));

		}
		else if (const config &child = cfg->child("add_card")) 
		{
			hero_map& heros = *resources::heros;
			int side = child["side"].to_int();
			int number = child["number"].to_int();
			bool dialog = child["dialog"].to_int()? true: false;
			team& selected_team = (*resources::teams)[side - 1];
			selected_team.add_card(number, true);
			if (dialog) {
				utils::string_map symbols;
				std::stringstream strstr;
				symbols["first"] = selected_team.holded_card(selected_team.holded_cards().size() - 1).name();
				game_events::show_hero_message(&heros[214], NULL, vgettext("Get card: $first.", symbols), game_events::INCIDENT_CARD);
			}
		}
		else if (const config &child = cfg->child("erase_card")) 
		{
			int side = child["side"].to_int();
			int index = child["index"].to_int();
			bool dialog = child["dialog"].to_int()? true: false;
			const std::vector<std::string> touched = utils::split(child["touched"]);

			map_location loc(child, resources::state_of_game);
			team& selected_team = (*resources::teams)[side - 1];
			card& c = selected_team.holded_card(index);
			if (loc.valid()) {
				std::vector<std::pair<int, unit*> > maps;
				for (std::vector<std::string>::const_iterator it = touched.begin(); it != touched.end(); ++ it) {
					int number = lexical_cast_default<int>(*it);
					if (c.target_hero()) {
						maps.push_back(std::make_pair<int, unit*>(number, find_unit(units, heros[number])));
						if (!maps.back().second) {
							replay::process_error("illegal hero number in erase_card\n");
						}
						// ignore loc.x/y
						++ it;
						++ it;
					} else {
						++ it;
						int x = lexical_cast_default<int>(*it);
						++ it;
						int y = lexical_cast_default<int>(*it);
						unit_map::const_iterator it_u = units.find(map_location(x, y));
						unit* u = &*it_u;
						if (number >= 0) {
							artifical* art = unit_2_artifical(u);
							std::vector<unit*>& resides = art->reside_troops();
							u = resides[number];
						}
						maps.push_back(std::make_pair<int, unit*>(number, u));
					}
				}
				selected_team.consume_card(index, loc, true, maps);
			} else {
				selected_team.erase_card(index, true);
			}
			if (dialog) {
				utils::string_map symbols;
				std::stringstream strstr;
				symbols["first"] = c.name();
				game_events::show_hero_message(&heros[214], NULL, vgettext("Lost card: $first.", symbols), game_events::INCIDENT_CARD);
			}
		}
		else if (const config &child = cfg->child("hero_field")) 
		{
			(*resources::heros)[child["number"].to_int(-1)].side_feature_ = child["side_feature"].to_int(-1);

		} else {
			if (!cfg->child("checksum")) {
				replay::process_error("unrecognized action:\n" + cfg->debug());
			}
		}

		//Check if we should refresh the shroud, and redraw the minimap/map tiles.
		//This is needed for shared vision to work properly.
		if (fix_shroud && clear_shroud(side_num) && !recorder.is_skipping()) {
			resources::screen->recalculate_minimap();
			resources::screen->invalidate_game_status();
			resources::screen->invalidate_all();
			resources::screen->draw();
		}

		if (const config &child = cfg->child("verify")) {
			verify(*resources::units, child);
		}
	}

	return false; /* Never attained, but silent a gcc warning. --Zas */
}

replay_network_sender::replay_network_sender(replay& obj) : obj_(obj), upto_(obj_.ncommands())
{
}

replay_network_sender::~replay_network_sender()
{
	commit_and_sync();
}

void replay_network_sender::sync_non_undoable()
{
	if (network::nconnections() > 0) {
		int cmd_end = obj_.ncommands();
		if (upto_ >= cmd_end - 1) {
			return;
		}
		config cfg;
		const config& data = cfg.add_child("turn", obj_.get_data_range(upto_, cmd_end - 1));
		if (data.empty() == false) {
			network::send_data(cfg, 0);
			// upto_ += data.get_children("command").size();
		}

		upto_ = cmd_end - 1;
	}

/*
	if (network::nconnections() > 0) {
		config cfg;
		const config& data = cfg.add_child("turn",obj_.get_data_range(upto_,obj_.ncommands(), replay::NON_UNDO_DATA));
		if(data.empty() == false) {
			network::send_data(cfg, 0, true);
		}
	}
*/
}

void replay_network_sender::commit_and_sync()
{
	if (network::nconnections() > 0) {
		config cfg;
		const config& data = cfg.add_child("turn", obj_.get_data_range(upto_,obj_.ncommands()));
		if (data.empty() == false) {
			network::send_data(cfg, 0);
		}
		upto_ = obj_.ncommands();
	}
}

config mp_sync::get_user_choice(const std::string &name, const user_choice &uch,
	int side, bool force_sp)
{
	if (force_sp && network::nconnections() != 0 &&
	    resources::state_of_game->phase() != game_state::PLAY)
	{
		/* We are in a multiplayer game, during an early event which
		   prevents synchronization, and the WML is not interested
		   in a random result. We cannot silently ignore the issue,
		   since it would lead to a broken replay. To be sure that
		   the WML does not catch the error and keep the game going,
		   we use a sticky exception to forcefully quit. */
		ERR_REPLAY << "MP synchronization does not work during prestart and start events.";
		throw end_level_exception(QUIT);
	}
	if (resources::state_of_game->phase() == game_state::PLAY || force_sp)
	{
		/* We have to communicate with the player and store the
		   choices in the replay. So a decision will be made on
		   one host and shared amongst all of them. */

		/* process the side parameter and ensure it is within boundaries */
		if (unsigned(side - 1) >= resources::teams->size())
			side = resources::controller->current_side();

		int active_side = side;
		if ((*resources::teams)[active_side - 1].is_local() &&
		    get_replay_source().at_end())
		{
			/* The decision is ours, and it will be inserted
			   into the replay. */
			DBG_REPLAY << "MP synchronization: local choice\n";
			config cfg = uch.query_user();
			recorder.user_input(name, cfg);
			return cfg;

		} else {
			/* The decision has already been made, and must
			   be extracted from the replay. */
			DBG_REPLAY << "MP synchronization: remote choice\n";
			do_replay_handle(active_side, name);
			const config *action = get_replay_source().get_next_action();
			if (!action || !*(action = &action->child(name))) {
				replay::process_error("[" + name + "] expected but none found\n");
				return config();
			}
			return *action;
		}
	}
	else
	{
		/* Neither the user nor a replay can be consulted, so a
		   decision will be made at all hosts simultaneously.
		   The result is not stored in the replay, since the
		   other clients have already taken the same decision. */
		DBG_REPLAY << "MP synchronization: synchronized choice\n";
		return uch.random_choice(resources::state_of_game->rng());
	}
}