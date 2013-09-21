/*
 * Copyright (c) 1997 - 2001 Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic licence.
 * (see licence.txt)
 */

#include <stdio.h>
#include <string.h>

#include <limits.h>
#ifdef UINT_MAX
#define MAXUINT32 UINT_MAX
#endif

#include "../simworld.h"
#include "../simcity.h"
#include "../simintr.h"
#include "../simhalt.h"
#include "../simfab.h"
#include "../boden/wege/weg.h"
#include "../boden/grund.h"
#include "../ifc/fahrer.h"
#include "loadsave.h"
#include "route.h"
#include "umgebung.h"
#include "../besch/bruecke_besch.h"

#include "../boden/wege/strasse.h"
#include "../dings/gebaeude.h"


// if defined, print some profiling informations into the file
//#define DEBUG_ROUTES

// binary heap, the fastest
#include "../tpl/binary_heap_tpl.h"


#ifdef DEBUG_ROUTES
#include "../simsys.h"
#endif



void route_t::kopiere(const route_t *r)
{
	assert(r != NULL);
	const unsigned int hops = r->get_count()-1;
	route.clear();
	route.resize(hops + 1);
	for( unsigned int i=0;  i<=hops;  i++ ) {
		route.append(r->route[i]);
	}
}

void route_t::append(const route_t *r)
{
	assert(r != NULL);
	const uint32 hops = r->get_count()-1;
	route.resize(hops+1+route.get_count());

	while (!route.empty() && back() == r->front()) {
		// skip identical end tiles
		route.pop_back();
	}
	// then append
	for( unsigned int i=0;  i<=hops;  i++ ) {
		route.append(r->position_bei(i));
	}
}


void route_t::insert(koord3d k)
{
	route.insert_at(0,k);
}


void route_t::remove_koord_from(uint32 i) {
	while(  i+1 < get_count()  ) {
		route.pop_back();
	}
}


/**
 * Appends a straight line from the last koord3d in route to the desired target.
 * Will return false if failed
 * @author prissi
 */
bool route_t::append_straight_route(karte_t *welt, koord3d dest )
{
	if(  !welt->is_within_limits(dest.get_2d())  ) {
		return false;
	}

	// then try to calculate direct route
	koord pos = back().get_2d();
	const koord ziel=dest.get_2d();
	route.resize( route.get_count()+koord_distance(pos,ziel)+2 );
DBG_MESSAGE("route_t::append_straight_route()","start from (%i,%i) to (%i,%i)",pos.x,pos.y,dest.x,dest.y);
	while(pos!=ziel) {
		// shortest way
		if(abs(pos.x-ziel.x)>=abs(pos.y-ziel.y)) {
			pos.x += (pos.x>ziel.x) ? -1 : 1;
		}
		else {
			pos.y += (pos.y>ziel.y) ? -1 : 1;
		}
		if(!welt->is_within_limits(pos)) {
			break;
		}
		route.append(welt->lookup_kartenboden(pos)->get_pos());
	}
	DBG_MESSAGE("route_t::append_straight_route()","to (%i,%i) found.",ziel.x,ziel.y);

	return pos==ziel;
}


static bool is_in_list(vector_tpl<route_t::ANode*> const& list, grund_t const* const to)
{
	FOR(vector_tpl<route_t::ANode*>, const i, list) {
		if (i->gr == to) {
			return true;
		}
	}
	return false;
}


// node arrays
uint32 route_t::MAX_STEP=0;
uint32 route_t::max_used_steps=0;
route_t::ANode *route_t::_nodes[MAX_NODES_ARRAY];
bool route_t::_nodes_in_use[MAX_NODES_ARRAY]; // semaphores, since we only have few nodes arrays in memory

void route_t::INIT_NODES(uint32 max_route_steps, const koord &world_size)
{
	for (int i = 0; i < MAX_NODES_ARRAY; ++i)
	{
		_nodes[i] = NULL;
		_nodes_in_use[i] = false;
	}

	// may need very much memory => configurable
	const uint32 max_world_step_size = world_size == koord::invalid ? max_route_steps :  world_size.x * world_size.y * 2;
	MAX_STEP = min(max_route_steps, max_world_step_size); 
	for (int i = 0; i < MAX_NODES_ARRAY; ++i)
	{
		_nodes[i] = new ANode[MAX_STEP + 4 + 2];
	}
}

