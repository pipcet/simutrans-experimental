#include <stdio.h>

#include "../../obj/roadsign.h"
#include "../../simunits.h"	// for kmh to speed conversion
#include "../roadsign_besch.h"
#include "../intro_dates.h"

#include "roadsign_reader.h"
#include "../obj_node_info.h"

#include "../../simdebug.h"
#include "../../network/pakset_info.h"


void roadsign_reader_t::register_obj(obj_besch_t *&data)
{
    roadsign_besch_t *besch = static_cast<roadsign_besch_t *>(data);

    roadsign_t::register_besch(besch);

	checksum_t *chk = new checksum_t();
	besch->calc_checksum(chk);
	pakset_info_t::append(besch->get_name(), chk);
}


bool roadsign_reader_t::successfully_loaded() const
{
    return roadsign_t::alles_geladen();
}


obj_besch_t * roadsign_reader_t::read_node(FILE *fp, obj_node_info_t &node)
{
	ALLOCA(char, besch_buf, node.size);

	roadsign_besch_t *besch = new roadsign_besch_t();
	besch->node_info = new obj_besch_t*[node.children];

	// Hajo: Read data
	fread(besch_buf, node.size, 1, fp);
	char * p = besch_buf;

	const uint16 v = decode_uint16(p);
	int version = v & 0x8000 ? v & 0x7FFF : 0;

	// Whether the read file is from Simutrans-Experimental
	// @author: jamespetts

	uint16 experimental_version = 0;
	const bool experimental = version > 0 ? v & EXP_VER : false;
	if(version > 0)
	{
		if(experimental)
		{
			// Experimental version to start at 0 and increment.
			version = version & EXP_VER ? version & 0x3FFF : 0;
			while(version > 0x100)
			{
				version -= 0x100;
				experimental_version ++;
			}
			experimental_version -= 1;
		}
	}

	if(version==3) {
		// Versioned node, version 3
		besch->min_speed = kmh_to_speed(decode_uint16(p));
		besch->cost = decode_uint32(p);
		besch->flags = decode_uint8(p);
		besch->wt = decode_uint8(p);
		besch->intro_date = decode_uint16(p);
		besch->obsolete_date = decode_uint16(p);
		if(experimental)
		{
			if(experimental_version > 1)
			{
				dbg->fatal( "roadsign_reader_t::read_node()","Incompatible pak file version for Simutrans-Ex, number %i", experimental_version );
			}
			besch->allow_underground = decode_uint8(p);
		}
	}
	else if(version==2) {
		// Versioned node, version 2
		besch->min_speed = kmh_to_speed(decode_uint16(p));
		besch->cost = decode_uint32(p);
		besch->flags = decode_uint8(p);
		besch->intro_date = DEFAULT_INTRO_DATE*12;
		besch->obsolete_date = DEFAULT_RETIRE_DATE*12;
		besch->wt = road_wt;
	}
	else if(version==1) {
		// Versioned node, version 1
		besch->min_speed = kmh_to_speed(decode_uint16(p));
		besch->cost = 50000;
		besch->flags = decode_uint8(p);
		besch->intro_date = DEFAULT_INTRO_DATE*12;
		besch->obsolete_date = DEFAULT_RETIRE_DATE*12;
		besch->wt = road_wt;
	}
	else {
		dbg->fatal("roadsign_reader_t::read_node()","version 0 not supported. File corrupt?");
	}

	if(!experimental)
	{
		// Standard roadsigns can be placed both underground and above ground.
		besch->allow_underground = 2;
	}

	DBG_DEBUG("roadsign_reader_t::read_node()","min_speed=%i, cost=%i, flags=%x, waytype=%i, intro=%i%i, retire=%i,%i",
		besch->min_speed, besch->cost/100, besch->flags, besch->wt, besch->intro_date % 12 + 1, besch->intro_date / 12, besch->obsolete_date % 12 + 1, besch->obsolete_date / 12 );
	return besch;
}
