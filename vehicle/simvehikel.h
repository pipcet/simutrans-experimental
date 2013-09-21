/*
 * Copyright (c) 1997 - 2001 Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic license.
 * (see license.txt)
 */

/*
 * Vehicle base type.
 */

#ifndef _simvehikel_h
#define _simvehikel_h

#include <limits>
#include <string>
#include "../simtypes.h"
#include "../simworld.h"
#include "../simconvoi.h"
#include "../simdings.h"
#include "../halthandle_t.h"
#include "../convoihandle_t.h"
#include "../ifc/fahrer.h"
#include "../boden/grund.h"
#include "../besch/vehikel_besch.h"
#include "../vehicle/overtaker.h"
#include "../tpl/slist_tpl.h"

#include "../tpl/fixed_list_tpl.h"

class convoi_t;
class schedule_t;
class signal_t;
class ware_t;
class route_t;

// for aircrafts:
// length of the holding pattern.
#define HOLDING_PATTERN_LENGTH 16
// offset of end tile of the holding pattern before touchdown tile.
#define HOLDING_PATTERN_OFFSET 3

/*----------------------- Fahrdings ------------------------------------*/

/**
 * Basisklasse f�r alle Fahrzeuge
 *
 * @author Hj. Malthaner
 */
class vehikel_basis_t : public ding_t
{
protected:
	// offsets for different directions
	static sint8 dxdy[16];

	// to make the length on diagonals configurable
	// Number of vehicle steps along a diagonal...
	// remember to subtract one when stepping down to 0
	static uint8 diagonal_vehicle_steps_per_tile;
	static uint8 old_diagonal_vehicle_steps_per_tile;
	static uint16 diagonal_multiplier;

	// [0]=xoff [1]=yoff
	static sint8 overtaking_base_offsets[8][2];

	/**
	 * Aktuelle Fahrtrichtung in Bildschirm-Koordinaten
	 * @author Hj. Malthaner
	 */
	ribi_t::ribi fahrtrichtung;

	// true on slope (make calc_height much faster)
	uint8 use_calc_height:1;

	// if true, use offests to emulate driving on other side
	uint8 drives_on_left:1;

	sint8 dx, dy;

	// number of steps in this tile (255 per tile)
	uint8 steps, steps_next;

	/**
	 * Next position on our path
	 * @author Hj. Malthaner
	 */
	koord3d pos_next;

	/**
	 * Offsets fuer Bergauf/Bergab
	 * @author Hj. Malthaner
	 */
	sint8 hoff;

	// cached image
	image_id bild;

	// The current livery of this vehicle.
	// @author: jamespetts, April 2011
	std::string current_livery;

	/**
	 * Vehicle movement: calculates z-offset of vehicles on slopes,
	 * handles vehicles that are invisible in tunnels.
	 * @param gr vehicle is on this ground (can be NULL)
	 * @return new offset
	 */
	sint8 calc_height(grund_t *gr);

	/**
	 * Vehicle movement: check whether this vehicle can enter the next tile (pos_next).
	 */
	virtual bool hop_check() = 0;

	/**
	 * Vehicle movement: change tiles, calls verlasse_feld and betrete_feld.
	 * @return pointer to ground of new position (never NULL)
	 */
	virtual grund_t* hop() = 0;

	virtual void update_bookkeeping(uint32 steps) = 0;

	virtual void calc_bild() = 0;

	// check for road vehicle, if next tile is free
	vehikel_basis_t *no_cars_blocking( const grund_t *gr, const convoi_t *cnv, const uint8 current_fahrtrichtung, const uint8 next_fahrtrichtung, const uint8 next_90fahrtrichtung );

	// only needed for old way of moving vehicles to determine position at loading time
	bool is_about_to_hop( const sint8 neu_xoff, const sint8 neu_yoff ) const;


public:
	// only called during load time: set some offsets
	static void set_diagonal_multiplier( uint32 multiplier, uint32 old_multiplier );
	static uint16 get_diagonal_multiplier() { return diagonal_multiplier; }
	static uint8 get_diagonal_vehicle_steps_per_tile() { return diagonal_vehicle_steps_per_tile; }

