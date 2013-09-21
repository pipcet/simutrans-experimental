/*
 * Copyright (c) 1997 - 2001 Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic licence.
 * (see licence.txt)
 */

#include <stdio.h>
#if MULTI_THREAD>1
#include <pthread.h>
static pthread_mutex_t verbinde_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t calc_bild_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pumpe_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t senke_list_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#include "leitung2.h"
#include "../simdebug.h"
#include "../simworld.h"
#include "../simdings.h"
#include "../player/simplay.h"
#include "../simimg.h"
#include "../simfab.h"
#include "../simskin.h"

#include "../simgraph.h"

#include "../utils/cbuffer_t.h"

#include "../dataobj/translator.h"
#include "../dataobj/loadsave.h"
#include "../dataobj/powernet.h"
#include "../dataobj/umgebung.h"

#include "../boden/grund.h"
#include "../bauer/wegbauer.h"

#include "../tpl/vector_tpl.h"
#include "../tpl/weighted_vector_tpl.h"

/**
 * returns possible directions for powerline on this tile
 */
ribi_t::ribi get_powerline_ribi(grund_t *gr)
{
	hang_t::typ slope = gr->get_weg_hang();
	ribi_t::ribi ribi = (ribi_t::ribi)ribi_t::alle;
	if (slope == hang_t::flach) {
		// respect possible directions for bridge and tunnel starts
		if (gr->ist_karten_boden()  &&  (gr->ist_tunnel()  ||  gr->ist_bruecke())) {
			ribi = ribi_t::doppelt( ribi_typ( gr->get_grund_hang() ) );
		}
	}
	else {
		ribi = ribi_t::doppelt( ribi_typ(slope) );
	}
	return ribi;
}

int leitung_t::gimme_neighbours(leitung_t **conn)
{
	int count = 0;
	grund_t *gr_base = welt->lookup(get_pos());
	ribi_t::ribi ribi = get_powerline_ribi(gr_base);
	for(int i=0; i<4; i++) {
		// get next connected tile (if there)
		grund_t *gr;
		conn[i] = NULL;
		if(  (ribi & ribi_t::nsow[i])  &&  gr_base->get_neighbour( gr, invalid_wt, ribi_t::nsow[i] ) ) {
			leitung_t *lt = gr->get_leitung();
			// check that we can connect to the other tile: correct slope,
			// both ground or both tunnel or both not tunnel
			bool const ok = (gr->ist_karten_boden()  &&  gr_base->ist_karten_boden())  ||  (gr->ist_tunnel()==gr_base->ist_tunnel());
			if(  lt  &&  (ribi_t::rueckwaerts(ribi_t::nsow[i]) & get_powerline_ribi(gr))  &&  ok  ) {
				if(lt->get_besitzer()->allows_access_to(get_besitzer()->get_player_nr()) || get_besitzer()->is_public_service())
				{
					conn[i] = lt;
					count++;
				}
			}
		}
	}
	return count;
}


fabrik_t *leitung_t::suche_fab_4(const koord pos)
{
	for(int k=0; k<4; k++) {
		fabrik_t *new_fab = fabrik_t::get_fab( welt, pos+koord::nsow[k] );
		if(new_fab) {
			return new_fab;
		}
	}
	return NULL;
}


#ifdef INLINE_DING_TYPE
leitung_t::leitung_t(karte_t *welt, typ type, loadsave_t *file) : ding_t(welt, type)
{
	city = NULL;
	bild = IMG_LEER;
	set_net(NULL);
	ribi = ribi_t::keine;
	rdwr(file);
	modified_production_delta_t = welt->calc_adjusted_monthly_figure(PRODUCTION_DELTA_T) + 1;
}

leitung_t::leitung_t(karte_t *welt, loadsave_t *file) : ding_t(welt, ding_t::leitung)
#else
leitung_t::leitung_t(karte_t *welt, loadsave_t *file) : ding_t(welt)
#endif
{
	city = NULL;
	bild = IMG_LEER;
	set_net(NULL);
	ribi = ribi_t::keine;
	rdwr(file);
	modified_production_delta_t = welt->calc_adjusted_monthly_figure(PRODUCTION_DELTA_T) + 1;
}


