/*
 * Copyright (c) 1997 - 2001 Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic licence.
 * (see licence.txt)
 */

#ifndef dings_gebaeude_h
#define dings_gebaeude_h

#include "../ifc/sync_steppable.h"
#include "../simdings.h"
#include "../simcolor.h"

class haus_tile_besch_t;
class fabrik_t;
class stadt_t;

/**
 * Asynchron oder synchron animierte Gebaeude f�r Simutrans.
 * @author Hj. Malthaner
 */
class gebaeude_t : public ding_t, sync_steppable
{
public:
	/**
	 * Vom typ "unbekannt" sind auch spezielle gebaeude z.B. das Rathaus
	 * "Of type "unknown" are also special gebaeude eg City Hall" (Google)
	 * residential, commercial, industrial, unknown
	 * @author Hj. Malthaner
	 */
	enum typ {wohnung, gewerbe, industrie, unbekannt};

private:
	const haus_tile_besch_t *tile;

	/**
	* either point to a factory or a city
	* @author Hj. Malthaner
	*/
	union {
		fabrik_t  *fab;
		stadt_t *stadt;
	} ptr;

	/**
	 * Zeitpunkt an dem das Gebaeude Gebaut wurde
	 * "Time at that was built the building" (Babelfish)
	 * @author Hj. Malthaner
	 */
	sint64 insta_zeit;

	/**
	 * Time control for animation progress.
	 * @author Hj. Malthaner
	 */
	uint16 anim_time;

	/**
	 * Current anim frame
	 * @author Hj. Malthaner
	 */
	uint8 count;

	/**
	 * Is this a sync animated object?
	 * @author Hj. Malthaner
	 */
	uint8 sync:1;

	/**
	 * Boolean flag if a construction site or buildings image
	 * shall be displayed.
	 * @author Hj. Malthaner
	 */
	uint8 zeige_baugrube:1;

	/**
	 * if true, this ptr union contains a factory pointer
	 * ? Surely, this cannot be right with an 8 bit integer? Out of date comment?
	 * @author Hj. Malthaner
	 */
	uint8 is_factory:1;
	/**
	 * if true show snow image
	 */
	bool snow:1;

	/* true if ground image can go */
	bool remove_ground:1;

	/**
	 * Initializes all variables with save, usable values
	 * @author Hj. Malthaner
	 */
	void init();

	/** 
	 * Stores the dynamic population of this building:
	 * either as visitor demand (commercial) or population
	 * (residential). This is the fully adjusted figure, 
	 * representing passengers to be generated or demanded 
	 * per game month.
	 */
	union people_t
	{
		uint16 population;
		uint16 visitor_demand;
	} people;

	/** 
	 * Stores the dynamic number of jobs in this building
	 * at present. By default, jobs == jobs_per_workday.
	 */
	uint16 jobs;
	uint32 jobs_per_workday;

	/** 
	 * Stores the dynamic level of mail demand in this building
	 * at present. This is the fully adjusted figure, 
	 * representing packets of mail to be generated or demanded 
	 * per game month.
	 */
	uint16 mail_demand;
	uint16 adjusted_mail_demand;

	/** The following variables record the proportion of
	 * successful passenger trips generated by this building
	 * in the current and last year respectively.
	 */
	uint16 passengers_generated_local;
	uint16 passengers_succeeded_local;
	uint8 passenger_success_percent_last_year_local;

	uint16 passengers_generated_non_local;
	uint16 passengers_succeeded_non_local;
	uint8 passenger_success_percent_last_year_non_local;

	/**
	* This is the number of jobs supplied by this building
	* multiplied by the number of ticks per month, subtracted
	* from the creation time, to which is added the number
	* of ticks per month whenever a commuter reaches this
	* destination. Further, this value is set so that, 
	* whenever a number is added to it, it will never be less
	* than that number plus the number of ticks per month
	* multiplied by the number of available jobs minus
	* the current time. This is intended to prevent more
	* commuters going to this building each month than there
	* are jobs available for them. 
	* @author: jamespetts
	*/
	sint64 available_jobs_by_time;

	uint16 commuters_in_transit;

#ifdef INLINE_DING_TYPE
protected:
	gebaeude_t(karte_t *welt, ding_t::typ type);
	gebaeude_t(karte_t *welt, ding_t::typ type, koord3d pos,spieler_t *sp, const haus_tile_besch_t *t);
	void init(spieler_t *sp, const haus_tile_besch_t *t);

public:
	gebaeude_t(karte_t *welt, loadsave_t *file);
	gebaeude_t(karte_t *welt, koord3d pos,spieler_t *sp, const haus_tile_besch_t *t);
#else
protected:
	gebaeude_t(karte_t *welt);

public:
	gebaeude_t(karte_t *welt, loadsave_t *file);
	gebaeude_t(karte_t *welt, koord3d pos,spieler_t *sp, const haus_tile_besch_t *t);
#endif
	virtual ~gebaeude_t();

	void rotate90();

	typ get_haustyp() const;

	void add_alter(sint64 a);

	void set_fab(fabrik_t *fb);
	void set_stadt(stadt_t *s);