void route_t::TERM_NODES()
{
	if (MAX_STEP)
	{
		MAX_STEP = 0;
		for (int i = 0; i < MAX_NODES_ARRAY; ++i)
		{
			delete [] _nodes[i];
			_nodes[i] = NULL;
			_nodes_in_use[i] = false;
		}
	}
}

uint8 route_t::GET_NODES(ANode **nodes) 
{
	for (int i = 0; i < MAX_NODES_ARRAY; ++i)
		if (!_nodes_in_use[i])
		{
			_nodes_in_use[i] = true;
			*nodes = _nodes[i];
			return i;
		}
	dbg->fatal("GET_NODE","called while list in use");
	return 0;
}

void route_t::RELEASE_NODES(uint8 nodes_index) 
{
	if (!_nodes_in_use[nodes_index])
		dbg->fatal("RELEASE_NODE","called while list free"); 
	_nodes_in_use[nodes_index] = false; 
}


/* find the route to an unknown location
 * @author prissi
 */
bool route_t::find_route(karte_t *welt, const koord3d start, fahrer_t *fahr, const uint32 max_khm, uint8 start_dir, uint32 weight, uint32 max_depth, find_route_flags flags)
{
	bool ok = false;

	// check for existing koordinates
	const grund_t* g = welt->lookup(start);
	if (g == NULL)
	{
		return false;
	}

	const uint8 enforce_weight_limits = welt->get_settings().get_enforce_weight_limits();

	// some thing for the search
	const waytype_t wegtyp = fahr->get_waytype();

	// memory in static list ...
	if(!MAX_STEP)
	{
		INIT_NODES(welt->get_settings().get_max_route_steps(), welt->get_size());
	}

//	INT_CHECK("route 347");

	// nothing in lists
	// NOTE: This will have to be reworked substantially if this algorithm
	// is to be multi-threaded anywhere: a specific vector or hashtable will 
	// have to be used instead.
	welt->unmarkiere_alle();

	// there are several variant for maintaining the open list
	// however, only binary heap and HOT queue with binary heap are worth considering
#if defined(tpl_HOT_queue_tpl_h)
    // static 
	HOT_queue_tpl <ANode *> queue;
#elif defined(tpl_binary_heap_tpl_h)
    //static 
	binary_heap_tpl <ANode *> queue;
#elif defined(tpl_sorted_heap_tpl_h)
    //static 
	sorted_heap_tpl <ANode *> queue;
#else
    //static 
	prioqueue_tpl <ANode *> queue;
#endif

	// nothing in lists
	queue.clear();

	// we clear it here probably twice: does not hurt ...
	route.clear();

	// first tile is not valid?!?
	if(!fahr->ist_befahrbar(g)) 
	{
		return false;
	}

	ANode *nodes;
	uint8 ni = GET_NODES(&nodes);

	uint32 step = 0;
	ANode* tmp = &nodes[step++];
	if (route_t::max_used_steps < step)
	{
		route_t::max_used_steps = step;
	}
	tmp->parent = NULL;
	tmp->gr = g;
	tmp->count = 0;
	tmp->g = 0;

	// start in open
	queue.insert(tmp);

//DBG_MESSAGE("route_t::find_route()","calc route from %d,%d,%d",start.x, start.y, start.z);
	const grund_t* gr;
	do 
	{
		// Hajo: this is too expensive to be called each step
		//if((step & 127) == 0) {
		//	INT_CHECK("route 161");
		//}

		ANode *test_tmp = queue.pop();

		// already in open or closed (i.e. all processed nodes) list?
		if(welt->ist_markiert(test_tmp->gr))
		{
			// we were already here on a faster route, thus ignore this branch
			// (trading speed against memory consumption)
			continue;
		}

		tmp = test_tmp;
		gr = tmp->gr;
		welt->markiere(gr);

		// already there
		if(fahr->ist_ziel(gr, tmp->parent == NULL ? NULL : tmp->parent->gr))
		{
			if(flags != private_car_checker)
			{
				// we added a target to the closed list: check for length
				break;
			}
			else
			{
				// Private car route checking does not reconstruct the route.
				// Cost should be journey time per *straight line* tile, as the private car route
				// system needs to be able to approximate the total travelling time from the straight
				// line distance.
				const koord k = gr->get_pos().get_2d();
				const stadt_t* destination_city = welt->lookup(k)->get_city();
				stadt_t* origin_city = welt->lookup(start.get_2d())->get_city();
				if(destination_city && destination_city->get_townhall_road() == k)
				{
					// This is a city destination.
					if(start.get_2d() == k)
					{
						// Very rare, but happens occasionally - two cities share a townhall road tile.
						// Must treat specially in order to avoid a division by zero error
						origin_city->add_road_connexion(10, destination_city);
					}
					else
					{
						const uint16 straight_line_distance = shortest_distance(origin_city->get_townhall_road(), k);
						origin_city->add_road_connexion(tmp->g / straight_line_distance, welt->lookup(k)->get_city());
					}
				}
				
				strasse_t* str = (strasse_t*)gr->get_weg(road_wt);
				fprintf(stderr, "str %p at <%d,%d,%d>\n", str,
					gr->get_pos().x,
					gr->get_pos().y, gr->get_pos().z);

				if(str && str->connected_buildings.get_count() > 0)
				{
					FOR(minivec_tpl<gebaeude_t*>, const gb, str->connected_buildings)
					{
						if(!gb)
						{
							// Dud building - remove
							str->connected_buildings.remove(gb);
							continue;
						}
						
						uint16 straight_line_distance = shortest_distance(origin_city->get_townhall_road(), k);
						uint16 journey_time_per_tile;
						if(straight_line_distance == 0)
						{
							journey_time_per_tile = 10;
						}
						else
						{
							journey_time_per_tile = tmp->g / straight_line_distance;
						}
						const fabrik_t* fab = gb->get_fabrik();
						if(fab)
						{
							// This is an industry
							origin_city->add_road_connexion(journey_time_per_tile, fab);
						}
						else
						{
							origin_city->add_road_connexion(journey_time_per_tile, gb);
						}
					}
				}
			}
		}

		// testing all four possible directions
		const ribi_t::ribi ribi = fahr->get_ribi(gr);
		for(int r = 0; r < 4; r++) 
		{
			// a way goes here, and it is not marked (i.e. in the closed list)
			grund_t* to;
			if((ribi & ribi_t::nsow[r] & start_dir) != 0  // allowed dir (we can restrict the first step by start_dir)
				&& koord_distance(start.get_2d(),gr->get_pos().get_2d()+koord::nsow[r]) < max_depth	// not too far away
				&& gr->get_neighbour(to, wegtyp, ribi_t::nsow[r])  // is connected
				&& fahr->ist_befahrbar(to)	// can be driven on
				&& !welt->ist_markiert(to) // Not in the closed list
			) {

				weg_t* w = to->get_weg(fahr->get_waytype());
				
				if (enforce_weight_limits > 1 && w != NULL)
				{
					// Bernd Gabriel, Mar 10, 2010: way limit info
					const uint32 way_max_axle_load = w->get_max_axle_load();
					max_axle_load = min(max_axle_load, way_max_axle_load);

					if(weight > way_max_axle_load)
					{
						if(enforce_weight_limits == 2)
						{
							// Avoid routing over ways for which the convoy is overweight.
							continue;
						}
						else if(/*enforce_weight_limits == 3 && */ way_max_axle_load > 0 && (weight * 100) / way_max_axle_load > 110)
						{
							// Avoid routing over ways for which the convoy is more than 10% overweight.
							continue;
						}
					}
				}

				// Add new node
				ANode* k = &nodes[step++];
				if (route_t::max_used_steps < step)
				{
					route_t::max_used_steps = step;
				}

				k->parent = tmp;
				k->gr = to;
				k->count = tmp->count + 1;
				k->g = tmp->g + fahr->get_kosten(to, max_khm, gr->get_pos().get_2d());

				// insert here
				queue.insert(k);
			}
		}

		// ok, now no more restraints
		start_dir = ribi_t::alle;

	} while(!queue.empty() && step < MAX_STEP  &&  queue.get_count() < max_depth);

//	INT_CHECK("route 194");

//DBG_DEBUG("reached","");
	// target reached?
	if(!fahr->ist_ziel(gr, tmp->parent == NULL ? NULL : tmp->parent->gr) || step >= MAX_STEP)
	{
		if(  step >= MAX_STEP  ) 
		{
			dbg->warning("route_t::find_route()","Too many steps (%i>=max %i) in route (too long/complex)", step, MAX_STEP);
		}
	}
	else
	{
		if(flags != private_car_checker)
		{
			// reached => construct route
			route.clear();
			route.resize(tmp->count+16);
			while(tmp != NULL) 
			{
				route.store_at(tmp->count, tmp->gr->get_pos());
	//DBG_DEBUG("add","%i,%i",tmp->pos.x,tmp->pos.y);
				tmp = tmp->parent;
			}
			ok = !route.empty();
		}
		else
		{
			ok = step < MAX_STEP;
		}
	}

	RELEASE_NODES(ni);
	return ok;
}



