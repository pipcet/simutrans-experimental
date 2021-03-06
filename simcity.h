/*
 * Copyright (c) 1997 - 2001 Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic license.
 * (see license.txt)
 */

#ifndef simcity_h
#define simcity_h

#include "dataobj/ribi.h"

#include "simobj.h"
#include "obj/gebaeude.h"

#include "tpl/vector_tpl.h"
#include "tpl/weighted_vector_tpl.h"
#include "tpl/array2d_tpl.h"
#include "tpl/slist_tpl.h"
#include "tpl/koordhashtable_tpl.h"

#include "vehicle/simverkehr.h"
#include "tpl/sparse_tpl.h"
#include "utils/plainstring.h"

#include <string>

class karte_ptr_t;
class spieler_t;
class fabrik_t;
class rule_t;

// For private subroutines
class haus_besch_t;

// part of passengers going to factories or toursit attractions (100% mx)
#define FACTORY_PAX (33)	// workers
#define TOURIST_PAX (16)		// tourists

#define MAX_CITY_HISTORY_YEARS  (12) // number of years to keep history
#define MAX_CITY_HISTORY_MONTHS (12) // number of months to keep history

#define PAX_DESTINATIONS_SIZE (256) // size of the minimap in the city window (sparse array)

enum city_cost {
	HIST_CITICENS=0,		// total people
	HIST_JOBS,				// Total jobs
	HIST_VISITOR_DEMAND,	// Total visitor demand
	HIST_GROWTH,			// growth (just for convenience)
	HIST_BUILDING,			// number of buildings
	HIST_CITYCARS,			// Amount of private traffic produced by the city
	HIST_PAS_TRANSPORTED,	// number of passengers who could start their journey
	HIST_PAS_GENERATED,		// total number generated
	HIST_PAS_WALKED,		// The number of passengers who walked to their destination.
	HIST_MAIL_TRANSPORTED,	// letters that could be sent
	HIST_MAIL_GENERATED,	// all letters generated
	HIST_GOODS_RECIEVED,	// times all storages were not empty
	HIST_GOODS_NEEDED,		// times sotrages checked
	HIST_POWER_RECIEVED,	// power consumption 
	HIST_POWER_NEEDED,		// Power demand by the city.
	HIST_CONGESTION,		// Level of congestion in the city, expressed in percent.
	MAX_CITY_HISTORY		// Total number of items in array
};

#define LEGACY_HIST_CAR_OWNERSHIP (16)

class private_car_destination_finder_t : public fahrer_t
{
private:
	karte_t* welt;
	automobil_t *master;
	stadt_t* origin_city;
	const stadt_t* last_city;
	uint32 last_tile_speed;
	int last_tile_cost_diagonal;
	int last_tile_cost_straight;
	uint16 meters_per_tile_x100;

public:
	private_car_destination_finder_t(karte_t* w, automobil_t* m, stadt_t* o);	
	
	virtual waytype_t get_waytype() const { return road_wt; };
	virtual bool ist_befahrbar( const grund_t* gr ) const;

	virtual bool ist_ziel(const grund_t* gr, const grund_t*);

	virtual ribi_t::ribi get_ribi( const grund_t* gr) const;

	virtual int get_kosten(const grund_t* gr, const sint32 max_speed, koord from_pos);
};

/**
 * Die Objecte der Klasse stadt_t bilden die Staedte in Simu. Sie
 * wachsen automatisch.
 * @author Hj. Malthaner
 */
class stadt_t
{
	/**
	* best_t:
	*
	* Kleine Hilfsklasse - speichert die beste Bewertung einer Position.
	*
	* "Small helper class - saves the best assessment of a position." (Google)
	*
	* @author V. Meyer
	*/
	class best_t {
		sint8 best_wert;
		koord best_pos;
	public:
		void reset(koord pos) { best_wert = 0; best_pos = pos; }

		void check(koord pos, sint8 wert) {
			if(wert > best_wert) {
				best_wert = wert;
				best_pos = pos;
			}
		}

		bool found() const { return best_wert > 0; }

		koord get_pos() const { return best_pos;}
	// sint8 get_wert() const { return best_wert; }
	};

public:
	/**
	 * Reads city configuration data from config/cityrules.tab
	 * @author Hj. Malthaner
	 */
	static bool cityrules_init(const std::string &objpathname);
	uint16 get_electricity_consumption(sint32 monthyear) const;
	static void electricity_consumption_init(const std::string &objfilename);

