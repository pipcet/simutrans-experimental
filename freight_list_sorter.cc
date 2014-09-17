#include <algorithm>

#include "freight_list_sorter.h"
#include "simhalt.h"
#include "simtypes.h"
#include "simware.h"
#include "simfab.h"
#include "simworld.h"
#include "simcity.h"

#include "dataobj/translator.h"

#include "tpl/slist_tpl.h"
#include "tpl/vector_tpl.h"

#include "utils/cbuffer_t.h"

// Necessary for MinGW
#include "malloc.h"

karte_ptr_t freight_list_sorter_t::welt;
freight_list_sorter_t::sort_mode_t freight_list_sorter_t::sortby=by_name;

/**
 *  @return whether w1 is less than w2
 */
bool freight_list_sorter_t::compare_ware(ware_t const& w1, ware_t const& w2)
{
	// sort according to freight
	// if w1 and w2 differ, they are sorted according to catg_index and index
	// we sort with respect to catg_indexfirst, since freights with the same category
	// will be displayed together
	int idx = w1.get_besch()->get_catg_index() - w2.get_besch()->get_catg_index();
	if(  idx == 0  ) {
		idx = w1.get_besch()->get_index() - w2.get_besch()->get_index();
	}
	if(  idx != 0  ) {
		return idx < 0;
	}

	switch (sortby) {
		default:
			dbg->error("freight_list_sorter::compare_ware()", "illegal sort mode!");
			break;

		case by_via_sum:
		case by_amount: { // sort by ware amount
			int const order = w2.menge - w1.menge;
			if(  order != 0  ) {
				return order < 0;
			}
			/* FALLTHROUGH */
		}
		// no break

		case by_via: { // sort by via_destination name
			halthandle_t const v1 = w1.get_zwischenziel();
			halthandle_t const v2 = w2.get_zwischenziel();
			if(  v1.is_bound() && v2.is_bound()  ) {
				int const order = strcmp(v1->get_name(), v2->get_name());
				if(  order != 0) return order < 0;
			}
			else if(  v1.is_bound()  ) {
				return false;
			}
			else if(  v2.is_bound()  ) {
				return true;
			}
			/* FALLTHROUGH */
		}
		// no break

		case by_origin: // Sort by origin stop name
		case by_origin_amount: {
			halthandle_t const o1 = w1.get_origin();
			halthandle_t const o2 = w2.get_origin();
			if (o1.is_bound() && o2.is_bound()) {
				int const order = strcmp(o1->get_name(), o2->get_name()) < 0;
				if (order != 0) return order < 0;
			} else if (o1.is_bound()) {
				return false;
			} else if (o2.is_bound()) {
				return true;
			}
			/* FALLTHROUGH */
		}
		// no break

		case by_name: { // sort by destination stop name
			halthandle_t const d1 = w1.get_ziel();
			halthandle_t const d2 = w2.get_ziel();
			if(  d1.is_bound()  &&  d2.is_bound()  ) {
				const fabrik_t *fab = NULL;
				const char *const name1 = d1->get_name();
				const char *const name2 = d2->get_name();
				return strcmp(name1, name2) < 0;
			}
			else if (d1.is_bound()) {
				return false;
			}
			else if (d2.is_bound()) {
				return true;
			}
		}

		case by_destination_detail: // Sort by ultimate destination name
		{
				
			grund_t* gr = welt->lookup_kartenboden(w1.get_zielpos());
			const gebaeude_t* const gb1 = gr ? gr->find<gebaeude_t>() : NULL;
			gr = welt->lookup_kartenboden(w2.get_zielpos());
			const gebaeude_t* const gb2 = gr ? gr->find<gebaeude_t>() : NULL;
			if(gb1 && gb2)
			{
				const fabrik_t *fab = NULL;
				// TODO -oBG, 29.12.2013: optimize:
				const char *const name1 = (fabrik_t::get_fab(w1.get_zielpos()) && sortby != by_origin ? ((fab = fabrik_t::get_fab(w1.get_zielpos())) ? fab->get_name() : "Invalid Factory" ) : translator::translate(gb1->get_tile()->get_besch()->get_name()));
				const char *const name2 = (fabrik_t::get_fab(w2.get_zielpos()) && sortby != by_origin ? ((fab = fabrik_t::get_fab(w2.get_zielpos())) ? fab->get_name() : "Invalid Factory" ) : translator::translate(gb2->get_tile()->get_besch()->get_name()));
				return strcmp(name1, name2) < 0;
			}
			if(gb1) 
			{
				return false;
			}
			if(gb2)
			{
				return true;
			}
		}
	}
	return false;
}