	static void set_overtaking_offsets( bool driving_on_the_left );

	// if true, this convoi needs to restart for correct alignment
	bool need_realignment() const;

	uint32 fahre_basis(uint32 dist);	// basis movement code

	inline void set_bild( image_id b ) { bild = b; }
	virtual image_id get_bild() const {return bild;}

	sint8 get_hoff() const {return hoff;}
	uint8 get_steps() const {return steps;} // number of steps pass on the current tile. 
	uint8 get_steps_next() const {return steps_next;} // total number of steps to pass on the current tile - 1. Mostly VEHICLE_STEPS_PER_TILE - 1 for straight route or diagonal_vehicle_steps_per_tile - 1 for a diagonal route.

	// to make smaller steps than the tile granularity, we have to calculate our offsets ourselves!
	virtual void get_screen_offset( int &xoff, int &yoff, const sint16 raster_width ) const;

	virtual void rotate90();

	static ribi_t::ribi calc_richtung(koord start, koord ende);
	ribi_t::ribi calc_set_richtung(koord start, koord ende);
	uint16 get_tile_steps(const koord &start, const koord &ende, /*out*/ ribi_t::ribi &richtung) const;

	ribi_t::ribi get_fahrtrichtung() const {return fahrtrichtung;}

	koord3d get_pos_next() const {return pos_next;}

	virtual waytype_t get_waytype() const = 0;

	// true, if this vehicle did not moved for some time
	virtual bool is_stuck() { return true; }

	/**
	 * Vehicle movement: enter tile, add this to the ground.
	 * @pre position (ding_t::pos) needs to be updated prior to calling this functions
	 * @return pointer to ground (never NULL)
	 */
	virtual grund_t* betrete_feld();

	/**
	 * Vehicle movement: leave tile, release reserved crossing, remove vehicle from the ground.
	 */
	virtual void verlasse_feld();

	virtual overtaker_t *get_overtaker() { return NULL; }

#ifdef INLINE_DING_TYPE
protected:
	vehikel_basis_t(karte_t *welt, typ type);
	vehikel_basis_t(karte_t *welt, typ type, koord3d pos);
#else
	vehikel_basis_t(karte_t *welt);

	vehikel_basis_t(karte_t *welt, koord3d pos);
#endif
};


template<> inline vehikel_basis_t* ding_cast<vehikel_basis_t>(ding_t* const d)
{
	return d->is_moving() ? static_cast<vehikel_basis_t*>(d) : 0;
}


/**
 * Klasse f�r alle Fahrzeuge mit Route
 *
 * @author Hj. Malthaner
 */

class vehikel_t : public vehikel_basis_t, public fahrer_t
{
private:
	/**
	* Kaufdatum in months
	* @author Hj. Malthaner
	*/
	sint32 insta_zeit;

	/* For the more physical acceleration model friction is introduced
	* frictionforce = gamma*speed*weight
	* since the total weight is needed a lot of times, we save it
	* @author prissi
	* BG, 18.10.2011: in tons in simutrans standard, in kg in simutrans experimental
	*/
	uint32 sum_weight; 

	bool hop_check();

	/**
	 * berechnet aktuelle Geschwindigkeit aufgrund der Steigung
	 * (Hoehendifferenz) der Fahrbahn
	 */
	virtual void calc_drag_coefficient(const grund_t *gr);

	sint32 calc_modified_speed_limit(koord3d position, ribi_t::ribi current_direction, bool is_corner);

	/**
	 * Unload freight to halt
	 * @return sum of unloaded goods
	 * @author Hj. Malthaner
	 */
	uint16 unload_freight(halthandle_t halt, sint64 & revenue_from_unloading, array_tpl<sint64> & apportioned_revenues );

	/**
	 * Load freight from halt
	 * @return loading successful?
	 * @author Hj. Malthaner
	 */
	bool load_freight(halthandle_t halt) { return load_freight(halt, false); }

	bool load_freight(halthandle_t halt, bool overcrowd);