	/**
	 * Reads/writes city configuration data from/to a savegame
	 * called from settings_t::rdwr
	 * only written for networkgames
	 * @author Dwachs
	 */
	static void cityrules_rdwr(loadsave_t *file);
	static void electricity_consumption_rdwr(loadsave_t *file);
	void set_check_road_connexions(bool value) { check_road_connexions = value; }

	static void set_cluster_factor( uint32 factor ) { stadt_t::cluster_factor = factor; }
	static uint32 get_cluster_factor() { return stadt_t::cluster_factor; }

	void set_private_car_trip(int passengers, stadt_t* destination_town);

private:
	static karte_ptr_t welt;
	spieler_t *besitzer_p;
	plainstring name;

	weighted_vector_tpl <gebaeude_t *> buildings;

	sparse_tpl<uint8> pax_destinations_old;
	sparse_tpl<uint8> pax_destinations_new;

	// this counter will increment by one for every change => dialogs can question, if they need to update map
	unsigned long pax_destinations_new_change;

	koord pos;				// Gruendungsplanquadrat der Stadt ("founding grid square" - Google)
	koord townhall_road;	// road in front of townhall
	koord lo, ur;			// max size of housing area

	bool allow_citygrowth;	// Whether growth is permitted (true by default)

	/**
	 * in this fixed interval, construction will happen
	 */
	static const uint32 city_growth_step;

	/**
	 * When to do growth next
	 * @author Hj. Malthaner
	 */
	uint32 next_growth_step;

	static uint32 cluster_factor;

	// population statistics
	sint32 bev; // total population
	sint32 arb; // amount with jobs
	sint32 won; // amount with homes

	/**
	 * Modifier for city growth
	 * transient data, not saved
	 * Unsupplied city growth needs
	 * A value of 2^30 means 1 new resident
	 * @author Hj. Malthaner
	 */
	sint32 wachstum;

	/**
	* City history
	* @author prissi
	*/
	sint64 city_history_year[MAX_CITY_HISTORY_YEARS][MAX_CITY_HISTORY];
	sint64 city_history_month[MAX_CITY_HISTORY_MONTHS][MAX_CITY_HISTORY];

	/* updates the city history
	* @author prissi
	*/
	void roll_history();

	// This is needed to prevent double counting of incoming traffic.
	sint32 incoming_private_cars;
	
	//This is needed because outgoing cars are disregarded when calculating growth.
	sint32 outgoing_private_cars;

	// The factories that are *inside* the city limits.
	// Needed for power consumption of such factories.
	vector_tpl<fabrik_t *> city_factories;

	// Hashtable of all cities/attractions/industries connected by road from this city.
	// Key: city (etc.) location
	// Value: journey time per tile (equiv. straight line distance)
	// (in 10ths of minutes); 65535 = unreachable.
	// @author: jamespetts, April 2010, modified December 2010 to koords rather than poiners
	// so as to be network safe
	typedef koordhashtable_tpl<koord, uint16> connexion_map;
	connexion_map connected_cities;
	connexion_map connected_industries;
	connexion_map connected_attractions;

	vector_tpl<senke_t*> substations;

	sint32 number_of_cars;

	/**
	* Will fill the world's hashtable of tiles
	* belonging to cities with all the tiles of
	* this city
	*/
	void check_city_tiles(bool del = false);

public:

	void add_building_to_list(gebaeude_t* building, bool ordered = false);

	/**
	 * Returns pointer to history for city
	 * @author hsiegeln
	 */
	sint64* get_city_history_year() { return *city_history_year; }
	sint64* get_city_history_month() { return *city_history_month; }

	// just needed by stadt_info.cc
	static inline karte_t* get_welt() { return welt; }

	uint32 stadtinfo_options;

	void set_private_car_trips(uint16 number) 
	{
		// Do not add to the city's history here, as this 
		// will distort the statistics in the city window
		// for the number of people who have travelled by
		// private car *from* the city.
		incoming_private_cars += number;
	}

	inline void add_transported_passengers(uint16 passengers)
	{
		city_history_year[0][HIST_PAS_TRANSPORTED] += passengers;
		city_history_month[0][HIST_PAS_TRANSPORTED] += passengers;
	}

	inline void add_walking_passengers(uint16 passengers)
	{
		city_history_year[0][HIST_PAS_WALKED] += passengers;
		city_history_month[0][HIST_PAS_WALKED] += passengers;
	}

	inline void add_transported_mail(uint16 mail)
	{
		city_history_year[0][HIST_MAIL_TRANSPORTED] += mail;
		city_history_month[0][HIST_MAIL_TRANSPORTED] += mail;
	}

	//@author: jamespetts
	void add_power(uint32 p) { city_history_month[0][HIST_POWER_RECIEVED] += p; city_history_year[0][HIST_POWER_RECIEVED] += p; }

