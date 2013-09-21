#ifndef simware_h
#define simware_h

#include "halthandle_t.h"
#include "dataobj/koord.h"
#include "besch/ware_besch.h"

class warenbauer_t;
class karte_t;
class spieler_t;

/** Class to handle goods packets (and their destinations) */
class ware_t
{
	friend class warenbauer_t;

private:
	/// private lookup table to speedup
	static const ware_besch_t *index_to_besch[256];

public:
	/// type of good, used as index into index_to_besch
	uint32 index: 8;

	/// amount of goods
	uint32 menge : 24;

private:
	/**
	 * Handle of target station.
	 * @author Hj. Malthaner
	 */
	halthandle_t ziel;

	/**
	 * Handle of station, where the packet has to leave convoy.
	 * @author Hj. Malthaner
	 */
	halthandle_t zwischenziel;

	/**
	 * A handle to the ultimate origin.
	 * @author: jamespetts
	 */
	halthandle_t origin;

	/**
	 * A handle to the previous transfer
	 * for the purposes of distance calculation
	 * @author: jamespetts, November 2011
	 */
	halthandle_t last_transfer;

	/**
	 * The position of the actual building that we originated at */
	koord origin_pos;
	/**
	 * Target position (factory, etc)
	 * 
	 * "the final target position, which is on behalf 
	 * not the goal stop position"
	 *
	 * @author Hj. Malthaner
	 */
	koord zielpos;

	/**
	 * Update target (zielpos) for factory-going goods (after loading or rotating)
	 */
	void update_factory_target(karte_t *welt);

public:
	const halthandle_t &get_ziel() const { return ziel; }
	void set_ziel(const halthandle_t &ziel) { this->ziel = ziel; }

	const halthandle_t &get_zwischenziel() const { return zwischenziel; }
	halthandle_t &access_zwischenziel() { return zwischenziel; }
	void set_zwischenziel(const halthandle_t &zwischenziel) { this->zwischenziel = zwischenziel; }

	koord get_zielpos() const { return zielpos; }
	void set_zielpos(const koord zielpos) { this->zielpos = zielpos; }

	koord get_origin_pos() const { return origin_pos; }
	void set_origin_pos(const koord zielpos) { this->origin_pos = zielpos; }

	void reset() { menge = 0; ziel = zwischenziel = origin = last_transfer = halthandle_t(); origin_pos = zielpos = koord::invalid; }

	ware_t();
	ware_t(const ware_besch_t *typ);
	ware_t(const ware_besch_t *typ, halthandle_t o, koord pos);
	ware_t(karte_t *welt,loadsave_t *file);

	/**
	 * gibt den nicht-uebersetzten warennamen zur�ck
	 * @author Hj. Malthaner
	 * "There the non-translated names were back"
	 */
	const char *get_name() const { return get_besch()->get_name(); }
	const char *get_mass() const { return get_besch()->get_mass(); }
	uint8 get_catg() const { return get_besch()->get_catg(); }
	uint8 get_index() const { return index; }

	//@author: jamespetts
	halthandle_t get_origin() const { return origin; }
	void set_origin(halthandle_t value) { origin = value; }
	halthandle_t get_last_transfer() const { return last_transfer; }
	void set_last_transfer(halthandle_t value) { last_transfer = value; }

	const ware_besch_t* get_besch() const { return index_to_besch[index]; }
	void set_besch(const ware_besch_t* type);

	void rdwr(karte_t *welt,loadsave_t *file);

	void laden_abschliessen(karte_t *welt,spieler_t *sp);

	// find out the category ...
	bool is_passenger() const { return index == 0 || index == 1; }
	bool is_tourist() const { return index == 0; }
	bool is_commuter() const { return index == 1; }
	bool is_mail() const { return index == 2; }
	bool is_freight() const { return index > 3; }

	// The time at which this packet arrived at the current station
	// @author: jamespetts
	sint64 arrival_time;

	int operator==(const ware_t &w) {
		return	menge == w.menge &&
			zwischenziel == w.zwischenziel &&
			arrival_time == w.arrival_time &&
			index  == w.index  &&
			ziel  == w.ziel  &&
			zielpos == w.zielpos &&
			origin == w.origin && 
			last_transfer == w.last_transfer;
	}

	int operator!=(const ware_t &w) { return !(*this == w); 	}

	/**
	 * Adjust target coordinates.
	 * Must be called after factories have been rotated!
	 */
	void rotate90( karte_t *welt, sint16 y_size );
};

#endif