void freight_list_sorter_t::add_ware_heading(cbuffer_t &buf, uint32 sum, uint32 max, const ware_t *ware, const char *what_doing)
{
	// not the first line?
	if(  buf.len() > 0  ) {
		buf.append("\n");
	}
	buf.printf(" %u", sum);
	if(  max != 0  ) {
		// convois
		buf.printf("/%u", max);
	}
	ware_besch_t const& desc = *ware->get_besch();
	char const*  const  name = translator::translate(ware->get_catg() != 0 ? desc.get_catg_name() : desc.get_name());
	char const*  const  what = translator::translate(what_doing);
	// Ensure consistent spacing
	if(ware->get_catg() == 0)
	{
		buf.printf(" %s %s\n", name, what);
	}
	else
	{
		char const*  unit = translator::translate(desc.get_mass());
		// special freight (catg == 0) needs own name
		buf.printf(" %s %s %s\n", unit, name, what);
	}
}


void freight_list_sorter_t::sort_freight(vector_tpl<ware_t> const& warray, cbuffer_t& buf, sort_mode_t sort_mode, const slist_tpl<ware_t>* full_list, const char* what_doing)
{
	sortby = sort_mode;

	// hsiegeln
	// added sorting to ware's destination list
	int pos = 0;
	ware_t* wlist;
	const int warray_size = warray.get_count() * sizeof(ware_t);
#ifdef _MSC_VER
	const int max_stack_size = 838860; 
	if(warray_size < max_stack_size)
	{
		// Old method - use the stack, but will cause stack overflows if
		// warray_size is too large
		wlist = (ware_t*) alloca(warray_size);
	}
	else
#endif
	{
		// Too large for the stack - use the heap (much slower)
		wlist = (ware_t*) malloc(warray_size);
	}


	FOR(vector_tpl<ware_t>, const& ware, warray) {
		if(  ware.get_besch() == warenbauer_t::nichts  ||  ware.menge == 0  ) {
			continue;
		}
		wlist[pos] = ware;
		// for the sorting via the number for the next stop we unify entries
		if(sort_mode == by_via_sum && pos > 0) 
		{
//DBG_MESSAGE("freight_list_sorter_t::get_freight_info()","for halt %i check connection",pos);
			// only add it, if there is not another thing waiting with the same via but another destination
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() && 
					wlist[i].get_zwischenziel() == wlist[pos].get_zwischenziel() &&
					( wlist[i].get_ziel() == wlist[i].get_zwischenziel() ) == ( wlist[pos].get_ziel() == wlist[pos].get_zwischenziel() )  )
				{
					wlist[i].menge += wlist[pos--].menge;
				}
			}
		}
		if(sort_mode == by_origin_amount && pos > 0) 
		{
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() && wlist[i].get_origin() == wlist[pos].get_origin()) 
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}

		if(sort_mode == by_origin && pos > 0) 
		{
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() && wlist[i].get_origin() == wlist[pos].get_origin() && wlist[i].get_index() == wlist[pos].get_index() && wlist[i].get_ziel() == wlist[pos].get_ziel()) 
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}

		if((sort_mode == by_name || sort_mode == by_via || sort_mode == by_amount) && pos > 0) 
		{
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() && wlist[i].get_ziel() == wlist[pos].get_ziel()) 
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}

		if(sort_mode == by_destination_detail && pos > 0)
		{
			for(int i = 0; i < pos; i++) 
			{
				if(wlist[i].get_index() == wlist[pos].get_index() && wlist[i].get_zielpos() == wlist[pos].get_zielpos()) 
				{
					wlist[i].menge += wlist[pos--].menge;
					break;
				}
			}
		}
		pos++;
	}

	// if there, give the capacity for each freight
	slist_tpl<ware_t>                 const  dummy;
	slist_tpl<ware_t>                 const& list     = full_list ? *full_list : dummy;
	slist_tpl<ware_t>::const_iterator        full_i   = list.begin();
	slist_tpl<ware_t>::const_iterator const  full_end = list.end();

	// at least some capacity added?
	if(  pos != 0  ) {
		// sort the ware's list
		std::sort( wlist, wlist + pos, compare_ware );

		// print the ware's list to buffer - it should be in sortorder by now!
		int last_ware_index = -1;
		int last_ware_catg = -1;

		for(  int j = 0;  j < pos;  j++  ) {
			halthandle_t const halt			= wlist[j].get_ziel();
			halthandle_t const via_halt		= wlist[j].get_zwischenziel();
			halthandle_t const origin_halt	= wlist[j].get_origin();

			const char * name = translator::translate("unknown");
			if(  halt.is_bound()  ) {
				name = halt->get_name();
			}

			ware_t const& ware = wlist[j];
			if(  last_ware_index!=ware.get_index()  &&  last_ware_catg!=ware.get_catg()  ) {
				sint32 sum = 0;
				last_ware_index = ware.get_index();
				last_ware_catg = (ware.get_catg()!=0) ? ware.get_catg() : -1;
				for(  int i=j;  i<pos;  i++  ) {
					ware_t const& sumware = wlist[i];
					if(  last_ware_index != sumware.get_index()  ) {
						if(  last_ware_catg != sumware.get_catg()  ) {
							break;	// next category reached ...
						}
					}
					sum += sumware.menge;
				}

				// special freight => handle different
				last_ware_catg = (ware.get_catg()!=0) ? ware.get_catg() : -1;

				// display all ware
				if(full_list == NULL || full_list->get_count() == 0) {
					add_ware_heading( buf, sum, 0, &ware, what_doing );
				}
				else {
					// ok, we have a list of freights
					while(  full_i != full_end  ) {
						ware_t const& current = *full_i++;
						if(  last_ware_index==current.get_index()  ||  last_ware_catg==current.get_catg()  ) {
							add_ware_heading( buf, sum, current.menge, &current, what_doing );
							break;
						}
						else 
						{
							add_ware_heading( buf, 0, current.menge, &current, what_doing );
						}
					}
				}
			}
			// detail amount
			ware_besch_t const& desc = *ware.get_besch();
			buf.printf("   %u%s %s %c ", ware.menge, translator::translate(desc.get_mass()), translator::translate(desc.get_name()), ">>>>><>"[sortby]);
			// the target name is not correct for the via sort
			if(sortby != by_via_sum && sortby != by_origin_amount) 
			{
				koord zielpos = ware.get_zielpos();
				const grund_t* gr = welt->lookup_kartenboden(zielpos);
				const gebaeude_t* const gb = gr ? gr->get_building() : NULL;
				cbuffer_t dbuf;
				if (gb)
				{
					gb->get_description(dbuf);
				}
				else
				{
					dbuf.append(translator::translate("Unknown destination"));
				}
				const stadt_t* city = welt->get_city(zielpos);
				if(ware.is_passenger() && sortby == by_destination_detail)
				{
					const char* trip_type = (ware.is_commuting_trip ? translator::translate("commuting") : translator::translate("visiting"));

					if(city)
					{ 
						buf.printf("%s <%i, %i> (%s; %s)\n     ", dbuf.get_str(), zielpos.x, zielpos.y, city->get_name(), trip_type);
					}
					else 
					{
						buf.printf("%s <%i, %i> (%s)\n     ", dbuf.get_str(), zielpos.x, zielpos.y, trip_type);
					}
				}
				else if(sortby == by_destination_detail)
				{
					if(city)
					{
						buf.printf("%s <%i, %i> (%s)\n     ", dbuf.get_str(), zielpos.x, zielpos.y, city->get_name());
					}
					else
					{
						buf.printf("%s <%i, %i>\n     ", dbuf.get_str(), zielpos.x, zielpos.y);
					}
				}
			}

			if(sortby == by_name || sortby == by_destination_detail || sortby == by_amount || sortby == by_origin || (sortby == by_via_sum && via_halt == halt) || sortby == by_via)
			{
				const char *destination_name = translator::translate("unknown");
				if(halt.is_bound()) 
				{
					destination_name = halt->get_name();
				}
				if(sortby == by_destination_detail)
				{
					buf.printf(translator::translate(" via %s"), destination_name);
				}
				else
				{
					buf.printf(destination_name);
				}
			}

			if(sortby == by_origin_amount)
			{
				const char *origin_name = translator::translate("unknown");
				if(origin_halt.is_bound()) 
				{
					origin_name = origin_halt->get_name();
				}
				buf.printf(origin_name);
			}
			
			if(via_halt != halt && (sortby == by_via || sortby == by_via_sum))
			{
				const char *via_name = translator::translate("unknown");
				if(via_halt.is_bound()) 
				{
					via_name = via_halt->get_name();
				}
				buf.printf(translator::translate(" via %s"), via_name);
			}
			if(sortby == by_origin)
			{
				const char *origin_name = translator::translate("unknown");
				if(origin_halt.is_bound()) 
				{
					origin_name = origin_halt->get_name();
				}

				buf.printf(translator::translate(" from %s"), origin_name);
			}

			buf.append("\n");
			// debug ende
		}
	}

	// still entires left?
	for(  ; full_i != full_end; ++full_i  ) {
		ware_t const& g = *full_i;
		add_ware_heading(buf, 0, g.menge, &g, what_doing);
	}

#ifdef _MSC_VER
	if(warray_size >= max_stack_size)
#endif
	{
		free(wlist);
	}
}