	// @author: jamespetts
	// uint16 local_bonus_supplement; 
	// A supplementary bonus for local transportation,
	// if needed, to compensate for not having the effect
	// of the long-distance speed bonus.

	// @author: jamespetts
	// Cornering settings.

	fixed_list_tpl<sint16, 16> pre_corner_direction;

	sint16 direction_steps;

	uint8 hill_up;
	uint8 hill_down;
	bool is_overweight;

	// Whether this individual vehicle is reversed.
	// @author: jamespetts
	bool reversed;

	//@author: jamespetts
	uint16 diagonal_costs;
	uint16 base_costs;

//#define debug_corners

#ifdef debug_corners
	uint16 current_corner;
#endif

	static sint64 sound_ticks;

protected:
	virtual grund_t* hop();

	virtual void update_bookkeeping(uint32 steps) {
	   // Only the first vehicle in a convoy does this,
	   // or else there is double counting.
	   // NOTE: As of 9.0, increment_odometer() also adds running costs for *all* vehicles in the convoy.
		if (ist_erstes) cnv->increment_odometer(steps);
	}

	// current limit (due to track etc.)
	sint32 speed_limit;

	ribi_t::ribi alte_fahrtrichtung;

	//uint16 target_speed[16];

	//const koord3d *lookahead[16];

	// for target reservation and search
	halthandle_t target_halt;

	/* The friction is calculated new every step, so we save it too
	* @author prissi
	*/
	sint16 current_friction;

	/**
	* Current index on the route
	* @author Hj. Malthaner
	*/
	uint16 route_index;

	uint16 total_freight;	// since the sum is needed quite often, it is chached
	slist_tpl<ware_t> fracht;   // liste der gerade transportierten g�ter ("list of goods being transported" - Google)
	const vehikel_besch_t *besch;

	convoi_t *cnv;		// != NULL falls das vehikel zu einem Convoi gehoert

	/**
	* Previous position on our path
	* @author Hj. Malthaner
	*/
	koord3d pos_prev;

	bool ist_erstes:1;				// falls vehikel im convoi f�hrt, geben diese ("appropriate vehicle in Convoi runs, these" - Google)
	bool ist_letztes:1;				// flags auskunft �ber die position ("flags provide information on the position" - Google)
	bool rauchen:1;
	bool check_for_finish:1;		// true, if on the last tile
	bool has_driven:1;

	virtual void calc_bild();

	bool check_access(const weg_t* way) const;

public:
	sint32 calc_speed_limit(const weg_t *weg, const weg_t *weg_previous, fixed_list_tpl<sint16, 16>* cornering_data, ribi_t::ribi current_direction, ribi_t::ribi previous_direction);

	virtual bool ist_befahrbar(const grund_t* ) const {return false;}

	inline bool check_way_constraints(const weg_t &way) const;

	uint8 hop_count;

//public:
	// the coordinates, where the vehicle was loaded the last time
	koord last_stop_pos;

	convoi_t *get_convoi() const { return cnv; }

	virtual void rotate90();

	virtual bool ist_weg_frei( int &/*restart_speed*/, bool /*second_check*/ ) { return true; }

	virtual grund_t* betrete_feld();

	virtual void verlasse_feld();

	virtual waytype_t get_waytype() const = 0;

	/**
	* Ermittelt die f�r das Fahrzeug geltenden Richtungsbits,
	* abh�ngig vom Untergrund.
	*
	* @author Hj. Malthaner, 04.01.01
	*/
	virtual ribi_t::ribi get_ribi(const grund_t* gr) const { return gr->get_weg_ribi(get_waytype()); }

	sint32 get_insta_zeit() const {return insta_zeit;}

	void darf_rauchen(bool yesno ) { rauchen = yesno;}

	virtual route_t::route_result_t calc_route(koord3d start, koord3d ziel, sint32 max_speed_kmh, route_t* route);
	uint16 get_route_index() const {return route_index;}
	void set_route_index(uint16 value) { route_index = value; }
	const koord3d get_pos_prev() const {return pos_prev;}