ribi_t::ribi *get_next_dirs(const koord gr_pos, const koord ziel)
{
	static ribi_t::ribi next_ribi[4];
	if( abs(gr_pos.x-ziel.x)>abs(gr_pos.y-ziel.y) ) {
		next_ribi[0] = (ziel.x>gr_pos.x) ? ribi_t::ost : ribi_t::west;
		next_ribi[1] = (ziel.y>gr_pos.y) ? ribi_t::sued : ribi_t::nord;
	}
	else {
		next_ribi[0] = (ziel.y>gr_pos.y) ? ribi_t::sued : ribi_t::nord;
		next_ribi[1] = (ziel.x>gr_pos.x) ? ribi_t::ost : ribi_t::west;
	}
	next_ribi[2] = ribi_t::rueckwaerts( next_ribi[1] );
	next_ribi[3] = ribi_t::rueckwaerts( next_ribi[0] );
	return next_ribi;
}


void route_t::concatenate_routes(route_t* tail_route)
{
	route.resize(route.get_count() + tail_route->route.get_count());
	ITERATE_PTR(tail_route, i)
	{
		if(i == 0)
		{ 
			// This is necessary, as otherwise the first tile of the new route
			// will be the same as the last tile of the old route, causing
			// disrupted convoy movement. 
			continue;
		}
		route.append(tail_route->route.get_element(i));
	}
}


