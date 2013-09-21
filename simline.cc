
#include "simtypes.h"
#include "simline.h"
#include "simhalt.h"
#include "simworld.h"

#include "utils/simstring.h"
#include "dataobj/fahrplan.h"
#include "dataobj/translator.h"
#include "dataobj/loadsave.h"
#include "player/simplay.h"
#include "vehicle/simvehikel.h"
#include "simconvoi.h"
#include "convoihandle_t.h"
#include "simlinemgmt.h"

uint8 convoi_to_line_catgory_[convoi_t::MAX_CONVOI_COST] =
{
	LINE_CAPACITY, LINE_TRANSPORTED_GOODS, LINE_AVERAGE_SPEED, LINE_COMFORT, LINE_REVENUE, LINE_OPERATIONS, LINE_PROFIT, LINE_DISTANCE, LINE_REFUNDS 
};

uint8 simline_t::convoi_to_line_catgory(uint8 cnv_cost)
{
	assert(cnv_cost < convoi_t::MAX_CONVOI_COST);
	return convoi_to_line_catgory_[cnv_cost];
}


karte_t *simline_t::welt=NULL;

simline_t::simline_t(karte_t* welt, spieler_t* sp, linetype type)
{
	self = linehandle_t(this);
	char printname[128];
	sprintf(printname, "(%i) %s", self.get_id(), translator::translate("Line", welt->get_settings().get_name_language_id()));
	name = printname;

	init_financial_history();
	this->type = type;
	this->welt = welt;
	this->fpl = NULL;
	this->sp = sp;
	withdraw = false;
	state_color = COL_WHITE;

	for(uint8 i = 0; i < MAX_LINE_COST; i ++)
	{	
		rolling_average[i] = 0;
		rolling_average_count[i] = 0;
	}
	start_reversed = false;
	livery_scheme_index = 0;

	create_schedule();

	is_alternating_circle_route = false;

	average_journey_times = new koordhashtable_tpl<id_pair, average_tpl<uint16> >;
	average_journey_times_reverse_circular = NULL;
}


simline_t::simline_t(karte_t* welt, spieler_t* sp, linetype type, loadsave_t *file)
{
	// id will be read and assigned during rdwr
	self = linehandle_t();
	this->type = type;
	this->welt = welt;
	this->fpl = NULL;
	this->sp = sp;
	withdraw = false;
	create_schedule();
	average_journey_times = new koordhashtable_tpl<id_pair, average_tpl<uint16> >;
	average_journey_times_reverse_circular = NULL;
	rdwr(file);

	// now self has the right id but the this-pointer is not assigned to the quickstone handle yet
	// do this explicitly
	// some savegames have line_id=0, resolve that in laden_abschliessen
	if (self.get_id()!=0) {
		self = linehandle_t(this, self.get_id());
	}
}


simline_t::~simline_t()
{
	DBG_DEBUG("simline_t::~simline_t()", "deleting fpl=%p", fpl);

	assert(count_convoys()==0);
	unregister_stops();

	delete fpl;
	self.detach();
	DBG_MESSAGE("simline_t::~simline_t()", "line %d (%p) destroyed", self.get_id(), this);

	delete average_journey_times;
	delete average_journey_times_reverse_circular;
}

void simline_t::create_schedule()
{
	switch(type) {
		case simline_t::truckline:       set_schedule(new autofahrplan_t()); break;
		case simline_t::trainline:       set_schedule(new zugfahrplan_t()); break;
		case simline_t::shipline:        set_schedule(new schifffahrplan_t()); break;
		case simline_t::airline:         set_schedule(new airfahrplan_t()); break;
		case simline_t::monorailline:    set_schedule(new monorailfahrplan_t()); break;
		case simline_t::tramline:        set_schedule(new tramfahrplan_t()); break;
		case simline_t::maglevline:      set_schedule(new maglevfahrplan_t()); break;
		case simline_t::narrowgaugeline: set_schedule(new narrowgaugefahrplan_t()); break;
		default:
			dbg->fatal( "simline_t::create_schedule()", "Cannot create default schedule!" );
	}
	get_schedule()->eingabe_abschliessen();
}