    virtual bool reroute(const uint16 reroute_index, const koord3d &ziel, route_t* route = NULL);

	/**
	* gibt das Basisbild zurueck
	* @author Hj. Malthaner
	*/
	int get_basis_bild() const { return besch->get_basis_bild(current_livery.c_str()); }

	/**
	* @return vehicle description object
	* @author Hj. Malthaner
	*/
	const vehikel_besch_t *get_besch() const {return besch; }
	void set_besch(const vehikel_besch_t* value) { besch = value; }

	/**
	* @return die running_cost in Cr/100Km
	* @author Hj. Malthaner
	*/
	int get_running_cost() const { return besch->get_running_cost(); }
	int get_running_cost(const karte_t* welt) const { return besch->get_running_cost(welt); }

	/**
	* @return fixed maintenance costs in Cr/100months
	* @author Bernd Gabriel
	*/
	uint32 get_fixed_cost() const { return besch->get_fixed_cost(); }
	uint32 get_fixed_cost(karte_t* welt) const { return besch->get_fixed_cost(welt); }

	/**
	* spielt den Sound, wenn das Vehikel sichtbar ist
	* @author Hj. Malthaner
	*/
	void play_sound() const;

	/**
	* Bereitet Fahrzeiug auf neue Fahrt vor - wird aufgerufen wenn
	* der Convoi eine neue Route ermittelt
	* @author Hj. Malthaner
	*/
	void neue_fahrt( uint16 start_route_index, bool recalc );

#ifdef INLINE_DING_TYPE
protected:
	vehikel_t(karte_t *welt, typ type);
	vehikel_t(typ type, koord3d pos, const vehikel_besch_t* besch, spieler_t* sp);
public:
#else
	vehikel_t(karte_t *welt);
	vehikel_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp);
#endif

	~vehikel_t();

	void rauche() const;

	void zeige_info();

#if 0
private:
	/**
	* der normale Infotext
	* @author Hj. Malthaner
	*/
	void info(cbuffer_t & buf, bool dummy = false) const;