	void add_power_demand(uint32 p) { city_history_month[0][HIST_POWER_NEEDED] += p; city_history_year[0][HIST_POWER_NEEDED] += p; }

	void add_all_buildings_to_world_list();

	/* end of history related thingies */
private:
	sint32 best_haus_wert;
	sint32 best_strasse_wert;

	best_t best_haus;
	best_t best_strasse;

public:
	/**
 	 * recalcs city borders (after loading old files, after house deletion, after house construction)
	 */
	void reset_city_borders();

private:

	/**
	 * Enlarges city borders (after being unable to build a building, before trying again)
	 * Returns false if there are other cities on all four sides
	 */
	bool enlarge_city_borders();
	/**
	 * Enlarges city borders in a particular direction (N,S,E, or W)
	 * Returns false if it can't
	 */
	bool enlarge_city_borders(ribi_t::ribi direction);

	// calculates the growth rate for next growth_interval using all the different indicators
	void calc_growth();

	/**
	 * Build new buildings when growing city
	 * @author Hj. Malthaner
	 */
	void step_grow_city(bool new_town = false);

	enum pax_return_type { no_return, factory_return, tourist_return, city_return };

	/**
	 * baut Spezialgebaeude, z.B Stadion
	 * @author Hj. Malthaner
	 */
	void check_bau_spezial(bool);

	/**
	 * baut ein angemessenes Rathaus
	 * @author V. Meyer
	 */
	void check_bau_rathaus(bool);

	/**
	 * constructs a new consumer
	 * @author prissi
	 */
	void check_bau_factory(bool);

	// bewertungsfunktionen fuer den Hauserbau
	// wie gut passt so ein Gebaeudetyp an diese Stelle ?
	gebaeude_t::typ was_ist_an(koord pos) const;

	// find out, what building matches best
	void bewerte_res_com_ind(const koord pos, int &ind, int &com, int &res);

	/**
	 * Build/renovates a city building at Planquadrat x,y
	 */
	void build_city_building(koord pos, bool new_town);
	bool renovate_city_building(gebaeude_t *gb);
	// Subroutines for build_city_building and renovate_city_buiding
	// @author neroden
	const gebaeude_t* get_citybuilding_at(const koord k) const;
	int get_best_layout(const haus_besch_t* h, const koord & k) const;

	/**
	 * Build a short road bridge extending from bd in direction.
	 *
	 * @author neroden
	 */
	bool build_bridge(grund_t* bd, ribi_t::ribi direction);

	/**
	 * baut ein Stueck Strasse
	 *
	 * @param k         Bauposition
	 *
	 * @author Hj. Malthaner, V. Meyer
	 */
	bool maybe_build_road(koord k);
	bool baue_strasse(const koord k, spieler_t *sp, bool forced);

	void baue(bool new_town);

	/**
	 * @param pos position to check
	 * @param regel the rule to evaluate
	 * @return true on match, false otherwise
	 * @author Hj. Malthaner
	 */

	bool bewerte_loc_has_public_road(koord pos);
	bool bewerte_loc(koord pos, const rule_t &regel, int rotation);


	/*
	 * evaluates the location, tests again all rules, and caches the result
	 */
	uint16 bewerte_loc_cache(const koord pos, bool force=false);

	/**
	 * Check rule in all transformations at given position
	 * @author Hj. Malthaner
	 */

	sint32 bewerte_pos(koord pos, const rule_t &regel);

	void bewerte_strasse(koord pos, sint32 rd, const rule_t &regel);
	void bewerte_haus(koord pos, sint32 rd, const rule_t &regel);

	bool check_road_connexions;

	sint32 traffic_level;
	void calc_traffic_level();

public:

	/**
	 * ein Passagierziel in die Zielkarte eintragen
	 * @author Hj. Malthaner
	 */
	void merke_passagier_ziel(koord ziel, uint8 color);

	// this function removes houses from the city house list
	// (called when removed by player, or by town)
	void remove_gebaeude_from_stadt(gebaeude_t *gb);

	// This is necessary to be separate from add/remove gebaeude_to_stadt
	// because of the need for the present to retain the existing pattern
	// of what sorts of buildings are added to the city list.
	void update_city_stats_with_building(gebaeude_t* building, bool remove);

	/**
	* This function adds buildings to the city building list; 
	* ordered for multithreaded loading.
	*/
	void add_gebaeude_to_stadt(gebaeude_t *gb, bool ordered=false);

	/**
	* Returns the finance history for cities
	* @author hsiegeln
	*/
	sint64 get_finance_history_year(int year, int type) { return city_history_year[year][type]; }
	sint64 get_finance_history_month(int month, int type) { return city_history_month[month][type]; }