#ifdef INLINE_DING_TYPE
leitung_t::leitung_t(karte_t *welt, typ type, koord3d pos, spieler_t *sp) : ding_t(welt, type, pos)
{
	city = NULL;
	bild = IMG_LEER;
	set_net(NULL);
	set_besitzer( sp );
	set_besch(wegbauer_t::leitung_besch);
	modified_production_delta_t = welt->calc_adjusted_monthly_figure(PRODUCTION_DELTA_T) + 1;
}

leitung_t::leitung_t(karte_t *welt, koord3d pos, spieler_t *sp) : ding_t(welt, ding_t::leitung, pos)
#else
leitung_t::leitung_t(karte_t *welt, koord3d pos, spieler_t *sp) : ding_t(welt, pos)
#endif
{
	city = NULL;
	bild = IMG_LEER;
	set_net(NULL);
	set_besitzer( sp );
	set_besch(wegbauer_t::leitung_besch);
	modified_production_delta_t = welt->calc_adjusted_monthly_figure(PRODUCTION_DELTA_T) + 1;
}


leitung_t::~leitung_t()
{
	grund_t *gr = welt->lookup(get_pos());
	if(gr) {
		leitung_t *conn[4];
		int neighbours = gimme_neighbours(conn);
		gr->obj_remove(this);
		set_flag( ding_t::not_on_map );

		powernet_t *new_net = NULL;
		if(neighbours>1) {
			// only reconnect if two connections ...
			bool first = true;
			for(int i=0; i<4; i++) {
				if(conn[i]!=NULL) {
					if(!first) {
						// replace both nets
						new_net = new powernet_t();
						conn[i]->replace(new_net);
					}
					first = false;
				}
			}
		}

		// recalc images
		for(int i=0; i<4; i++) {
			if(conn[i]!=NULL) {
				conn[i]->calc_neighbourhood();
			}
		}

		if(neighbours==0) {
			delete net;
		}
		if(!gr->ist_tunnel()) {
			spieler_t::add_maintenance(get_besitzer(), -besch->get_wartung(), powerline_wt);
		}
	}
}


void leitung_t::entferne(spieler_t *sp) //"remove".
{
	spieler_t::book_construction_costs(sp, -besch->get_preis()/2, get_pos().get_2d(), powerline_wt);
	mark_image_dirty( bild, 0 );
}


/**
 * called during map rotation
 * @author prissi
 */
void leitung_t::rotate90()
{
	ding_t::rotate90();
	ribi = ribi_t::rotate90( ribi );
}


/* replace networks connection
 * non-trivial to handle transformers correctly
 * @author prissi
 */
void leitung_t::replace(powernet_t* new_net)
{
	if (get_net() != new_net) {
		// convert myself ...
//DBG_MESSAGE("leitung_t::replace()","My net %p by %p at (%i,%i)",new_net,current,base_pos.x,base_pos.y);
		set_net(new_net);
	}

	leitung_t * conn[4];
	if(gimme_neighbours(conn)>0) {
		for(int i=0; i<4; i++) {
			if(conn[i] && conn[i]->get_net()!=new_net) {
				conn[i]->replace(new_net);
			}
		}
	}
}


/**
 * Connect this piece of powerline to its neighbours
 * -> this can merge power networks
 * @author Hj. Malthaner
 */
void leitung_t::verbinde()
{
	// first get my own ...
	powernet_t *new_net = get_net();
//DBG_MESSAGE("leitung_t::verbinde()","Searching net at (%i,%i)",get_pos().x,get_pos().x);
	leitung_t * conn[4];
	if(gimme_neighbours(conn)>0) {
		for( uint8 i=0;  i<4 && new_net==NULL;  i++  ) {
			if(conn[i]) {
				new_net = conn[i]->get_net();
			}
		}
	}

//DBG_MESSAGE("leitung_t::verbinde()","Found net %p",new_net);

	// we are alone?
	if(get_net()==NULL) {
		if(new_net!=NULL) {
			replace(new_net);
		}
		else {
			// then we start a new net
			set_net(new powernet_t());
//DBG_MESSAGE("leitung_t::verbinde()","Creating new net %p",new_net);
		}
	}
	else if(new_net) {
		powernet_t *my_net = get_net();
		for( uint8 i=0;  i<4;  i++  ) {
			if(conn[i] && conn[i]->get_net()!=new_net) {
				conn[i]->replace(new_net);
			}
		}
		if(my_net && my_net!=new_net) {
			delete my_net;
		}
	}
}