public:
#endif
	/**
	* Ermittelt fahrtrichtung
	* @author Hj. Malthaner
	*/
	ribi_t::ribi richtung() const;

	/* return friction constant: changes in hill and curves; may even negative downhill *
	* @author prissi
	*/
	inline sint16 get_frictionfactor() const { return current_friction; }

	/* Return total weight including freight (in kg!)
	* @author prissi
	*/
	inline uint32 get_gesamtgewicht() const { return sum_weight; }

	// returns speedlimit of ways (and if convoi enters station etc)
	// the convoi takes care of the max_speed of the vehicle
	// In Experimental this is mostly for entering stations etc.,
	// as the new physics engine handles ways
	sint32 get_speed_limit() const { return speed_limit; }
	static sint32 speed_unlimited() {return (std::numeric_limits<sint32>::max)(); }

	const slist_tpl<ware_t> & get_fracht() const { return fracht;}   // liste der gerade transportierten g�ter

	/**
	 * Rotate freight target coordinates, has to be called after rotating factories.
	 */
	void rotate90_freight_destinations(const sint16 y_size);

	/**
	* berechnet die gesamtmenge der bef�rderten waren
	*/
	uint16 get_fracht_menge() const { return total_freight; }

	/**
	* Berechnet Gesamtgewicht der transportierten Fracht in KG
	* @author Hj. Malthaner
	*/
	uint32 get_fracht_gewicht() const;

	const char * get_fracht_name() const;

	/**
	* setzt den typ der bef�rderbaren ware
	*/
	const ware_besch_t* get_fracht_typ() const { return besch->get_ware(); }

	/**
	* setzt die maximale Kapazitaet
	*/
	uint16 get_fracht_max() const {return besch->get_zuladung(); }

	const char * get_fracht_mass() const;

	void get_tooltip_info(cbuffer_t & buf) const;

	/**
	* erstellt einen Info-Text zur Fracht, z.B. zur Darstellung in einem
	* Info-Fenster
	* @author Hj. Malthaner
	*/
	void get_fracht_info(cbuffer_t & buf) const;

	// Check for straightness of way.
	//@author jamespetts
	
	enum direction_degrees {
		North = 360,
		Northeast = 45,
		East = 90,
		Southeast = 135,
		South = 180,
		Southwest = 225,
		West = 270,
		Northwest = 315,
	};

	direction_degrees get_direction_degrees(ribi_t::ribi) const;

	sint16 compare_directions(sint16 first_direction, sint16 second_direction) const;

	/**
	* loescht alle fracht aus dem Fahrzeug
	* @author Hj. Malthaner
	*/
	void loesche_fracht();

	/**
	* Payment is done per hop. It iterates all goods and calculates
	* the income for the last hop. This method must be called upon
	* every stop.
	* @return income total for last hop
	* @author Hj. Malthaner
	*/
	//sint64  calc_gewinn(koord start, koord end, convoi_t* cnv) const;

	/**
	* fahrzeug an haltestelle entladen
	* @author Hj. Malthaner
	*/
	uint16 entladen(halthandle_t halt, sint64 & revenue_from_unloading, array_tpl<sint64> & apportioned_revenues );

	/**
	* fahrzeug an haltestelle beladen
	*/
	uint16 beladen(halthandle_t halt) { return beladen(halt, false); }

	uint16 beladen(halthandle_t halt, bool overcrowd);

	// sets or querey begin and end of convois
	void set_erstes(bool janein) {ist_erstes = janein;} //janein = "yesno" (Google)
	bool is_first() const {return ist_erstes;}

	void set_letztes(bool janein) {ist_letztes = janein;}
	bool is_last() const {return ist_letztes;}

	// marks the vehicle as really used
	void set_driven() { has_driven = true; }

	virtual void set_convoi(convoi_t *c);

	/**
	* Remove freight that no longer can reach it's destination
	* i.e. because of a changed schedule
	* @author Hj. Malthaner
	*/
	void remove_stale_freight();

	/**
	* erzeuge einen f�r diesen Vehikeltyp passenden Fahrplan
	* @author Hj. Malthaner
	*/
	virtual schedule_t * erzeuge_neuen_fahrplan() const = 0;

	const char * ist_entfernbar(const spieler_t *sp);

	void rdwr(loadsave_t *file);
	virtual void rdwr_from_convoi(loadsave_t *file);

	uint32 calc_restwert() const;

	// true, if this vehicle did not moved for some time
	virtual bool is_stuck() { return cnv==NULL  ||  cnv->is_waiting(); }

	// this draws a tooltips for things stucked on depot order or lost
	virtual void display_after(int xpos, int ypos, bool dirty) const;

	bool is_reversed() const { return reversed; }
	void set_reversed(bool value);

	// Gets modified direction, used for drawing
	// vehicles in reverse formation.
	ribi_t::ribi get_direction_of_travel() const;

	uint16 get_sum_weight() const { return (sum_weight + 499) / 1000; }

	// @author: jamespetts
	uint16 get_overcrowding() const;

	// @author: jamespetts
	uint8 get_comfort(uint8 catering_level = 0) const;

	// BG, 06.06.2009: update player's fixed maintenance
	void laden_abschliessen();
	void before_delete();

	void set_current_livery(const char* liv) { current_livery = liv; }
	const char* get_current_livery() const { return current_livery.c_str(); }

	virtual sint32 get_takeoff_route_index() const { return INVALID_INDEX; }
	virtual sint32 get_touchdown_route_index() const { return INVALID_INDEX; }
};


template<> inline vehikel_t* ding_cast<vehikel_t>(ding_t* const d)
{
	return dynamic_cast<vehikel_t*>(d);
}


/**
 * Eine Klasse f�r Strassenfahrzeuge. Verwaltet das Aussehen der
 * Fahrzeuge und die Befahrbarkeit des Untergrundes.
 *
 * @author Hj. Malthaner
 * @see vehikel_t
 */