void simline_t::add_convoy(convoihandle_t cnv, bool from_loading)
{
	if (line_managed_convoys.empty()  &&  self.is_bound()) {
		// first convoi -> ok, now we can announce this connection to the stations
		// unbound self can happen during loading if this line had line_id=0
		register_stops(fpl);
	}

	// first convoi may change line type
	if (type == trainline  &&  line_managed_convoys.empty() &&  cnv.is_bound()) {
		// check, if needed to convert to tram/monorail line
		if (vehikel_t const* const v = cnv->front()) {
			switch (v->get_besch()->get_waytype()) {
				case tram_wt:     type = simline_t::tramline;     break;
				// elevated monorail were saved with wrong coordinates for some versions.
				// We try to recover here
				case monorail_wt: type = simline_t::monorailline; break;
				default:          break;
			}
		}
	}
	// only add convoy if not already member of line
	line_managed_convoys.append_unique(cnv);

	// what goods can this line transport?
	bool update_schedules = false;
	if(  cnv->get_state()!=convoi_t::INITIAL  ) {
		/*
		// already on the road => need to add them
		for(  uint8 i=0;  i<cnv->get_vehikel_anzahl();  i++  ) {
			// Only consider vehicles that really transport something
			// this helps against routing errors through passenger
			// trains pulling only freight wagons
			if(  cnv->get_vehikel(i)->get_fracht_max() == 0  ) {
				continue;
			}
			const ware_besch_t *ware=cnv->get_vehikel(i)->get_fracht_typ();
			if(  ware!=warenbauer_t::nichts  &&  !goods_catg_index.is_contained(ware->get_catg_index())  ) {
				goods_catg_index.append( ware->get_catg_index(), 1 );
				update_schedules = true;
			}
		}
		*/

		// Added by : Knightly
		const minivec_tpl<uint8> &categories = cnv->get_goods_catg_index();
		const uint8 catg_count = categories.get_count();
		for (uint8 i = 0; i < catg_count; i++)
		{
			if (!goods_catg_index.is_contained(categories[i]))
			{
				goods_catg_index.append(categories[i], 1);
				update_schedules = true;
			}
		}
	}

	// will not hurt ...
	financial_history[0][LINE_CONVOIS] = count_convoys();
	recalc_status();

	// do we need to tell the world about our new schedule?
	if(  update_schedules  ) 
	{
		// Added by : Knightly
		haltestelle_t::refresh_routing(fpl, goods_catg_index, sp);
	}

	// if the schedule is flagged as bidirectional, set the initial convoy direction
	if( fpl->is_bidirectional() && !from_loading ) {
		cnv->set_reverse_schedule(start_reversed);
		start_reversed = !start_reversed;
	}
	calc_is_alternating_circular_route();
}


void simline_t::remove_convoy(convoihandle_t cnv)
{
	if(line_managed_convoys.is_contained(cnv)) {
		line_managed_convoys.remove(cnv);
		recalc_catg_index();
		financial_history[0][LINE_CONVOIS] = count_convoys();
		recalc_status();
	}
	if(line_managed_convoys.empty()) {
		unregister_stops();
	}
	calc_is_alternating_circular_route();
}


// invalid line id prior to 110.0
#define INVALID_LINE_ID_OLD ((uint16)(-1))
// invalid line id from 110.0 on
#define INVALID_LINE_ID ((uint16)(0))