/* extended by prissi */
void leitung_t::calc_bild()
{
	is_crossing = false;
	const koord pos = get_pos().get_2d();
	bool snow = get_pos().z >= welt->get_snowline();

	grund_t *gr = welt->lookup(get_pos());
	if(gr==NULL) {
		// no valid ground; usually happens during building ...
		return;
	}
	if(gr->ist_bruecke() || (gr->get_typ()==grund_t::tunnelboden && gr->ist_karten_boden())) {
		// don't display on a bridge or in a tunnel)
		set_bild(IMG_LEER);
		return;
	}

	image_id old_image = get_bild();
	hang_t::typ hang = gr->get_weg_hang();
	if(hang != hang_t::flach) {
		set_bild( besch->get_hang_bild_nr(hang, snow));
	}
	else {
		if(gr->hat_wege()) {
			// crossing with road or rail
			weg_t* way = gr->get_weg_nr(0);
			if(ribi_t::ist_gerade_ow(way->get_ribi())) {
				set_bild( besch->get_diagonal_bild_nr(ribi_t::nord|ribi_t::ost, snow));
			}
			else {
				set_bild( besch->get_diagonal_bild_nr(ribi_t::sued|ribi_t::ost, snow));
			}
			is_crossing = true;
		}
		else {
			if(ribi_t::ist_gerade(ribi)  &&  !ribi_t::ist_einfach(ribi)  &&  (pos.x+pos.y)&1) {
				// every second skip mast
				if(ribi_t::ist_gerade_ns(ribi)) {
					set_bild( besch->get_diagonal_bild_nr(ribi_t::nord|ribi_t::west, snow));
				}
				else {
					set_bild( besch->get_diagonal_bild_nr(ribi_t::sued|ribi_t::west, snow));
				}
			}
			else {
				set_bild( besch->get_bild_nr(ribi, snow));
			}
		}
	}
	if (old_image != get_bild()) {
		mark_image_dirty(old_image,0);
	}
}


/**
 * Recalculates the images of all neighbouring
 * powerlines and the powerline itself
 *
 * @author Hj. Malthaner
 */
void leitung_t::calc_neighbourhood()
{
	leitung_t *conn[4];
	ribi = ribi_t::keine;
	if(gimme_neighbours(conn)>0) {
		for( uint8 i=0;  i<4 ;  i++  ) {
			if(conn[i]  &&  conn[i]->get_net()==get_net()) {
				ribi |= ribi_t::nsow[i];
				conn[i]->add_ribi(ribi_t::rueckwaerts(ribi_t::nsow[i]));
				conn[i]->calc_bild();
			}
		}
	}
	set_flag( ding_t::dirty );
	calc_bild();
}


void print_power(cbuffer_t & buf, uint64 power_in_kW, const char *fmt_MW, const char *fmt_KW)
{
	uint64 power_in_MW = power_in_kW >> POWER_TO_MW;
	if(power_in_MW)
	{
		buf.printf( translator::translate(fmt_MW), (uint) power_in_MW );
	}
	else
	{
		buf.printf( translator::translate(fmt_KW), (uint) ((power_in_kW * 1000) >> POWER_TO_MW) );
	}
}


/**
 * @return Einen Beschreibungsstring f�r das Objekt, der z.B. in einem
 * Beobachtungsfenster angezeigt wird.
 * @author Hj. Malthaner
 */
void leitung_t::info(cbuffer_t & buf, bool dummy) const
{
	ding_t::info(buf);

	const uint64 supply = get_net()->get_supply();
	const uint64 demand = get_net()->get_demand();
	const uint64 load = demand>supply ? supply:demand;

	buf.printf( translator::translate("Net ID: %u\n"), (unsigned long)get_net() );
	print_power(buf, get_net()->get_max_capacity(), "Capacity: %u MW\n", "Capacity: %u KW\n");
	print_power(buf, demand, "Demand: %u MW\n", "Demand: %u KW\n");
	print_power(buf, supply, "Generation: %u MW\n", "Generation: %u KW\n");
	print_power(buf, load, "Act. load: %u MW\n", "Act. load: %u KW\n");
	buf.printf( translator::translate("Usage: %u %%"), (100*load)/(supply>0?supply:1) );
}


/**
 * Wird nach dem Laden der Welt aufgerufen - �blicherweise benutzt
 * um das Aussehen des Dings an Boden und Umgebung anzupassen
 *
 * @author Hj. Malthaner
 */