class automobil_t : public vehikel_t
{
private:
	// called internally only from ist_weg_frei()
	// returns true on success
	bool choose_route( int &restart_speed, ribi_t::dir richtung, uint16 index );

public:
	bool ist_befahrbar(const grund_t *bd) const;

protected:
	bool is_checker;

public:
	virtual grund_t* betrete_feld();

	virtual waytype_t get_waytype() const { return road_wt; }

	automobil_t(karte_t *welt, loadsave_t *file, bool first, bool last);
	automobil_t(karte_t *welt);
	automobil_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv); // start und fahrplan

	virtual void set_convoi(convoi_t *c);

	// how expensive to go here (for way search)
	virtual int get_kosten(const grund_t *, const sint32, koord);

	virtual route_t::route_result_t calc_route(koord3d start, koord3d ziel, sint32 max_speed, route_t* route);

	virtual bool ist_weg_frei(int &restart_speed, bool second_check );

	// returns true for the way search to an unknown target.
	virtual bool ist_ziel(const grund_t *,const grund_t *);

	// since we must consider overtaking, we use this for offset calculation
	virtual void get_screen_offset( int &xoff, int &yoff, const sint16 raster_width ) const;

#ifdef INLINE_DING_TYPE
#else
	ding_t::typ get_typ() const { return automobil; }
#endif

	schedule_t * erzeuge_neuen_fahrplan() const;

	virtual overtaker_t* get_overtaker() { return cnv; }
};


/**
 * Eine Klasse f�r Schienenfahrzeuge. Verwaltet das Aussehen der
 * Fahrzeuge und die Befahrbarkeit des Untergrundes.
 *
 * @author Hj. Malthaner
 * @see vehikel_t
 */
class waggon_t : public vehikel_t
{
protected:
	bool ist_befahrbar(const grund_t *bd) const;

	grund_t* betrete_feld();

	bool is_weg_frei_signal( uint16 start_index, int &restart_speed );

	bool is_weg_frei_pre_signal( signal_t *sig, uint16 start_index, int &restart_speed );

	bool is_weg_frei_longblock_signal( signal_t *sig, uint16 start_index, int &restart_speed );

	bool is_weg_frei_choose_signal( signal_t *sig, uint16 start_index, int &restart_speed );


public:
	virtual waytype_t get_waytype() const { return track_wt; }

	// since we might need to unreserve previously used blocks, we must do this before calculation a new route
	route_t::route_result_t calc_route(koord3d start, koord3d ziel, sint32 max_speed, route_t* route);

	// how expensive to go here (for way search)
	virtual int get_kosten(const grund_t *, const sint32, koord);

	// returns true for the way search to an unknown target.
	virtual bool ist_ziel(const grund_t *,const grund_t *);

	// handles all block stuff and route choosing ...
	virtual bool ist_weg_frei(int &restart_speed, bool );

	// reserves or unreserves all blocks and returns the handle to the next block (if there)
	// returns ture on successful reservation
	bool block_reserver(route_t *route, uint16 start_index, uint16 &next_singal, uint16 &next_crossing, int signal_count, bool reserve, bool force_unreserve ) const;

	void verlasse_feld();

#ifdef INLINE_DING_TYPE
protected:
	waggon_t(karte_t *welt, typ type, loadsave_t *file, bool is_first, bool is_last);
	waggon_t(typ type, koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t *cnv); // start und fahrplan
	void init(loadsave_t *file, bool is_first, bool is_last);
public:
#else
	typ get_typ() const { return waggon; }
#endif

	waggon_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last);
	waggon_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t *cnv); // start und fahrplan
	virtual ~waggon_t();

	virtual void set_convoi(convoi_t *c);

	virtual schedule_t * erzeuge_neuen_fahrplan() const;
};



/**
 * very similar to normal railroad, so wie can implement it here completely ...
 * @author prissi
 * @see vehikel_t
 */
class monorail_waggon_t : public waggon_t
{
public:
	virtual waytype_t get_waytype() const { return monorail_wt; }

#ifdef INLINE_DING_TYPE
	// all handled by waggon_t
	monorail_waggon_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last) : waggon_t(welt, monorailwaggon, file,is_first, is_last) {}
	monorail_waggon_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv) : waggon_t(monorailwaggon, pos, besch, sp, cnv) {}