bool route_t::intern_calc_route(karte_t *welt, const koord3d ziel, const koord3d start, fahrer_t *fahr, const sint32 max_speed, const uint32 max_cost, const uint32 axle_load, const uint32 convoy_weight, const sint32 tile_length)
{
	bool ok = false;

	// check for existing koordinates
	const grund_t *gr=welt->lookup(start);
	if(gr==NULL  ||  welt->lookup(ziel)==NULL) {
		return false;
	}

	// we clear it here probably twice: does not hurt ...
	route.clear();
	max_axle_load = MAXUINT32;
	max_convoy_weight = MAXUINT32;

	// first tile is not valid?!?
	if(!fahr->ist_befahrbar(gr)) {
		return false;
	}

	// some thing for the search
	const waytype_t wegtyp = fahr->get_waytype();
	const bool is_airplane = fahr->get_waytype()==air_wt;
	grund_t *to;

	bool ziel_erreicht=false;

	// memory in static list ...
	if(!MAX_STEP)
	{
		INIT_NODES(welt->get_settings().get_max_route_steps(), welt->get_size());
	}

	//INT_CHECK("route 347");

	// there are several variant for maintaining the open list
	// however, only binary heap and HOT queue with binary heap are worth considering
#if defined(tpl_HOT_queue_tpl_h)
    // static 
	HOT_queue_tpl <ANode *> queue;
#elif defined(tpl_binary_heap_tpl_h)
    //static 
	binary_heap_tpl <ANode *> queue;
#elif defined(tpl_sorted_heap_tpl_h)
    //static 
	sorted_heap_tpl <ANode *> queue;
#else
    //static 
	prioqueue_tpl <ANode *> queue;
#endif

	ANode *nodes;
	uint8 ni = GET_NODES(&nodes);

	uint32 step = 0;
	ANode* tmp = &nodes[step];
	step ++;
	if (route_t::max_used_steps < step)
		route_t::max_used_steps = step;

	tmp->parent = NULL;
	tmp->gr = welt->lookup(start);
	tmp->f = calc_distance(start,ziel);
	tmp->g = 0;
	tmp->dir = 0;
	tmp->count = 0;
	tmp->ribi_from = ribi_t::alle;

	// nothing in lists
	welt->unmarkiere_alle();

	// clear the queue (should be empty anyhow)
	queue.clear();
	queue.insert(tmp);
	ANode* new_top = NULL;

//DBG_MESSAGE("route_t::itern_calc_route()","calc route from %d,%d,%d to %d,%d,%d",ziel.x, ziel.y, ziel.z, start.x, start.y, start.z);
	const uint8 enforce_weight_limits = welt->get_settings().get_enforce_weight_limits();
	uint32 beat=1;
	int bridge_tile_count = 0;
	do {
		// Hajo: this is too expensive to be called each step
		if((beat++ & 255) == 0) 
		{
			//INT_CHECK("route 161");
		}

		if (new_top) {
			// this is not in closed list, no check necessary
			tmp = new_top;
			new_top = NULL;
		}
		else {
			tmp = queue.pop();
			if(welt->ist_markiert(tmp->gr)) {
				// we were already here on a faster route, thus ignore this branch
				// (trading speed against memory consumption)
				continue;
			}
		}

		gr = tmp->gr;
		welt->markiere(gr);

		// we took the target pos out of the closed list
		if(ziel==gr->get_pos())
		{
			ziel_erreicht = true; //"a goal reaches" (Babelfish).
			break;
		}

		uint32 topnode_g = !queue.empty() ? queue.front()->g : max_cost;

		// testing all four possible directions
		// mask direction we came from
		const ribi_t::ribi ribi =  fahr->get_ribi(gr)  &  ( ~ribi_t::rueckwaerts(tmp->ribi_from) );

		const ribi_t::ribi *next_ribi = get_next_dirs(gr->get_pos().get_2d(),ziel.get_2d());
		for(int r=0; r<4; r++) {

			// a way in our direction?
			if(  (ribi & next_ribi[r])==0  ) 
			{
				continue;
			}

			to = NULL;
			if(is_airplane) 
			{
				const planquadrat_t *pl=welt->lookup(gr->get_pos().get_2d()+koord(next_ribi[r]));
				if(pl) 
				{
					to = pl->get_kartenboden();
				}
			}

			// a way goes here, and it is not marked (i.e. in the closed list)
			if((to || gr->get_neighbour(to, wegtyp, next_ribi[r])) && fahr->ist_befahrbar(to) && !welt->ist_markiert(to)) 
			{
				// Do not go on a tile, where a oneway sign forbids going.
				// This saves time and fixed the bug, that a oneway sign on the final tile was ignored.
				ribi_t::ribi last_dir=next_ribi[r];
				weg_t *w = to->get_weg(wegtyp);
				ribi_t::ribi go_dir = (w==NULL) ? 0 : w->get_ribi_maske();
				if((last_dir&go_dir)!=0) 
				{
						continue;
				}
				if(enforce_weight_limits > 1 && w != NULL)
				{
					// Bernd Gabriel, Mar 10, 2010: way limit info
					bool is_overweight = false;
					if(to->ist_bruecke() || w->get_besch()->get_styp() == weg_t::type_elevated || w->get_waytype() == air_wt || w->get_waytype() == water_wt)
					{
						// Bridges care about convoy weight, whereas other types of way
						// care about axle weight.
						bridge_tile_count ++;
						
						// This is actually maximum convoy weight: the name is odd because of the virtual method.
						uint32 way_max_convoy_weight;
						
						if(w->get_besch()->get_styp() == weg_t::type_tram)
						{
							// Trams need to check the weight of the underlying bridge.
							const weg_t* underlying_bridge = welt->lookup(w->get_pos())->get_weg(road_wt);
							way_max_convoy_weight = underlying_bridge->get_max_axle_load(); 
						}
						else
						{
							way_max_convoy_weight = w->get_max_axle_load(); 
						}

						// This ensures that only that part of the convoy that is actually on the bridge counts.
						const sint32 proper_tile_length = tile_length > 8888 ? tile_length - 8888 : tile_length;
						uint32 adjusted_convoy_weight = tile_length == 0 ? convoy_weight : (convoy_weight * max(bridge_tile_count - 2, 1)) / proper_tile_length;
						const uint32 min_weight = min(adjusted_convoy_weight, convoy_weight);
						if(min_weight > way_max_convoy_weight)
						{
							if(enforce_weight_limits == 3)
							{
								is_overweight = way_max_convoy_weight == 0 || (min_weight * 100) / way_max_convoy_weight > 110;
							}
							
							else
							{
								is_overweight = true;
							}
						}
					}
					else
					{
						bridge_tile_count = 0;
						const uint32 way_max_axle_load = w->get_max_axle_load();
						max_axle_load = min(max_axle_load, way_max_axle_load);
						if(axle_load > way_max_axle_load)
						{
							if(enforce_weight_limits == 3)
							{
								is_overweight = way_max_axle_load == 0 || (axle_load * 100) / way_max_axle_load > 110;
							}
							
							else
							{
								is_overweight = true;
							}
						}
					}

					if(is_overweight)
					{
						// Avoid routing over ways for which the convoy is overweight.
						continue;
					}
					
				}

				// new values for cost g (without way it is either in the air or in water => no costs)
				uint32 new_g = tmp->g + (w ? fahr->get_kosten(to, max_speed, tmp->gr->get_pos().get_2d()) : 1);

				// check for curves (usually, one would need the lastlast and the last;
				// if not there, then we could just take the last
				uint8 current_dir;
				if(tmp->parent!=NULL) 
				{
					current_dir = ribi_typ(tmp->parent->gr->get_pos().get_2d(), to->get_pos().get_2d());
					if(tmp->dir!=current_dir)
					{
						new_g += 3;
						if(tmp->parent->dir!=tmp->dir  &&  tmp->parent->parent!=NULL) {
							// discourage 90� turns
							new_g += 10;
						}
						else if(ribi_t::ist_exakt_orthogonal(tmp->dir,current_dir))
						{
							// discourage v turns heavily
							new_g += 25;
						}
					}

				}
				else {
					current_dir = ribi_typ( gr->get_pos().get_2d(), to->get_pos().get_2d() );
				}

				const uint32 new_f = new_g + calc_distance( to->get_pos(), ziel );

				// add new
				ANode* k = &nodes[step];
				step ++;
				if (route_t::max_used_steps < step)
					route_t::max_used_steps = step;

				k->parent = tmp;
				k->gr = to;
				k->g = new_g;
				k->f = new_f;
				k->dir = current_dir;
				k->ribi_from = next_ribi[r];
				k->count = tmp->count+1;

				if (new_g <= topnode_g) {
					// do not put in queue if the new node is the best one
					topnode_g = new_g;
					if (new_top) {
						queue.insert(new_top);
					}
					new_top = k;
				}
				else {
					queue.insert( k );
				}
			}
		}

	} while (  (!queue.empty() ||  new_top) && step < MAX_STEP && tmp->g < max_cost);

#ifdef DEBUG_ROUTES
	// display marked route
	//reliefkarte_t::get_karte()->calc_map();
	DBG_DEBUG("route_t::intern_calc_route()","steps=%i  (max %i) in route, open %i, cost %u (max %u)",step,MAX_STEP,queue.get_count(),tmp->g,max_cost);
#endif

	//INT_CHECK("route 194");
	// target reached?
	if(!ziel_erreicht  || step >= MAX_STEP  ||  tmp->parent==NULL) {
		if(  step >= MAX_STEP  ) {
			dbg->warning("route_t::intern_calc_route()","Too many steps (%i>=max %i) in route (too long/complex)",step,MAX_STEP);
		}
	}
	else {
		// reached => construct route
		route.store_at( tmp->count, tmp->gr->get_pos() );
		while(tmp != NULL) {
			route[ tmp->count ] = tmp->gr->get_pos();
			tmp = tmp->parent;
		}
		ok = true;
	}

	RELEASE_NODES(ni);
	return ok;
}