void leitung_t::laden_abschliessen()
{
#if MULTI_THREAD>1
	pthread_mutex_lock( &verbinde_mutex );
#endif
	verbinde();
#if MULTI_THREAD>1
	pthread_mutex_unlock( &verbinde_mutex );
#endif
#if MULTI_THREAD>1
	pthread_mutex_lock( &calc_bild_mutex );
#endif
	calc_neighbourhood();
#if MULTI_THREAD>1
	pthread_mutex_unlock( &calc_bild_mutex );
#endif
	const grund_t *gr = welt->lookup(get_pos());
	assert(gr);

	spieler_t::add_maintenance(get_besitzer(), besch->get_wartung(), powerline_wt);
}


/**
 * Speichert den Zustand des Objekts.
 *
 * @param file Zeigt auf die Datei, in die das Objekt geschrieben werden
 * soll.
 * @author Hj. Malthaner
 */
void leitung_t::rdwr(loadsave_t *file)
{
	xml_tag_t d( file, "leitung_t" );

	uint32 value;

	ding_t::rdwr(file);
	if(file->is_saving()) 
	{
		value = (unsigned long)get_net();
		file->rdwr_long(value);
		koord city_pos = koord::invalid;
		if(city != NULL)
		{
			city_pos = city->get_pos();
		}
		city_pos.rdwr(file);

		if(file->get_experimental_version() >= 12 || (file->get_experimental_version() == 11 && file->get_version() >= 112006))
		{
			if(get_typ() == senke)
			{
				senke_t* substation = (senke_t*)this;
				uint32 lpd = substation->get_last_power_demand();
				file->rdwr_long(lpd);
			}
			else
			{
				uint32 dummy = 4294967295; // 32 bit unsigned integer max.
				file->rdwr_long(dummy);
			}
		}
	}
	else 
	{
		file->rdwr_long(value);
		set_net(NULL);
		if(file->get_experimental_version() >= 3)
		{
			koord city_pos = koord::invalid;
			city_pos.rdwr(file);
			city = welt->get_city(city_pos);
			if(city)
			{
				city->add_substation((senke_t*)this);
			}

			if(file->get_experimental_version() >= 12 || (file->get_experimental_version() == 11 && file->get_version() >= 112006))
			{
				uint32 lpd = 0;
				file->rdwr_long(lpd);
				if(lpd != 4294967295)
				{
					senke_t* substation = (senke_t*)this;
					substation->set_last_power_demand(lpd);
				}
			}
		}
	}
	if(get_typ() == leitung) 
	{
		/* ATTENTION: during loading this MUST not be called from the constructor!!!
		 * (Otherwise it will be always true!)
		 */
		if(file->get_version() > 102002 && (file->get_experimental_version() >= 8 || file->get_experimental_version() == 0))
		{
			if(file->is_saving()) 
			{
				const char *s = besch->get_name();
				file->rdwr_str(s);
			}
			else 
			{
				char bname[128];
				file->rdwr_str(bname, lengthof(bname));
				if(bname[0] == '~')
				{
					set_besch(wegbauer_t::leitung_besch);					
					return;
				}

				const weg_besch_t *besch = wegbauer_t::get_besch(bname);
				if(besch==NULL) 
				{
					besch = wegbauer_t::get_besch(translator::compatibility_name(bname));
					if(besch==NULL) 
					{
						welt->add_missing_paks( bname, karte_t::MISSING_WAY );
						besch = wegbauer_t::leitung_besch;
					}
					dbg->warning("leitung_t::rdwr()", "Unknown powerline %s replaced by %s", bname, besch->get_name() );
				}
				set_besch(besch);
			}
		}
		else 
		{
			if (file->is_loading()) 
			{
				set_besch(wegbauer_t::leitung_besch);
			}
		}
	}
	else if(file->get_experimental_version() >= 8 && (get_typ() == pumpe || get_typ() == senke))
	{
		// Must add dummy string here, or else the loading/saving will fail, 
		// since we do not know whether a leitung is a plain leitung, or a pumpe
		// or a senke on *loading*, whereas we do on saving.
		char dummy[2] = "~";
		file->rdwr_str(dummy, 2);
	}
}


/************************************ from here on pump (source) stuff ********************************************/