	/**
	 * Ein Gebaeude kann zu einer Fabrik geh�ren.
	 * @return Einen Zeiger auf die Fabrik zu der das Objekt geh�rt oder NULL,
	 * wenn das Objekt zu keiner Fabrik geh�rt.
	 *
	 * A building can belong to a factory. 
	 * return a pointer on the factory to that the object belongs or NULL,
	 * if the object belongs to no factory. (Google)
	 *
	 * @author Hj. Malthaner
	 */
	fabrik_t* get_fabrik() const { return is_factory ? ptr.fab : NULL; }
	stadt_t* get_stadt() const { return is_factory ? NULL : ptr.stadt; }

#ifdef INLINE_DING_TYPE
#else
	ding_t::typ get_typ() const { return ding_t::gebaeude; }
#endif

	/**
	 * waytype associated with this object
	 */
	waytype_t get_waytype() const;

	// snowline height may have been changed
	bool check_season(const long /*month*/) { calc_bild(); return true; }

	image_id get_bild() const;
	image_id get_bild(int nr) const;
	image_id get_after_bild() const;
	void mark_images_dirty() const;

	image_id get_outline_bild() const;
	PLAYER_COLOR_VAL get_outline_colour() const;

	// caches image at height 0
	void calc_bild();

	/**
	 * @return eigener Name oder Name der Fabrik falls Teil einer Fabrik
	 * @author Hj. Malthaner
	 */
	virtual const char *get_name() const;

	void get_description(cbuffer_t & buf) const;

	/**
	* Town hall
	*/
	bool ist_rathaus() const;

	/**
	* "Head office" (Google)
	*/
	bool ist_firmensitz() const;

	bool is_monument() const;

	bool is_attraction() const;

	/**
	 * @return Einen Beschreibungsstring f�r das Objekt, der z.B. in einem
	 * Beobachtungsfenster angezeigt wird.
	 * @author Hj. Malthaner
	 */
	void info(cbuffer_t & buf, bool dummy = false) const;

	void rdwr(loadsave_t *file);

	/**
	 * Methode f�r Echtzeitfunktionen eines Objekts. Spielt animation.
	 * @return true
	 * @author Hj. Malthaner
	 */
	bool sync_step(long delta_t);

	/**
	 * @return Den level (die Ausbaustufe) des Gebaudes
	 * @author Hj. Malthaner
	 */
	int get_passagier_level() const;

	int get_post_level() const; 

	void set_tile( const haus_tile_besch_t *t, bool start_with_construction );

	const haus_tile_besch_t *get_tile() const { return tile; }

	virtual void zeige_info();
	void zeige_info2();

	void entferne(spieler_t *sp);

	void laden_abschliessen();

	/**
	 * @returns pointer to first tile of a multi-tile building.
	 */
	gebaeude_t* get_first_tile();

	void add_passengers_generated_local(uint16 number) { passengers_generated_local += number; }
	void add_passengers_succeeded_local(uint16 number) { passengers_succeeded_local += number; }

	uint16 get_passenger_success_percent_this_year_local() const { return passengers_generated_local > 0 ? (passengers_succeeded_local * 100) / passengers_generated_local : 0; }
	uint16 get_passenger_success_percent_last_year_local() const { return passenger_success_percent_last_year_local; }
	uint16 get_average_passenger_success_percent_local() const { return (get_passenger_success_percent_this_year_local() + passenger_success_percent_last_year_local) / 2; }

	void add_passengers_generated_non_local(uint16 number) { passengers_generated_non_local += number; }
	void add_passengers_succeeded_non_local(uint16 number) { passengers_succeeded_non_local += number; }

	uint16 get_passenger_success_percent_this_year_non_local() const { return passengers_generated_non_local > 0 ? (passengers_succeeded_non_local * 100) / passengers_generated_non_local : 0; }
	uint16 get_passenger_success_percent_last_year_non_local() const { return passenger_success_percent_last_year_non_local; }
	uint16 get_average_passenger_success_percent_non_local() const { return (get_passenger_success_percent_this_year_non_local() + passenger_success_percent_last_year_non_local) / 2; }

	void new_year();

	void check_road_tiles(bool del);

	uint16 get_weight() const;

	bool get_is_factory() const { return is_factory; }

	/**
	* Call this method when commuting passengers are sent to this building.
	* Pass the number of passengers being sent.
	* @author: jamespetts, August 2013
	*/
	void commuters_arrived(uint16 number);
	void commuters_departed(uint16 number);
	void commuters_destroyed(uint16 number);

	uint16 get_population() const;
	uint16 get_passenger_trips_per_workday() const;

	uint16 get_visitor_demand() const;
	uint16 get_visitor_demand_per_workday() const;

	uint16 get_jobs() const;
	uint32 get_jobs_per_workday() const;

	uint16 get_mail_demand() const;
	uint16 get_mail_demand_per_workday() const;

	bool jobs_available() const;

private:
	sint64 calc_available_jobs_by_time() const;

	/**
	* Returns the number of jobs left in this building this month.
	* Note: this is measured in *adjusted* jobs.
	*/
	sint32 check_remaining_available_jobs() const;
	
	void commuters_changed();
};


template<> inline gebaeude_t* ding_cast<gebaeude_t>(ding_t* const d)
{
	return dynamic_cast<gebaeude_t*>(d);
}

#endif
