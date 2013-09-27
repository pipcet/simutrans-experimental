/*
 * Copyright (c) 1997 - 2001 Hj. Malthaner
 *
 * This file is part of the Simutrans project under the artistic license.
 * (see license.txt)
 */

#include <stdio.h>
#include <string.h>

#include "simmem.h"
#include "simdebug.h"
#include "simfab.h"
#include "simhalt.h"
#include "simtypes.h"
#include "simware.h"
#include "dataobj/loadsave.h"
#include "dataobj/koord.h"

#include "besch/ware_besch.h"
#include "bauer/warenbauer.h"



const ware_besch_t *ware_t::index_to_besch[256];



ware_t::ware_t() : ziel(), zwischenziel(), zielpos(-1, -1)
{
	menge = 0;
	index = 0;
	arrival_time = 0;
	origin_pos = koord::invalid;
}


ware_t::ware_t(const ware_besch_t *wtyp) : ziel(), zwischenziel(), zielpos(-1, -1)
{
	//This constructor is called from simcity.cc
	menge = 0;
	index = wtyp->get_index();
	origin_pos = koord::invalid;
	arrival_time = 0;
}

// Constructor for new revenue system: packet of cargo keeps track of its origin.
//@author: jamespetts
ware_t::ware_t(const ware_besch_t *wtyp, halthandle_t o, koord pos) : ziel(), zwischenziel(), zielpos(-1, -1)
{
	menge = 0;
	index = wtyp->get_index();
	origin = o;
	origin_pos = pos;
	arrival_time = 0;
}


ware_t::ware_t(karte_t *welt,loadsave_t *file)
{
	rdwr(welt,file);
}


void ware_t::set_besch(const ware_besch_t* type)
{
	index = type->get_index();
}