slist_tpl<pumpe_t *> pumpe_t::pumpe_list;


void pumpe_t::neue_karte()
{
	pumpe_list.clear();
}


void pumpe_t::step_all(long delta_t)
{
	FOR(slist_tpl<pumpe_t*>, const p, pumpe_list) {
		p->step(delta_t);
	}
}

#ifdef INLINE_DING_TYPE
pumpe_t::pumpe_t(karte_t *welt, loadsave_t *file ) : leitung_t( welt, ding_t::pumpe, koord3d::invalid, NULL )
#else
pumpe_t::pumpe_t(karte_t *welt, loadsave_t *file ) : leitung_t( welt, koord3d::invalid, NULL )
#endif
{
	fab = NULL;
	supply = 0;
	rdwr( file );
}


#ifdef INLINE_DING_TYPE
pumpe_t::pumpe_t(karte_t *welt, koord3d pos, spieler_t *sp) : leitung_t(welt , ding_t::pumpe, pos, sp)
#else
pumpe_t::pumpe_t(karte_t *welt, koord3d pos, spieler_t *sp) : leitung_t(welt , pos, sp)
#endif
{
	fab = NULL;
	supply = 0;
	spieler_t::book_construction_costs(sp, welt->get_settings().cst_transformer, get_pos().get_2d(), powerline_wt);
}


pumpe_t::~pumpe_t()
{
	if(fab) {
		fab->set_transformer_connected( NULL );
		fab = NULL;
	}
	pumpe_list.remove( this );
	spieler_t::add_maintenance(get_besitzer(), (sint32)welt->get_settings().cst_maintain_transformer, powerline_wt);
}


void pumpe_t::step(long delta_t)
{
	if(fab==NULL) {
		return;
	}

	if(  delta_t==0  ) {
		return;
	}

	supply = fab->get_power();

	image_id new_bild;
	int winter_offset = 0;
	if (skinverwaltung_t::senke->get_bild_anzahl() > 3  &&  get_pos().z >= welt->get_snowline()) {
		winter_offset = 2;
	}
	if(  supply > 0  ) {
		get_net()->add_supply( supply );
		new_bild = skinverwaltung_t::pumpe->get_bild_nr(1+winter_offset);
	}
	else {
		new_bild = skinverwaltung_t::pumpe->get_bild_nr(0+winter_offset);
	}
	if(bild!=new_bild) {
		set_flag(ding_t::dirty);
		set_bild( new_bild );
	}
}


void pumpe_t::laden_abschliessen()
{
	leitung_t::laden_abschliessen();
	spieler_t::add_maintenance(get_besitzer(), -welt->get_settings().cst_maintain_transformer, powerline_wt);

	assert(get_net());

	if(  fab==NULL  ) {
		if(welt->lookup(get_pos())->ist_karten_boden()) {
			// on surface, check around
			fab = leitung_t::suche_fab_4(get_pos().get_2d());
		}
		else {
			// underground, check directly above
			fab = fabrik_t::get_fab(welt, get_pos().get_2d());
		}
		if(  fab  ) {
			// only add when factory there
			fab->set_transformer_connected( this );
		}
	}
#if MULTI_THREAD>1
	pthread_mutex_lock( &pumpe_list_mutex );
#endif
	pumpe_list.insert( this );
#if MULTI_THREAD>1
	pthread_mutex_unlock( &pumpe_list_mutex );
#endif
#if MULTI_THREAD>1
	pthread_mutex_lock( &calc_bild_mutex );
#endif
	set_bild(skinverwaltung_t::pumpe->get_bild_nr(0));
	is_crossing = false;
#if MULTI_THREAD>1
	pthread_mutex_unlock( &calc_bild_mutex );
#endif
}


void pumpe_t::info(cbuffer_t & buf, bool dummy) const
{
	ding_t::info( buf );

	buf.printf( translator::translate("Net ID: %lu\n"), (unsigned long)get_net() );
	buf.printf( translator::translate("Generation: %u MW\n"), supply>>POWER_TO_MW );
	buf.printf("\n\n"); // pad for consistent dialog size
}


/************************************ From here on drain stuff ********************************************/

slist_tpl<senke_t *> senke_t::senke_list;


void senke_t::neue_karte()
{
	senke_list.clear();
}


void senke_t::step_all(long delta_t)
{
	FOR(slist_tpl<senke_t*>, const s, senke_list) {
		s->step(delta_t);
	}
}


