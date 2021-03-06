/*
 *  Copyright (c) 1997 - 2002 by Volker Meyer & Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic licence.
 *
 *  node structure:
 *  0   Name
 *  1   Copyright
 *  2   Bildliste Hintergrund
 *  3   Bildliste Vordergrund
 *  4   cursor(image 0) and icon (image 1)
 *[ 5   Bildliste Hintergrund - snow ] (if present)
 *[ 6   Bildliste Vordergrund - snow ] (if present)
 *[ 7 (or 5 if no snow image) underground way ] (if present)
 */

#ifndef __TUNNEL_BESCH_H
#define __TUNNEL_BESCH_H

#include "../display/simimg.h"
#include "../simtypes.h"
#include "../dataobj/way_constraints.h"
#include "obj_besch_std_name.h"
#include "skin_besch.h"
#include "bildliste2d_besch.h"
#include "weg_besch.h"


class tunnel_besch_t : public obj_besch_transport_infrastructure_t {
	friend class tunnel_reader_t;
	friend class tunnelbauer_t;	// to convert the old tunnels to new ones

private:
	static int hang_indices[81];
	//uint32 max_axle_load; // maximum weight for vehicles. @author: jamespetts


	/* number of seasons (0 = none, 1 = no snow/snow)
	*/
	sint8 number_seasons;

	/*Way constraints for, e.g., loading gauges, types of electrification, etc.
	* @author: jamespetts*/
	way_constraints_of_way_t way_constraints;

	/* has underground way image ? (0 = no, 1 = yes)
	*/
	uint8 has_way;

	/* Has broad portals?
	 */
	uint8 broad_portals;

public:
	const bild_besch_t *get_hintergrund(hang_t::typ hang, uint8 season, uint8 type ) const
	{
		int const n = season && number_seasons == 1 ? 5 : 2;
		return get_child<bildliste_besch_t>(n)->get_bild(hang_indices[hang] + 4 * type);
	}

	image_id get_hintergrund_nr(hang_t::typ hang, uint8 season, uint8 type ) const
	{
		const bild_besch_t *besch = get_hintergrund(hang, season, type );
		return besch != NULL ? besch->get_nummer() : IMG_LEER;
	}

	const bild_besch_t *get_vordergrund(hang_t::typ hang, uint8 season, uint8 type ) const
	{
		int const n = season && number_seasons == 1 ? 6 : 3;
		return get_child<bildliste_besch_t>(n)->get_bild(hang_indices[hang] + 4 * type);
	}

	image_id get_vordergrund_nr(hang_t::typ hang, uint8 season, uint8 type) const
	{
		const bild_besch_t *besch = get_vordergrund(hang, season, type );
		return besch != NULL ? besch->get_nummer() :IMG_LEER;
	}

	skin_besch_t const* get_cursor() const { return get_child<skin_besch_t>(4); }

	waytype_t get_finance_waytype() const;

	uint32  get_max_axle_load() const { return axle_load; }
	
	/* Way constraints: determines whether vehicles
	 * can travel on this way. This method decodes
	 * the byte into bool values. See here for
	 * information on bitwise operations: 
	 * http://www.cprogramming.com/tutorial/bitwise_operators.html
	 * @author: jamespetts
	 * */
	const way_constraints_of_way_t& get_way_constraints() const { return way_constraints; }
	void set_way_constraints(const way_constraints_of_way_t& value) { way_constraints = value; }

	const weg_besch_t *get_weg_besch() const
	{
		if(has_way) {
			return get_child<weg_besch_t>(5 + number_seasons * 2);
		}
		return NULL;
	}

	bool has_broad_portals() const { return (broad_portals != 0); };

	void calc_checksum(checksum_t *chk) const;
};

#endif