void simline_t::rdwr_linehandle_t(loadsave_t *file, linehandle_t &line)
{
	uint16 id;
	if (file->is_saving()) {
		id = line.is_bound() ? line.get_id(): (file->get_version() < 110000  ? INVALID_LINE_ID_OLD : INVALID_LINE_ID);
	}
	else {
		// to avoid undefined errors during loading
		id = 0;
	}

	if(file->get_version()<88003) {
		sint32 dummy=id;
		file->rdwr_long(dummy);
		id = (uint16)dummy;
	}
	else {
		file->rdwr_short(id);
	}
	if (file->is_loading()) {
		// invalid line_id's: 0 and 65535
		if (id == INVALID_LINE_ID_OLD) {
			id = 0;
		}
		line.set_id(id);
	}
}


void simline_t::rdwr(loadsave_t *file)
{
	xml_tag_t s( file, "simline_t" );

	assert(fpl);

	file->rdwr_str(name);

	rdwr_linehandle_t(file, self);

	fpl->rdwr(file);

	//financial history
	if(file->get_version() <= 102002 || (file->get_version() < 103000 && file->get_experimental_version() < 7))
	{
		for (int j = 0; j<LINE_DISTANCE; j++) 
		{
			for (int k = MAX_MONTHS-1; k>=0; k--) 
			{
				if(((j == LINE_AVERAGE_SPEED || j == LINE_COMFORT) && file->get_experimental_version() <= 1) || (j == LINE_REFUNDS && file->get_experimental_version() < 8))
				{
					// Versions of Experimental saves with 1 and below
					// did not have settings for average speed or comfort.
					// Thus, this value must be skipped properly to
					// assign the values. Likewise, versions of Experimental < 8
					// did not store refund information.
					if(file->is_loading())
					{
						financial_history[k][j] = 0;
					}
					continue;
				}
				file->rdwr_longlong(financial_history[k][j]);
			}
		}
		for (int k = MAX_MONTHS-1; k>=0; k--) 
		{
			financial_history[k][LINE_DISTANCE] = 0;
		}
	}
	else 
	{
		for (int j = 0; j<MAX_LINE_COST; j++) 
		{
			for (int k = MAX_MONTHS-1; k>=0; k--)
			{
				if(((j == LINE_AVERAGE_SPEED || j == LINE_COMFORT) && file->get_experimental_version() <= 1) || (j == LINE_REFUNDS && file->get_experimental_version() < 8))
				{
					// Versions of Experimental saves with 1 and below
					// did not have settings for average speed or comfort.
					// Thus, this value must be skipped properly to
					// assign the values. Likewise, versions of Experimental < 8
					// did not store refund information.
					if(file->is_loading())
					{
						financial_history[k][j] = 0;
					}
					continue;
				}
				else if(j == 7 && file->get_version() >= 111001 && file->get_experimental_version() == 0)
				{
					// In Standard, this is LINE_MAXSPEED.
					sint64 dummy = 0;
					file->rdwr_longlong(dummy);
				}
				file->rdwr_longlong(financial_history[k][j]);
			}
		}
	}

	if(file->get_version()>=102002) {
		file->rdwr_bool(withdraw);
	}

	if(file->get_experimental_version() >= 9) 
	{
		file->rdwr_bool(start_reversed);
	}

	// otherwise inintialized to zero if loading ...
	financial_history[0][LINE_CONVOIS] = count_convoys();

	if(file->get_experimental_version() >= 2)
	{
		const uint8 counter = file->get_version() < 103000 ? LINE_DISTANCE : MAX_LINE_COST;
		for(uint8 i = 0; i < counter; i ++)
		{	
			file->rdwr_long(rolling_average[i]);
			file->rdwr_short(rolling_average_count[i]);
		}	
	}

	if(file->get_experimental_version() >= 9 && file->get_version() >= 110006)
	{
		file->rdwr_short(livery_scheme_index);
	}
	else
	{
		livery_scheme_index = 0;
	}

	if(file->get_experimental_version() >= 10)
	{
		if(file->is_saving())
		{
			uint32 count = average_journey_times->get_count();
			file->rdwr_long(count);

			FOR(journey_times_map, const& iter, *average_journey_times)
			{
				id_pair idp = iter.key;
				file->rdwr_short(idp.x);
				file->rdwr_short(idp.y);
				sint16 value = iter.value.count;
				file->rdwr_short(value);
				value = iter.value.total;
				file->rdwr_short(value);
			}
		}
		else
		{
			uint32 count = 0;
			file->rdwr_long(count);
			average_journey_times->clear();
			for(uint32 i = 0; i < count; i ++)
			{
				id_pair idp;
				file->rdwr_short(idp.x);
				file->rdwr_short(idp.y);
				
				uint16 count;
				uint16 total;
				file->rdwr_short(count);
				file->rdwr_short(total);

				average_tpl<uint16> average;
				average.count = count;
				average.total = total;

				average_journey_times->put(idp, average);
			}
		}
	}
	if(file->get_version() >= 111002 && file->get_experimental_version() >= 10)
	{
		file->rdwr_bool(is_alternating_circle_route);
		if(is_alternating_circle_route)
		{
			if(file->is_saving())
			{
				uint32 count = average_journey_times_reverse_circular->get_count();
				file->rdwr_long(count);

				FOR(journey_times_map, const& iter, *average_journey_times_reverse_circular)
				{
					id_pair idp = iter.key;
					file->rdwr_short(idp.x);
					file->rdwr_short(idp.y);
					sint16 value = iter.value.count;
					file->rdwr_short(value);
					value = iter.value.total;
					file->rdwr_short(value);
				}
			}
			else
			{
				uint32 count = 0;
				file->rdwr_long(count);
				if(average_journey_times_reverse_circular)
				{
					average_journey_times_reverse_circular->clear();
				}
				else
				{
					average_journey_times_reverse_circular = new journey_times_map();
				}
				for(uint32 i = 0; i < count; i ++)
				{
					id_pair idp;
					file->rdwr_short(idp.x);
					file->rdwr_short(idp.y);
				
					uint16 count;
					uint16 total;
					file->rdwr_short(count);
					file->rdwr_short(total);

					average_tpl<uint16> average;
					average.count = count;
					average.total = total;

					average_journey_times_reverse_circular->put(idp, average);
				}
			}
		}
		else
		{
			delete average_journey_times_reverse_circular;
			average_journey_times_reverse_circular = NULL;
		}
	}
	else
	{
		calc_is_alternating_circular_route();
	}
}