#ifdef INLINE_DING_TYPE
senke_t::senke_t(karte_t *welt, loadsave_t *file) : leitung_t( welt, ding_t::senke, koord3d::invalid, NULL )
#else
senke_t::senke_t(karte_t *welt, loadsave_t *file) : leitung_t( welt, koord3d::invalid, NULL )
#endif
{
	fab = NULL;
	einkommen = 0;
	max_einkommen = 1;
	next_t = 0;
	delta_sum = 0;
	last_power_demand = 0;
	power_load = 0;
	rdwr( file );
	welt->sync_add(this);
}


#ifdef INLINE_DING_TYPE
senke_t::senke_t(karte_t *welt, koord3d pos, spieler_t *sp, stadt_t* c) : leitung_t(welt, ding_t::senke, pos, sp)
#else
senke_t::senke_t(karte_t *welt, koord3d pos, spieler_t *sp, stadt_t* c) : leitung_t(welt, pos, sp)
#endif
{
	fab = NULL;
	einkommen = 0;
	max_einkommen = 1;
	city = c;
	if(city)
	{
		city->add_substation(this);
	}
	next_t = 0;
	delta_sum = 0;
	last_power_demand = 0;
	power_load = 0;
	spieler_t::book_construction_costs(sp, welt->get_settings().cst_transformer, get_pos().get_2d(), powerline_wt);
	welt->sync_add(this);
}


senke_t::~senke_t()
{
	senke_list.remove( this );
	welt->sync_remove( this );
	if(fab || city)
	{
		if(fab)
		{
			fab->set_transformer_connected( NULL );
		}
		if(city)
		{
			city->remove_substation(this);
			const vector_tpl<fabrik_t*>& city_factories = city->get_city_factories();
			ITERATE(city_factories, i)
			{
				city_factories[i]->set_transformer_connected( NULL );
			}
		}
	}
	spieler_t::add_maintenance(get_besitzer(), welt->get_settings().cst_maintain_transformer, powerline_wt);
}