void ware_t::rdwr(karte_t *welt,loadsave_t *file)
{
	sint32 amount = menge;
	file->rdwr_long(amount);
	menge = amount;
	if(file->get_version()<99008) {
		sint32 max;
		file->rdwr_long(max);
	}

	if(file->get_version()>=110005 && file->get_experimental_version() < 12) 
	{
		// Was "to_factory" / "factory_going".
		uint8 dummy;
		file->rdwr_byte(dummy);
	}

	uint8 catg=0;
	if(file->get_version()>=88005) {
		file->rdwr_byte(catg);
	}

	if(file->is_saving()) {
		const char *typ = NULL;
		typ = get_besch()->get_name();
		file->rdwr_str(typ);
	}
	else {
		char typ[256];
		file->rdwr_str(typ, lengthof(typ));
		const ware_besch_t *type = warenbauer_t::get_info(typ);
		if(type==NULL) {
			dbg->warning("ware_t::rdwr()","unknown ware of catg %d!",catg);
			index = warenbauer_t::get_info_catg(catg)->get_index();
			menge = 0;
		}
		else {
			index = type->get_index();
		}
	}
	// convert coordinate to halt indices
	if(file->get_version() > 110005 && (file->get_experimental_version() >= 10 || file->get_experimental_version() == 0))
	{
		// save halt id directly
		if(file->is_saving()) 
		{
			uint16 halt_id = ziel.is_bound() ? ziel.get_id() : 0;
			file->rdwr_short(halt_id);
			halt_id = zwischenziel.is_bound() ? zwischenziel.get_id() : 0;
			file->rdwr_short(halt_id);
			if(file->get_experimental_version() >= 1)
			{
				halt_id = origin.is_bound() ? origin.get_id() : 0;	
				file->rdwr_short(halt_id);
			}	
			if(file->get_experimental_version() >= 12)
			{
				origin_pos.rdwr(file);
			}
		}

		else
		{
			uint16 halt_id;
			file->rdwr_short(halt_id);
			ziel.set_id(halt_id);
			file->rdwr_short(halt_id);
			zwischenziel.set_id(halt_id);
			if(file->get_experimental_version() >= 1)
			{
				file->rdwr_short(halt_id);			
				origin.set_id(halt_id);
			}
			else
			{
				origin = zwischenziel;
			}

			if(file->get_experimental_version() >= 12)
			{
				origin_pos.rdwr(file);
			}
		}
	}
	else 
	{
		if(file->is_saving()) 
		{
			koord ziel_koord = ziel.is_bound() ? ziel->get_basis_pos() : koord::invalid;
			ziel_koord.rdwr(file);
			koord zwischenziel_koord = zwischenziel.is_bound() ? zwischenziel->get_basis_pos() : koord::invalid;
			zwischenziel_koord.rdwr(file);
			if(file->get_experimental_version() >= 1)
			{
				koord origin_koord = origin.is_bound() ? origin->get_basis_pos() : koord::invalid;	
				origin_koord.rdwr(file);
			}
			if(file->get_experimental_version() >= 12)
			{
				fprintf(stderr, "saved %d,%d\n", origin_pos.x, origin_pos.y);
				origin_pos.rdwr(file);
			}
		}
		else 
		{
			koord ziel_koord;
			ziel_koord.rdwr(file);
			ziel = welt->get_halt_koord_index(ziel_koord);
			koord zwischen_ziel_koord;
			zwischen_ziel_koord.rdwr(file);
			zwischenziel = welt->get_halt_koord_index(zwischen_ziel_koord);
		
			if(file->get_experimental_version() >= 1)
			{
				koord origin_koord;	

				origin_koord.rdwr(file);
				if(file->get_experimental_version() == 1)
				{				
					// Simutrans-Experimental save version 1 had extra parameters
					// such as "previous transfer" intended for use in the new revenue
					// system. In the end, the system was designed differently, and
					// these values are not present in versions 2 and above.
					koord dummy;
					dummy.rdwr(file);
				}
			
				origin = welt->get_halt_koord_index(origin_koord);

				origin_pos = koord::invalid;
				if(file->get_experimental_version() >= 12)
				{
					origin_pos.rdwr(file);
					fprintf(stderr, "loaded %d,%d\n", origin_pos.x, origin_pos.y);}
			}
			else
			{
				origin = zwischenziel;
			}
		}
	}
	zielpos.rdwr(file);

	if(file->get_experimental_version() == 1)
	{
		uint32 dummy_2;
		file->rdwr_long(dummy_2);
		file->rdwr_long(dummy_2);
	}

	if(file->get_experimental_version() >= 2)
	{
		if(file->get_version() < 110007)
		{
			// Was accumulated distance
			// (now handled in convoys)
			uint32 dummy = 0;
			file->rdwr_long(dummy);
		}
		if(file->get_experimental_version() < 4)
		{
			// Was journey steps
			uint8 dummy;
			file->rdwr_byte(dummy);
		}
		file->rdwr_longlong(arrival_time);
	}
	else
	{
		arrival_time = 0;
	}

	if(file->get_experimental_version() >= 10 && file->get_version() >= 111000)
	{
		if(file->is_saving()) 
		{
			uint16 halt_id = last_transfer.is_bound() ? last_transfer.get_id() : 0;
			file->rdwr_short(halt_id);
		}
		else
		{
			uint16 halt_id;
			file->rdwr_short(halt_id);
			last_transfer.set_id(halt_id);
		}
	}
	else
	{
		last_transfer.set_id(origin.get_id());
	}

	if(file->get_experimental_version() >= 12)
	{
		bool commuting = (get_besch() == warenbauer_t::commuters);
		file->rdwr_bool(commuting);
		if(commuting) {
			const ware_besch_t *type = warenbauer_t::commuters;
			index = type->get_index();
		}
	}
}

void ware_t::laden_abschliessen(karte_t *welt, spieler_t * /*sp*/)  //"Invite finish" (Google); "load lock" (Babelfish).
{
	// since some halt was referred by with several koordinates
	// this routine will correct it
	if(ziel.is_bound() && ziel->get_init_pos() != koord::invalid) 
	{
		ziel = welt->lookup(ziel->get_init_pos())->get_halt();
	}
	if(zwischenziel.is_bound() && zwischenziel->get_init_pos() != koord::invalid) 
	{
		zwischenziel = welt->lookup(zwischenziel->get_init_pos())->get_halt();
	}

	if(last_transfer.is_bound() && last_transfer->get_init_pos() != koord::invalid)
	{
		last_transfer = welt->lookup(last_transfer->get_init_pos())->get_halt();
	}

	if(origin.is_bound() && origin->get_init_pos() != koord::invalid) 
	{
		origin = welt->lookup(origin->get_init_pos())->get_halt();
	}

	update_factory_target(welt);
}


void ware_t::rotate90( karte_t *welt, sint16 y_size )
{
	origin_pos.rotate90( y_size);
	zielpos.rotate90( y_size );
	update_factory_target(welt);
}


void ware_t::update_factory_target(karte_t *welt)
{
	// assert that target coordinates are unique for cargo going to the same factory
	// as new cargo will be generated with possibly new factory coordinates
	fabrik_t *fab = fabrik_t::get_fab( welt, zielpos );
	if (fab) {
		zielpos = fab->get_pos().get_2d();
	}
}