void simline_t::laden_abschliessen()
{
	if(  !self.is_bound()  ) {
		// get correct handle
		self = sp->simlinemgmt.get_line_with_id_zero();
		assert( self.get_rep() == this );
		DBG_MESSAGE("simline_t::laden_abschliessen", "assigned id=%d to line %s", self.get_id(), get_name());
	}
	if (!line_managed_convoys.empty()) {
		register_stops(fpl);
	}
	recalc_status();
}



void simline_t::register_stops(schedule_t * fpl)
{
DBG_DEBUG("simline_t::register_stops()", "%d fpl entries in schedule %p", fpl->get_count(),fpl);
	FOR(minivec_tpl<linieneintrag_t>, const& i, fpl->eintrag) {
		halthandle_t const halt = haltestelle_t::get_halt(welt, i.pos, sp);
		if(halt.is_bound()) {
//DBG_DEBUG("simline_t::register_stops()", "halt not null");
			halt->add_line(self);
		}
		else {
DBG_DEBUG("simline_t::register_stops()", "halt null");
		}
	}
	calc_is_alternating_circular_route();
}

int simline_t::get_replacing_convoys_count() const {
	int count=0;
	for (uint32 i=0; i<line_managed_convoys.get_count(); ++i) {
		if (line_managed_convoys[i]->get_replace()) {
			count++;
		}
	}
	return count;
}