void senke_t::step(long delta_t)
{
	if(fab == NULL && city == NULL)
	{
		return;
	}

	if(delta_t == 0) 
	{
		return;
	}

	uint32 power_demand = 0;
	// 'fab' is the connected factory
	uint32 fab_power_demand = 0;
	uint32 fab_power_load = 0;
	// 'municipal' is city population plus factories in city
	uint32 municipal_power_demand = 0;
	uint32 municipal_power_load = 0;

	/* First add up all sources of demand and add the demand to the net. */
	if(fab != NULL && fab->get_city() == NULL)
	{
		// Factories in cities are dealt with separetely.
		fab_power_demand = fab->step_power_demand();
	}

	if(fab && !city && fab->get_city())
	{
		// A marginal case - a substation connected to a factory in a city
		// that is not itself in a city. In this case, the whole city
		// should be supplied.
		city = fab->get_city();
		fab = NULL; // Avoid having to recheck fab->get_city() many times later.
	}

	if(city)
	{
		const vector_tpl<fabrik_t*>& city_factories = city->get_city_factories();
		ITERATE(city_factories, i)
		{
			municipal_power_demand += city_factories[i]->get_power_demand();
		}
		// Add the demand for the population
		municipal_power_demand += city->get_power_demand();
	}

	power_demand = fab_power_demand + municipal_power_demand;
	uint32 shared_power_demand = power_demand;
	uint32 load_proportion = 100;

	bool supply_max = false;
	if(city)
	{
		// Check to see whether there are any *other* substations in the city that supply it with electricity,
		// and divide the demand between them.
		vector_tpl<senke_t*>* city_substations = city->get_substations();
		uint32 city_substations_number = city_substations->get_count();

		uint64 supply;
		vector_tpl<senke_t*> checked_substations;
		ITERATE_PTR(city_substations, i)
		{
			// Must use two passes here: first, check all those that don't have enough to supply 
			// an equal share, then check those that do.

			const powernet_t* net = city_substations->get_element(i)->get_net();
			supply = net->get_supply() - (net->get_demand() - shared_power_demand);

			if(supply < (shared_power_demand / (city_substations_number - checked_substations.get_count())))
			{
				if(city_substations->get_element(i) != this)
				{
					shared_power_demand -= supply;
				}
				else
				{
					supply_max = true;
				}
				checked_substations.append(city_substations->get_element(i));
			}
		}
		city_substations_number -= checked_substations.get_count();

		uint32 demand_distribution;
		uint8 count = 0;
		ITERATE_PTR(city_substations, n)
		{
			// Now check those that have more than enough power.

			if(city_substations->get_element(n) == this || checked_substations.is_contained(city_substations->get_element(n)))
			{
				continue;
			}

			supply = city_substations->get_element(n)->get_power_load();
			demand_distribution = shared_power_demand / (city_substations_number - count);
			if(supply < demand_distribution)
			{
				// Just in case the lack of power of the others means that this
				// one does not have enough power.
				shared_power_demand -= supply;
				count ++;
			}
			else
			{
				shared_power_demand -= demand_distribution;
				count ++;
			}
		}
	}

	power_load = get_power_load();

	if(supply_max)
	{
		shared_power_demand = get_net()->get_supply() - get_net()->get_demand();
	}

	// Add only this substation's share of the power. 
	get_net()->add_demand(shared_power_demand);
	
	if(city && city->get_substations()->get_count() > 1)
	{
		if(power_demand == 0)
		{
			power_demand = 1;
		}
		load_proportion = (shared_power_demand * 100) / power_demand;
	}

	/* Now actually feed the power through to the factories and city
	 * Note that a substation cannot supply both a city and factory at once.
	 */
	if(fab && !fab->get_city())
	{
		// The connected industry gets priority access to the power supply if there's a shortage.
		// This should use 'min', but the current version of that in simtypes.h 
		// would cast to signed int (12.05.10)  FIXME.
		if(fab_power_demand < power_load) 
		{
			fab_power_load = fab_power_demand;
		} 
		else
		{
			fab_power_load = power_load;
		}
		fab->add_power(fab_power_load);
		if(fab_power_demand > fab_power_load) 
		{
			// this allows subsequently stepped senke to supply demand
			// which this senke couldn't
			fab->add_power_demand(power_demand-fab_power_load);
		}
	}
	if(power_load > fab_power_load)
	{
		municipal_power_load = power_load - fab_power_load;
	}
	if(city)
	{
		// Everyone else splits power on a proportional basis -- brownouts!
		const vector_tpl<fabrik_t*>& city_factories = city->get_city_factories();
		ITERATE(city_factories, i)
		{
			//city_factories[i]->set_transformer_connected(this);
			const uint32 current_factory_demand = (city_factories[i]->step_power_demand() * load_proportion) / 100;
			const uint32 current_factory_load = municipal_power_demand == 0 ? current_factory_demand : 
				(
					current_factory_demand
					* ((municipal_power_load << 5)
					/ municipal_power_demand
				)) >> 5; // <<5 for same reasons as above, FIXME
			city_factories[i]->add_power(current_factory_load);
			if (current_factory_demand > current_factory_load) 
			{
				// this allows subsequently stepped senke to supply demand
				// which this senke couldn't
				city_factories[i]->add_power_demand(current_factory_demand - current_factory_load);
			}
		}
		// City gets growth credit for power for both citizens and city factories
		city->add_power(((municipal_power_load>>POWER_TO_MW) * load_proportion) / 100);
		city->add_power_demand((municipal_power_demand>>POWER_TO_MW) * (load_proportion * load_proportion) / 10000);
	}
	// Income
	if(!fab || fab->get_besch()->get_electric_amount() == 65535)
	{
		// City, or factory demand not specified in pak: use old fixed demands
		max_einkommen += last_power_demand * delta_t / modified_production_delta_t;
		einkommen += power_load * delta_t / modified_production_delta_t;
	}
	else
	{
		max_einkommen += (last_power_demand * delta_t / modified_production_delta_t);
		einkommen += (power_load * delta_t / modified_production_delta_t);
	}

	// Income rollover
	if(max_einkommen>(2000<<11)) {
		get_besitzer()->book_revenue(einkommen >> 11, get_pos().get_2d(), powerline_wt);
		einkommen = 0;
		max_einkommen = 1;
	}

	last_power_demand = shared_power_demand;
}