	/**
	* Adds passengers or mail to the number generated by this city.
	* Used for return journies
	*/
	void set_generated_passengers(uint32 number, int type) { city_history_year[0][type] += number; city_history_month[0][type] += number; }

	// growth number (smoothed!)
	sint32 get_wachstum() const {return ((sint32)city_history_month[0][HIST_GROWTH]*5) + (sint32)(city_history_month[1][HIST_GROWTH]*4) + (sint32)city_history_month[2][HIST_GROWTH]; }

	/**
	 * ermittelt die Einwohnerzahl der Stadt
	 * "determines the population of the city"
	 * @author Hj. Malthaner
	 */
	//sint32 get_einwohner() const {return (buildings.get_sum_weight()*6)+((2*bev-arb-won)>>1);}
	sint32 get_einwohner() const {return ((buildings.get_sum_weight() * welt->get_settings().get_meters_per_tile()) / 31)+((2*bev-arb-won)>>1);}
	//sint32 get_einwohner() const { return bev; }

	// Not suitable for use in game computations because this is not network safe. For GUI only.
	double get_land_area() const;

	// Not suitable for use in game computations because this is not network safe. For GUI only.
	uint32 get_population_density() const
	{
		return city_history_month[0][HIST_CITICENS] / get_land_area();
	}

	sint32 get_city_population() const { return (sint32) city_history_month[0][HIST_CITICENS]; }
	sint32 get_city_jobs() const { return (sint32) city_history_month[0][HIST_JOBS]; }
	sint32 get_city_visitor_demand() const { return (sint32) city_history_month[0][HIST_VISITOR_DEMAND]; }

	uint32 get_buildings()  const { return buildings.get_count(); }
	sint32 get_unemployed() const { return bev - arb; }
	sint32 get_homeless()   const { return bev - won; }

	/**
	 * Gibt den Namen der Stadt zur�ck.
	 * "Specifies the name of the town." (Google)
	 * @author Hj. Malthaner
	 */
	const char *get_name() const { return name; }

	/**
	 * Erm�glicht Zugriff auf Namesnarray
	 * @author Hj. Malthaner
	 */
	void set_name( const char *name );

	/**
	 * gibt einen zuf�llingen gleichverteilten Punkt innerhalb der
	 * Stadtgrenzen zur�ck
	 * @author Hj. Malthaner
	 */
	koord get_zufallspunkt(uint32 min_distance = 0, uint32 max_distance = 16384, koord origin = koord::invalid) const;

	/**
	 * gibt das pax-statistik-array f�r letzten monat zur�ck
	 * @author Hj. Malthaner
	 */
	const sparse_tpl<unsigned char>* get_pax_destinations_old() const { return &pax_destinations_old; }

	/**
	 * gibt das pax-statistik-array f�r den aktuellen monat zur�ck
	 * @author Hj. Malthaner
	 */
	const sparse_tpl<unsigned char>* get_pax_destinations_new() const { return &pax_destinations_new; }

	/* this counter will increment by one for every change
	 * => dialogs can question, if they need to update map
	 * @author prissi
	 */
	unsigned long get_pax_destinations_new_change() const { return pax_destinations_new_change; }

	/**
	 * Erzeugt eine neue Stadt auf Planquadrat (x,y) die dem Spieler sp
	 * gehoert.
	 * @param sp Der Besitzer der Stadt.
	 * @param x x-Planquadratkoordinate
	 * @param y y-Planquadratkoordinate
	 * @param number of citizens
	 * @author Hj. Malthaner
	 */
	stadt_t(spieler_t* sp, koord pos, sint32 citizens);

	/**
	 * Erzeugt eine neue Stadt nach Angaben aus der Datei file.
	 * @param welt Die Karte zu der die Stadt gehoeren soll.
	 * @param file Zeiger auf die Datei mit den Stadtbaudaten.
	 * @see stadt_t::speichern()
	 * @author Hj. Malthaner
	 */
	stadt_t(loadsave_t *file);

	// closes window and that stuff
	~stadt_t();

	/**
	 * Speichert die Daten der Stadt in der Datei file so, dass daraus
	 * die Stadt wieder erzeugt werden kann. Die Gebaude und strassen der
	 * Stadt werden nicht mit der Stadt gespeichert sondern mit den
	 * Planquadraten auf denen sie stehen.
	 * @see stadt_t::stadt_t()
	 * @see planquadrat_t
	 * @author Hj. Malthaner
	 */
	void rdwr(loadsave_t *file);