void simline_t::unregister_stops()
{
	unregister_stops(fpl);

	// It is necessary to clear all departure data,
	// which might be out of date on a change of schedule.
	FOR(vector_tpl<convoihandle_t>, & i, line_managed_convoys)
	{
		i->clear_departures();
	}
}


void simline_t::unregister_stops(schedule_t * fpl)
{
	FOR(minivec_tpl<linieneintrag_t>, const& i, fpl->eintrag) {
		halthandle_t const halt = haltestelle_t::get_halt(welt, i.pos, sp);
		if(halt.is_bound()) {
			halt->remove_line(self);
		}
	}
	calc_is_alternating_circular_route();
}


void simline_t::renew_stops()
{
	if (!line_managed_convoys.empty()) 
	{
		register_stops( fpl );
	
		// Added by Knightly
		haltestelle_t::refresh_routing(fpl, goods_catg_index, sp);
		
		DBG_DEBUG("simline_t::renew_stops()", "Line id=%d, name='%s'", self.get_id(), name.c_str());
	}
	calc_is_alternating_circular_route();
}

void simline_t::set_schedule(schedule_t* fpl)
{
	if (this->fpl) 
	{
		haltestelle_t::refresh_routing(fpl, goods_catg_index, sp);
		unregister_stops();
		delete this->fpl;
	}
	this->fpl = fpl;
	calc_is_alternating_circular_route();
}


void simline_t::check_freight()
{
	FOR(vector_tpl<convoihandle_t>, const i, line_managed_convoys) {
		i->check_freight();
	}
}


void simline_t::new_month()
{
	recalc_status();
	for (int j = 0; j<MAX_LINE_COST; j++) 
	{
		for (int k = MAX_MONTHS-1; k>0; k--)
		{
			financial_history[k][j] = financial_history[k-1][j];
		}
		financial_history[0][j] = 0;
	}
	financial_history[0][LINE_CONVOIS] = count_convoys();

	if(financial_history[1][LINE_AVERAGE_SPEED] == 0)
	{
		// Last month's average speed is recorded as zero. This means that no
		// average speed data have been recorded in the last month, making 
		// revenue calculations inaccurate. Use the second previous month's average speed
		// for the previous month's average speed.
		financial_history[1][LINE_AVERAGE_SPEED] = financial_history[2][LINE_AVERAGE_SPEED];
	}

	for(uint8 i = 0; i < MAX_LINE_COST; i ++)
	{	
		rolling_average[i] = 0;
		rolling_average_count[i] = 0;
	}
}


void simline_t::init_financial_history()
{
	MEMZERO(financial_history);
}



/*
 * the current state saved as color
 * Meanings are BLACK (ok), WHITE (no convois), YELLOW (no vehicle moved), RED (last month income minus), DARK PURPLE (some vehicles overcrowded), BLUE (at least one convoi vehicle is obsolete)
 */
void simline_t::recalc_status()
{
	// normal state
	// Moved from an else statement at bottom
	// to ensure that this value is always initialised.
	state_color = COL_BLACK;

	if(financial_history[0][LINE_CONVOIS]==0) 
	{
		// no convoys assigned to this line
		state_color = COL_WHITE;
		withdraw = false;
	}
	else if(financial_history[0][LINE_PROFIT]<0) 
	{
		// Loss-making
		state_color = COL_RED;
	} 

	else if((financial_history[0][LINE_OPERATIONS]|financial_history[1][LINE_OPERATIONS])==0) 
	{
		// Stuck or static
		state_color = COL_YELLOW;
	}
	else if(has_overcrowded())
	{
		// Overcrowded
		state_color = COL_DARK_PURPLE;
	}
	else if(welt->use_timeline()) 
	{
		// Has obsolete vehicles.
		bool has_obsolete = false;
		FOR(vector_tpl<convoihandle_t>, const i, line_managed_convoys) {
			has_obsolete = i->has_obsolete_vehicles();
			if (has_obsolete) break;
		}
		// now we have to set it
		state_color = has_obsolete ? COL_DARK_BLUE : COL_BLACK;
	}
}