uint32 senke_t::get_power_load() const
{
	const uint32 net_demand = get_net()->get_demand();
	uint32 pl;
	if(  net_demand > 0  ) 
	{
		pl = (last_power_demand * ((get_net()->get_supply() << 5) / net_demand)) >>5 ; //  <<5 for max calculation precision fitting within uint32 with max supply capped in dataobj/powernet.cc max_capacity
		if(  pl > last_power_demand  ) 
		{
			pl = last_power_demand;
		}
	}
	else 
	{
		pl = 0;
	}
	return pl;
}

bool senke_t::sync_step(long delta_t)
{
	if( fab == NULL && city == NULL) {
		return true;
	}

	delta_sum += delta_t;
	if(  delta_sum > PRODUCTION_DELTA_T  ) {
		// sawtooth waveform resetting at PRODUCTION_DELTA_T => time period for image changing
		delta_sum -= delta_sum - delta_sum % PRODUCTION_DELTA_T;
	}

	next_t += delta_t;
	if(  next_t > PRODUCTION_DELTA_T / 16  ) {
		// sawtooth waveform resetting at PRODUCTION_DELTA_T / 16 => image changes at most this fast
		next_t -= next_t - next_t % (PRODUCTION_DELTA_T / 16);

		image_id new_bild;
		int winter_offset = 0;
		if (skinverwaltung_t::senke->get_bild_anzahl() > 3  &&  get_pos().z >= welt->get_snowline()) {
			winter_offset = 2;
		}
		if(  last_power_demand > 0 ) {
			uint32 load_factor = power_load * PRODUCTION_DELTA_T / last_power_demand;

			// allow load factor to be 0, 1/8 to 7/8, 1
			// ensures power on image shows for low loads and ensures power off image shows for high loads
			if(  load_factor > 0  &&  load_factor < PRODUCTION_DELTA_T / 8  ) {
				load_factor = PRODUCTION_DELTA_T / 8;
			}
			else {
				if(  load_factor > 7 * PRODUCTION_DELTA_T / 8  &&  load_factor < PRODUCTION_DELTA_T  ) {
					load_factor = 7 * PRODUCTION_DELTA_T / 8;
				}
			}

			if(  delta_sum <= (sint32)load_factor  ) {
				new_bild = skinverwaltung_t::senke->get_bild_nr(1+winter_offset);
			}
			else {
				new_bild = skinverwaltung_t::senke->get_bild_nr(0+winter_offset);
			}
		}
		else {
			new_bild = skinverwaltung_t::senke->get_bild_nr(0+winter_offset);
		}
		if(  bild != new_bild  ) {
			set_flag(ding_t::dirty);
			set_bild( new_bild );
		}
	}
	return true;
}


void senke_t::laden_abschliessen()
{
	leitung_t::laden_abschliessen();
	spieler_t::add_maintenance(get_besitzer(), -welt->get_settings().cst_maintain_transformer, powerline_wt);

	check_industry_connexion();
#if MULTI_THREAD>1
	pthread_mutex_lock( &senke_list_mutex );
#endif
	senke_list.insert( this );
#if MULTI_THREAD>1
	pthread_mutex_unlock( &senke_list_mutex );
	pthread_mutex_lock( &calc_bild_mutex );
#endif
	set_bild(skinverwaltung_t::senke->get_bild_nr(0));
	is_crossing = false;
#if MULTI_THREAD>1
	pthread_mutex_unlock( &calc_bild_mutex );
#endif
}

void senke_t::check_industry_connexion()
{
	if(fab == NULL && city == NULL && get_net()) 
	{
		if(welt->lookup(get_pos())->ist_karten_boden()) {
			// on surface, check around
			fab = leitung_t::suche_fab_4(get_pos().get_2d());
		}
		else {
			// underground, check directly above
			fab = fabrik_t::get_fab(welt, get_pos().get_2d());
		}
		if(  fab  ) {
			fab->set_transformer_connected( this );
		}
	}
}


void senke_t::info(cbuffer_t & buf, bool dummy) const
{
	ding_t::info( buf );

	buf.printf( translator::translate("Net ID: %u\n"), (unsigned long)get_net() );
	print_power(buf, last_power_demand, "Demand: %u MW\n", "Demand: %u KW\n");
	print_power(buf, power_load, "Act. load: %u MW\n", "Act. load: %u KW\n");
	buf.printf( translator::translate("Usage: %u %%"), (100*power_load)/(last_power_demand>0?last_power_demand:1) );
	if(city)
	{
		buf.append(translator::translate("\nCity: "));
		buf.append(city->get_name());
	}
}