/* searches route, uses intern_calc_route() for distance between stations
 * handles only driving in stations by itself
 * corrected 12/2005 for station search
 * @author Hansj�rg Malthaner, prissi
 */
route_t::route_result_t route_t::calc_route(karte_t *welt, const koord3d ziel, const koord3d start, fahrer_t *fahr, const sint32 max_khm, const uint32 axle_load, sint32 max_len, const uint32 max_cost, const uint32 convoy_weight)
{
	route.clear();

//	INT_CHECK("route 336");

#ifdef DEBUG_ROUTES
	// profiling for routes ...
	long ms=dr_time();
#endif
	bool ok = intern_calc_route(welt, start, ziel, fahr, max_khm, max_cost, axle_load, convoy_weight, max_len);
#ifdef DEBUG_ROUTES
	if(fahr->get_waytype()==water_wt) {DBG_DEBUG("route_t::calc_route()","route from %d,%d to %d,%d with %i steps in %u ms found.",start.x, start.y, ziel.x, ziel.y, route.get_count()-1, dr_time()-ms );}
#endif

//	INT_CHECK("route 343");

	if(!ok)
	{
DBG_MESSAGE("route_t::calc_route()","No route from %d,%d to %d,%d found",start.x, start.y, ziel.x, ziel.y);
		// no route found
		route.resize(1);
		route.append(start); // just to be safe
		return no_route;
	}

	// advance so all convoy fits into a halt (only set for trains and cars)
	bool move_to_end_of_station = max_len >= 8888;
	if (move_to_end_of_station)
		max_len -= 8888;
	if(max_len > 1 )
	{

		// we need a halt of course ...
		grund_t *gr = welt->lookup(start);
		halthandle_t halt = gr->get_halt();
		// NOTE: halt is actually the *destination* halt.
		if(halt.is_bound()) 
		{
			sint32 platform_size = 0;
			// Count the station size
			for(sint32 i = route.get_count() - 1; i >= 0 && max_len > 0 && halt == haltestelle_t::get_halt(welt, route[i], NULL); i--) 
			{
				platform_size++;
 			}

			// Find the end of the station, and append these tiles to the route.
			const uint32 max_n = route.get_count() - 1;
			const koord zv = route[max_n].get_2d() - route[max_n - 1].get_2d();
			const int ribi = ribi_typ(zv);//fahr->get_ribi(welt->lookup(start));

			const waytype_t wegtyp = fahr->get_waytype();

			bool is_signal_at_end_of_station = false;
			while(gr->get_neighbour(gr, wegtyp, ribi) && gr->get_halt() == halt && fahr->ist_befahrbar(gr) && (fahr->get_ribi(gr) && ribi) != 0)
			{
				// Do not go on a tile where a one way sign forbids going.
				// This saves time and fixed the bug that a one way sign on the final tile was ignored.
				ribi_t::ribi go_dir=gr->get_weg(wegtyp)->get_ribi_maske();
				if((ribi & go_dir) != 0)
				{
					break;
				}
				route.append(gr->get_pos());
				platform_size++;
			}

			if(!move_to_end_of_station && platform_size > max_len)
			{
				// Do not go to the end, but stop part way along the platform.
				sint32 truncate_from_route = min(((platform_size - max_len) + 1) >> 1, get_count() - 1);
				while(truncate_from_route-- > 0)
				{
					route.pop_back();
				}
			}

			// station too short => warning!
			if(max_len > platform_size) 
			{
				return valid_route_halt_too_short;
			}

		}
	}

	return valid_route;
}




void route_t::rdwr(loadsave_t *file)
{
	xml_tag_t r( file, "route_t" );
	sint32 max_n = route.get_count()-1;

	if(file->get_experimental_version() >= 11 && file->get_version() >= 112003)
	{
		file->rdwr_long(max_axle_load);
		file->rdwr_long(max_convoy_weight);
	}
	file->rdwr_long(max_n);
	if(file->is_loading()) {
		koord3d k;
		route.clear();
		route.resize(max_n+2);
		for(sint32 i=0;  i<=max_n;  i++ ) {
			k.rdwr(file);
			route.append(k);
		}
	}
	else {
		// writing
		for(sint32 i=0; i<=max_n; i++) {
			route[i].rdwr(file);
		}
	}
}