#else
	// all handled by waggon_t
	monorail_waggon_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last) : waggon_t(welt, file,is_first, is_last) {}
	monorail_waggon_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv) : waggon_t(pos, besch, sp, cnv) {}

	typ get_typ() const { return monorailwaggon; }
#endif

	schedule_t * erzeuge_neuen_fahrplan() const;
};



/**
 * very similar to normal railroad, so wie can implement it here completely ...
 * @author prissi
 * @see vehikel_t
 */
class maglev_waggon_t : public waggon_t
{
public:
	virtual waytype_t get_waytype() const { return maglev_wt; }

#ifdef INLINE_DING_TYPE
	// all handled by waggon_t
	maglev_waggon_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last) : waggon_t(welt, maglevwaggon, file, is_first, is_last) {}
	maglev_waggon_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv) : waggon_t(maglevwaggon, pos, besch, sp, cnv) {}
#else
	// all handled by waggon_t
	maglev_waggon_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last) : waggon_t(welt, file, is_first, is_last) {}
	maglev_waggon_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv) : waggon_t(pos, besch, sp, cnv) {}

	typ get_typ() const { return maglevwaggon; }
#endif

	schedule_t * erzeuge_neuen_fahrplan() const;
};



/**
 * very similar to normal railroad, so wie can implement it here completely ...
 * @author prissi
 * @see vehikel_t
 */
class narrowgauge_waggon_t : public waggon_t
{
public:
	virtual waytype_t get_waytype() const { return narrowgauge_wt; }

#ifdef INLINE_DING_TYPE
	// all handled by waggon_t
	narrowgauge_waggon_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last) : waggon_t(welt, narrowgaugewaggon, file, is_first, is_last) {}
	narrowgauge_waggon_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv) : waggon_t(narrowgaugewaggon, pos, besch, sp, cnv) {}
#else
	// all handled by waggon_t
	narrowgauge_waggon_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last) : waggon_t(welt, file, is_first, is_last) {}
	narrowgauge_waggon_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv) : waggon_t(pos, besch, sp, cnv) {}

	typ get_typ() const { return narrowgaugewaggon; }
#endif

	schedule_t * erzeuge_neuen_fahrplan() const;
};



/**
 * Eine Klasse f�r Wasserfahrzeuge. Verwaltet das Aussehen der
 * Fahrzeuge und die Befahrbarkeit des Untergrundes.
 *
 * @author Hj. Malthaner
 * @see vehikel_t
 */
class schiff_t : public vehikel_t
{
protected:
	// how expensive to go here (for way search)
	virtual int get_kosten(const grund_t *, const sint32, koord) { return 1; }

	void calc_drag_coefficient(const grund_t *gr);

	bool ist_befahrbar(const grund_t *bd) const;

public:
	waytype_t get_waytype() const { return water_wt; }

	virtual bool ist_weg_frei(int &restart_speed, bool);

	// returns true for the way search to an unknown target.
	virtual bool ist_ziel(const grund_t *,const grund_t *) {return 0;}

	schiff_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last);
	schiff_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv); // start und fahrplan

#ifdef INLINE_DING_TYPE
#else
	ding_t::typ get_typ() const { return schiff; }
#endif

	schedule_t * erzeuge_neuen_fahrplan() const;

};


/**
 * Eine Klasse f�r Flugzeuge. Verwaltet das Aussehen der
 * Fahrzeuge und die Befahrbarkeit des Untergrundes.
 *
 * @author hsiegeln
 * @see vehikel_t
 */
class aircraft_t : public vehikel_t
{
private:
	// just to mark dirty afterwards
	sint16 old_x, old_y;
	image_id old_bild;

	// only used for ist_ziel() (do not need saving)
	ribi_t::ribi approach_dir;
#ifdef USE_DIFFERENT_WIND
	static uint8 get_approach_ribi( koord3d start, koord3d ziel );
#endif
	//// only used for route search and approach vectors of get_ribi() (do not need saving)
	//koord3d search_start;
	//koord3d search_end;