	/**
	 * Wird am Ende der LAderoutine aufgerufen, wenn die Welt geladen ist
	 * und nur noch die Datenstrukturenneu verkn�pft werden m�ssen.
	 * @author Hj. Malthaner
	 */
	void laden_abschliessen();

	void rotate90( const sint16 y_size );

	/* change size of city
	* @author prissi */
	void change_size( sint32 delta_citizens, bool new_town = false );

	// when ng is false, no town growth any more
	void set_citygrowth_yesno( bool ng ) { allow_citygrowth = ng; }
	bool get_citygrowth() const { return allow_citygrowth; }

	void step(long delta_t);

	void neuer_monat(bool check);

	void add_road_connexion(uint16 journey_time_per_tile, const stadt_t* city);
	void add_road_connexion(uint16 journey_time_per_tile, const fabrik_t* industry);
	void add_road_connexion(uint16 journey_time_per_tile, const gebaeude_t* attraction);

	void check_all_private_car_routes();

	// Checks to see whether this town is connected
	// by road to each other town.
	// @author: jamespetts, April 2010
	uint16 check_road_connexion_to(stadt_t* city);
	uint16 check_road_connexion_to(const fabrik_t* industry);
	uint16 check_road_connexion_to(const gebaeude_t* attraction);

	void erzeuge_verkehrsteilnehmer(koord pos, uint16 journey_tenths_of_minutes, koord target, uint8 number_of_passengers);

private:
	/**
	 * A weighted list of distances
	 * @author Knightly 
	 */
	static weighted_vector_tpl<uint32> distances;

	/**
	 * Record of a target city
	 * @author Knightly
	 */
	struct target_city_t
	{
		stadt_t *city;
		uint32 distance;

		target_city_t() : city(NULL), distance(0) { }
		target_city_t(stadt_t *const _city, const uint32 _distance) : city(_city), distance(_distance) { }

		bool operator == (const target_city_t &other) const { return city == other.city; }

		static bool less_than_or_equal(const target_city_t &a, const target_city_t &b) { return a.distance <= b.distance; }
		static bool less_than(const target_city_t &a, const target_city_t &b) { return a.distance < b.distance; }
	};


public:

	/**
	 * Gibt die Gruendungsposition der Stadt zurueck.
	 * @return die Koordinaten des Gruendungsplanquadrates
	 * "eturn the coordinates of the establishment grid square" (Babelfish)
	 * @author Hj. Malthaner
	 */
	inline koord get_pos() const {return pos;}
	inline koord get_townhall_road() const {return townhall_road;}

	inline koord get_linksoben() const { return lo;} // "Top left" (Google)
	inline koord get_rechtsunten() const { return ur;} // "Bottom right" (Google)

	koord get_center() const { return (lo+ur)/2; }

	// Checks whether any given postition is within the city limits.
	bool is_within_city_limits(koord k) const;

	/**
	 * Erzeugt ein Array zufaelliger Startkoordinaten,
	 * die fuer eine Stadtgruendung geeignet sind.
	 * @param wl Die Karte auf der die Stadt gegruendet werden soll.
	 * @param anzahl die Anzahl der zu liefernden Koordinaten
	 * @author Hj. Malthaner
	 * @param old_x, old_y: Generate no cities in (0,0) - (old_x, old_y)
	 * @author Gerd Wachsmuth
	 */

	static vector_tpl<koord> *random_place(const karte_t *wl, const vector_tpl<sint32> *sizes_list, sint16 old_x, sint16 old_y);
	// geeigneten platz zur Stadtgruendung durch Zufall ermitteln

	void zeige_info();

	void add_factory_arbeiterziel(fabrik_t *fab);

	uint8 get_congestion() const { return (uint8) city_history_month[0][HIST_CONGESTION]; }

	void add_city_factory(fabrik_t *fab) { city_factories.append_unique(fab); }
	void remove_city_factory(fabrik_t *fab) { city_factories.remove(fab); }
	const vector_tpl<fabrik_t*>& get_city_factories() const { return city_factories; }

	uint32 get_power_demand() const;

	void add_substation(senke_t* substation);
	void remove_substation(senke_t* substation);
	vector_tpl<senke_t*>* get_substations() { return &substations; }

	/**
	* These methods are used for removing a connected city (etc.)
	* from the list when these objects are deleted, to prevent 
	* acces violations.
	*/
	void remove_connected_city(stadt_t* city);
	void remove_connected_industry(fabrik_t* fab);
	void remove_connected_attraction(gebaeude_t* attraction);

	// @author: jamespetts
	// September 2010
	uint16 get_max_dimension();
};

#endif