bool simline_t::has_overcrowded() const
{
	ITERATE(line_managed_convoys,i)
	{
		if(line_managed_convoys[i]->get_overcrowded() > 0)
		{
			return true;
		}
	}
	return false;
}


// recalc what good this line is moving
void simline_t::recalc_catg_index()
{
	// first copy old
	minivec_tpl<uint8> old_goods_catg_index(goods_catg_index.get_count());
	FOR(minivec_tpl<uint8>, const i, goods_catg_index) {
		old_goods_catg_index.append(i);
	}
	goods_catg_index.clear();
	withdraw = !line_managed_convoys.empty();
	// then recreate current
	FOR(vector_tpl<convoihandle_t>, const i, line_managed_convoys) {
		// what goods can this line transport?
		convoi_t const& cnv = *i;
		withdraw &= cnv.get_withdraw();

		FOR(minivec_tpl<uint8>, const catg_index, cnv.get_goods_catg_index()) {
			goods_catg_index.append_unique( catg_index );
		}
	}
	
	// Modified by	: Knightly
	// Purpose		: Determine removed and added categories and refresh only those categories.
	//				  Avoids refreshing unchanged categories
	minivec_tpl<uint8> differences(goods_catg_index.get_count() + old_goods_catg_index.get_count());

	// removed categories : present in old category list but not in new category list
	for (uint8 i = 0; i < old_goods_catg_index.get_count(); i++)
	{
		if ( ! goods_catg_index.is_contained( old_goods_catg_index[i] ) )
		{
			differences.append( old_goods_catg_index[i] );
		}
	}

	// added categories : present in new category list but not in old category list
	FOR(minivec_tpl<uint8>, const i, goods_catg_index) 
	{
		if (!old_goods_catg_index.is_contained(i)) 
			{
			differences.append(i);
		}
	}

	// refresh only those categories which are either removed or added to the category list
	haltestelle_t::refresh_routing(fpl, differences, sp);
}



void simline_t::set_withdraw( bool yes_no )
{
	withdraw = yes_no && !line_managed_convoys.empty();
	// convois in depots will be immeadiately destroyed, thus we go backwards
	for (size_t i = line_managed_convoys.get_count(); i-- != 0;) {
		line_managed_convoys[i]->set_no_load(yes_no);	// must be first, since set withdraw might destroy convoi if in depot!
		line_managed_convoys[i]->set_withdraw(yes_no);
	}
}

void simline_t::propogate_livery_scheme()
{
	ITERATE(line_managed_convoys, i)
	{
		line_managed_convoys[i]->set_livery_scheme_index(livery_scheme_index);
		line_managed_convoys[i]->apply_livery_scheme();
	}
}


void simline_t::calc_is_alternating_circular_route()
{
	const bool old_is_alternating_circle_route = is_alternating_circle_route;
	is_alternating_circle_route = false;
	const uint32 count = count_convoys();
	if(count == 0)
	{
		return;
	}
	bool first_reverse_schedule = get_convoy(0)->get_reverse_schedule();
	if((get_convoy(0)->is_circular_route() || get_convoy(count - 1)->is_circular_route()) && count > 1)
	{
		for(int i = 1; i < count; i ++)
		{
			if(get_convoy(i)->get_reverse_schedule() != first_reverse_schedule)
			{
				is_alternating_circle_route = true;
				break;
			}
		}
	}
	
	if(old_is_alternating_circle_route == false && is_alternating_circle_route == true)
	{
		delete average_journey_times_reverse_circular;
		average_journey_times_reverse_circular = new journey_times_map;
	}
	else if(is_alternating_circle_route == true && is_alternating_circle_route == false)
	{
		delete average_journey_times_reverse_circular;
		average_journey_times_reverse_circular = NULL;
	}
}