	enum flight_state { taxiing=0, departing=1, flying=2, landing=3, looking_for_parking=4, circling=5, taxiing_to_halt=6  };

	flight_state state;	// functions needed for the search without destination from find_route

	sint16 flughoehe;
	sint16 target_height;
	uint32 suchen, touchdown, takeoff;

	// BG, 07.08.2012: extracted from calc_route()
	bool calc_route_internal(
		karte_t *welt, 
		const koord3d &start, 
		const koord3d &ziel, 
		sint32 max_speed, 
		uint32 weight, 
		aircraft_t::flight_state &state,
		sint16 &flughoehe, 
		sint16 &target_height,
		bool &runway_too_short,
		uint32 &takeoff, 
		uint32 &touchdown,
		uint32 &suchen, 
		route_t &route);

protected:
	// jumps to next tile and correct the height ...
	grund_t* hop();

	bool ist_befahrbar(const grund_t *bd) const;

	grund_t* betrete_feld();

	int block_reserver( uint32 start, uint32 end, bool reserve ) const;

	// find a route and reserve the stop position
	bool find_route_to_stop_position();

public:
	aircraft_t(karte_t *welt, loadsave_t *file, bool is_first, bool is_last);
	aircraft_t(koord3d pos, const vehikel_besch_t* besch, spieler_t* sp, convoi_t* cnv); // start und fahrplan

	// since we are drawing ourselves, we must mark ourselves dirty during deletion
	virtual ~aircraft_t();

	virtual waytype_t get_waytype() const { return air_wt; }

	// returns true for the way search to an unknown target.
	virtual bool ist_ziel(const grund_t *,const grund_t *);

	//bool can_takeoff_here(const grund_t *gr, ribi_t::ribi test_dir, uint8 len) const;

	// return valid direction
	virtual ribi_t::ribi get_ribi(const grund_t* ) const;

	// how expensive to go here (for way search)
	virtual int get_kosten(const grund_t *, const sint32, koord);

	virtual bool ist_weg_frei(int &restart_speed, bool);

	virtual void set_convoi(convoi_t *c);

	route_t::route_result_t calc_route(koord3d start, koord3d ziel, sint32 max_speed, route_t* route);

	// BG, 08.08.2012: extracted from ist_weg_frei()
    bool reroute(const uint16 reroute_index, const koord3d &ziel);

#ifdef INLINE_DING_TYPE
#else
	typ get_typ() const { return aircraft; }
#endif

	schedule_t * erzeuge_neuen_fahrplan() const;

	void rdwr_from_convoi(loadsave_t *file);

	int get_flyingheight() const {return flughoehe-hoff-2;}

	void force_land() { flughoehe = 0; target_height = 0; state = taxiing_to_halt; }

	// image: when flying empty, on ground the plane
	virtual image_id get_bild() const {return !is_on_ground() ? IMG_LEER : bild;}

	// image: when flying the shadow, on ground empty
	virtual image_id get_outline_bild() const {return !is_on_ground() ? bild : IMG_LEER;}

	// shadow has black color (when flying)
	virtual PLAYER_COLOR_VAL get_outline_colour() const {return !is_on_ground() ? TRANSPARENT75_FLAG | OUTLINE_FLAG | COL_BLACK : 0;}

	// this draws the "real" aircrafts (when flying)
	virtual void display_after(int xpos, int ypos, bool dirty) const;

	// the drag calculation happens it calc_height
	void calc_drag_coefficient(const grund_t*) {}

	bool is_on_ground() const { return flughoehe==0  &&  !(state==circling  ||  state==flying); }

	const char * ist_entfernbar(const spieler_t *sp);

	bool runway_too_short;

	virtual sint32 get_takeoff_route_index() const { return (sint32) takeoff; }
	virtual sint32 get_touchdown_route_index() const { return (sint32) touchdown; }
};

sint16 get_friction_of_waytype(waytype_t waytype);

#endif
