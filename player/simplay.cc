/*
 * Copyright (c) 1997 - 2001 Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic licence.
 * (see licence.txt)
 *
 * Renovation in dec 2004 for other vehicles, timeline
 * @author prissi
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../simconvoi.h"
#include "../simdebug.h"
#include "../simdepot.h"
#include "../simhalt.h"
#include "../simintr.h"
#include "../simline.h"
#include "../simmesg.h"
#include "../simsound.h"
#include "../simticker.h"
#include "../simwerkz.h"
#include "../gui/simwin.h"
#include "../simworld.h"
#include "../display/viewport.h"

#include "../bauer/brueckenbauer.h"
#include "../bauer/hausbauer.h"
#include "../bauer/tunnelbauer.h"

#include "../besch/tunnel_besch.h"
#include "../besch/weg_besch.h"

#include "../boden/grund.h"

#include "../dataobj/settings.h"
#include "../dataobj/scenario.h"
#include "../dataobj/loadsave.h"
#include "../dataobj/translator.h"
#include "../dataobj/environment.h"

#include "../obj/bruecke.h"
#include "../obj/gebaeude.h"
#include "../obj/leitung2.h"
#include "../obj/tunnel.h"

#include "../gui/messagebox.h"

#include "../utils/cbuffer_t.h"
#include "../utils/simstring.h"

#include "../vehicle/simvehikel.h"

#include "simplay.h"
#include "finance.h"

karte_t *spieler_t::welt = NULL;

#ifdef MULTI_THREAD
#include "../utils/simthread.h"
static pthread_mutex_t laden_abschl_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

spieler_t::spieler_t(karte_t *wl, uint8 nr) :
	simlinemgmt()
{
	finance = new finance_t(this, wl);
	welt = wl;
	player_nr = nr;
	player_age = 0;
	automat = false;		// Start nicht als automatischer Spieler
	locked = false;	/* allowe to change anything */
	unlock_pending = false;

	headquarter_pos = koord::invalid;
	headquarter_level = 0;

	welt->get_settings().set_default_player_color(this);

	// we have different AI, try to find out our type:
	sprintf(spieler_name_buf,"player %i",player_nr-1);

	const bool allow_access_by_default = player_nr == 1;

	for(int i = 0; i < MAX_PLAYER_COUNT; i ++)
	{
		access[i] = allow_access_by_default;
	}

	// By default, allow access to the public player.
	// In most cases, the public player can override the absence of 
	// access in any event, but this is relevant to whether private
	// cars may use player roads.
	access[1] = true; 
}


spieler_t::~spieler_t()
{
	while(  !messages.empty()  ) {
		delete messages.remove_first();
	}
	destroy_win(magic_finances_t + get_player_nr());
	if( finance !=NULL) {
		delete finance;
		finance = NULL;
	}
}


void spieler_t::book_construction_costs(spieler_t * const sp, const sint64 amount, const koord k, const waytype_t wt)
{
	if(sp!=NULL) {
		sp->finance->book_construction_costs(amount, wt);
		if(k != koord::invalid) {
			sp->add_money_message(amount, k);
		}
	}
}


/**
 * Adds some amount to the maintenance costs.
 * @param change the change
 * @return the new maintenance costs
 * @author Hj. Malthaner
 */
sint32 spieler_t::add_maintenance(sint32 change, waytype_t const wt)
{
	int tmp = 0;
#ifdef MULTI_THREAD
		pthread_mutex_lock( &laden_abschl_mutex  );
#endif
	tmp = finance->book_maintenance(change, wt);
#ifdef MULTI_THREAD
		pthread_mutex_unlock( &laden_abschl_mutex  );
#endif
	return tmp;
}


void spieler_t::add_money_message(const sint64 amount, const koord pos)
{
	if(amount != 0  &&  player_nr != 1) {
		if(  koord_distance(welt->get_viewport()->get_world_position(),pos)<2*(uint32)(display_get_width()/get_tile_raster_width())+3  ) {
			// only display, if near the screen ...
			add_message(amount, pos);

			// and same for sound too ...
			if(  amount>=10000  &&  !welt->is_fast_forward()  ) {
				welt->play_sound_area_clipped(pos, SFX_CASH);
			}
		}
	}
}


/**
 * amount has negative value = buy vehicle, positive value = vehicle sold
 */
void spieler_t::book_new_vehicle(const sint64 amount, const koord k, const waytype_t wt)
{
	finance->book_new_vehicle(amount, wt);
	add_money_message(amount, k);
}


void spieler_t::book_revenue(const sint64 amount, const koord k, const waytype_t wt, sint32 index)
{
	finance->book_revenue(amount, wt, index);
	add_money_message(amount, k);
}


void spieler_t::book_running_costs(const sint64 amount, const waytype_t wt)
{
	finance->book_running_costs(amount, wt);
}

void spieler_t::book_vehicle_maintenance(const sint64 amount, const waytype_t wt)
{
	finance->book_vehicle_maintenance_with_bits(amount, wt);
	// Consider putting messages in here
}


void spieler_t::book_toll_paid(const sint64 amount, const waytype_t wt)
{
	finance->book_toll_paid(amount, wt);
}


void spieler_t::book_toll_received(const sint64 amount, const waytype_t wt)
{
	finance->book_toll_received(amount, wt);
}


void spieler_t::book_transported(const sint64 amount, const waytype_t wt, int index)
{
	finance->book_transported(amount, wt, index);
}

void spieler_t::book_delivered(const sint64 amount, const waytype_t wt, int index)
{
	finance->book_delivered(amount, wt, index);
}

bool spieler_t::can_afford(const sint64 price) const
{
	return (
		   player_nr == 1 // Public service can always afford anything
		|| finance->can_afford(price)
	);
}

bool spieler_t::can_afford(spieler_t* sp, sint64 price)
{
	if (!sp) {
		// If there is no player involved, it can be afforded
		return true;
	} else {
		return sp->can_afford(price);
	}
}

/* returns the name of the player; "player -1" sits in front of the screen
 * @author prissi
 */
const char* spieler_t::get_name() const
{
	return translator::translate(spieler_name_buf);
}


void spieler_t::set_name(const char *new_name)
{
	tstrncpy( spieler_name_buf, new_name, lengthof(spieler_name_buf) );
}


/**
 * floating massages for all players here
 */
spieler_t::income_message_t::income_message_t( sint64 betrag, koord p )
{
	money_to_string(str, betrag/100.0);
	alter = 127;
	pos = p;
	amount = betrag;
}


void *spieler_t::income_message_t::operator new(size_t /*s*/)
{
	return freelist_t::gimme_node(sizeof(spieler_t::income_message_t));
}


void spieler_t::income_message_t::operator delete(void *p)
{
	freelist_t::putback_node(sizeof(spieler_t::income_message_t),p);
}


/**
 * Show income messages
 * @author prissi
 */
void spieler_t::display_messages()
{
	const viewport_t *vp = welt->get_viewport();

	FOR(slist_tpl<income_message_t*>, const m, messages) {

		const scr_coord scr_pos = vp->get_screen_coord(koord3d(m->pos,welt->lookup_hgt(m->pos)),koord(0,m->alter >> 4));

		display_shadow_proportional( scr_pos.x, scr_pos.y, PLAYER_FLAG|(kennfarbe1+3), COL_BLACK, m->str, true);
		if(  m->pos.x < 3  ||  m->pos.y < 3  ) {
			// very close to border => renew background
			welt->set_background_dirty();
		}
	}
}


/**
 * Age messages (move them upwards), delete too old ones
 * @author prissi
 */
void spieler_t::age_messages(long /*delta_t*/)
{
	for(slist_tpl<income_message_t *>::iterator iter = messages.begin(); iter != messages.end(); ) {
		income_message_t *m = *iter;
		m->alter -= 5;

		if(m->alter<-80) {
			iter = messages.erase(iter);
			delete m;
		}
		else {
			++iter;
		}
	}
}


void spieler_t::add_message(sint64 betrag, koord k)
{
	if(  !messages.empty()  &&  messages.back()->pos==k  &&  messages.back()->alter==127  ) {
		// last message exactly at same place, not aged
		messages.back()->amount += betrag;
		money_to_string(messages.back()->str, messages.back()->amount/100.0);
	}
	else {
		// otherwise new message
		income_message_t *m = new income_message_t(betrag,k);
		messages.append( m );
	}
}


void spieler_t::set_player_color(uint8 col1, uint8 col2)
{
	if(kennfarbe1 != col1 && welt->get_spieler(player_nr))
	{
		// Only show a change of colour scheme message if the primary colour changes.
		cbuffer_t message;
		const char* player_name = welt->get_spieler(player_nr)->get_name();
		message.printf(player_name);
		welt->get_message()->add_message(message, koord::invalid, message_t::ai, kennfarbe1);
		message.clear();
		message.printf("has changed its colour scheme.");
		welt->get_message()->add_message(message, koord::invalid, message_t::ai, col1);
	}
	set_player_color_no_message(col1, col2);
}

void spieler_t::set_player_color_no_message(uint8 col1, uint8 col2)
{
	kennfarbe1 = col1;
	kennfarbe2 = col2;
	display_set_player_color_scheme( player_nr, col1, col2 );
}


/**
 * Any action goes here (only need for AI at the moment)
 * @author Hj. Malthaner
 */
void spieler_t::step()
{
	/*
	NOTE: This would need updating to the new FOR iterators to work now.
	// die haltestellen m�ssen die Fahrpl�ne rgelmaessig pruefen
	uint8 i = (uint8)(welt->get_steps()+player_nr);
	//slist_iterator_tpl <nearby_halt_t> iter( halt_list );
	//while(iter.next()) {
	for(sint16 j = halt_list.get_count() - 1; j >= 0; j --)
	{
		if( (i & 31) == 0 ) {
			//iter.get_current().halt->step();
			halt_list[j].halt->step();
			INT_CHECK("simplay 280");
		}
	}
	*/
}


/**
 * wird von welt nach jedem monat aufgerufen
 * @author Hj. Malthaner
 */
bool spieler_t::neuer_monat()
{
	// since the messages must remain on the screen longer ...
	static cbuffer_t buf;

	// This handles rolling the finance records,
	// interest charges,
	// and infrastructure maintenance
	finance->new_month();

	// new month has started => recalculate vehicle value
	calc_assets();

	// After recalculating the assets, recalculate the credit limits
	// Credit limits are NEGATIVE numbers
	finance->calc_credit_limits();

	finance->calc_finance_history(); // Recalc after calc_assets

	simlinemgmt.new_month();


	// Insolvency settings.
	// Modified by jamespetts, February 2009
	// Bankrott ?
	sint64 account_balance = finance->get_account_balance();
	if(  account_balance < 0  )
	{
		finance->increase_account_overdrawn();
		if(!welt->get_settings().is_freeplay() && player_nr != 1 /* public player*/ )
		{
			if(welt->get_active_player_nr() == player_nr) 
			{
				if(  account_balance < finance->get_hard_credit_limit() && welt->get_settings().bankruptcy_allowed() && !env_t::networkmode )
				{
					destroy_all_win(true);
					create_win( display_get_width()/2-128, 40, new news_img("Bankrott:\n\nDu bist bankrott.\n"), w_info, magic_none);
					ticker::add_msg( translator::translate("Bankrott:\n\nDu bist bankrott.\n"), koord::invalid, PLAYER_FLAG + kennfarbe1 + 1 );
					welt->stop(false);
				}
				else
				{
					// Warnings about financial problems
					buf.clear();
					enum message_t::msg_typ warning_message_type = message_t::warnings;
					// Plural detection for the months. 
					// Different languages pluralise in different ways, so whole string must
					// be re-translated.
					if(finance->get_account_overdrawn() > 1)
					{
						buf.printf(translator::translate("You have been overdrawn\nfor %i months"), finance->get_account_overdrawn() );
					}
					else
					{
						buf.printf("%s", translator::translate("You have been overdrawn\nfor one month"));
					}
					if(welt->get_settings().get_interest_rate_percent() > 0)
					{
						buf.printf(translator::translate("\n\nInterest on your debt is\naccumulating at %i %%"),welt->get_settings().get_interest_rate_percent() );
					}
					if(  account_balance < finance->get_hard_credit_limit()  ) {
						buf.printf( translator::translate("\n\nYou are insolvent!") );
						// Only in network mode, freeplay, or no-bankruptcy
						// This is a more serious problem than the interest
						warning_message_type = message_t::problems;
					}
					else if(  account_balance < finance->get_soft_credit_limit()  ) {
						buf.printf( translator::translate("\n\nYou have exceeded your credit limit!") );
						// This is a more serious problem than the interest
						warning_message_type = message_t::problems;
					}
					welt->get_message()->add_message( buf, koord::invalid, warning_message_type, player_nr, IMG_LEER );
				}
			}
			
			if(welt->get_active_player_nr() != player_nr || env_t::networkmode)  // Not the active player or a multi-player game
			{
				// AI players play by the same rules as human players regarding bankruptcy.
				if(  account_balance < finance->get_hard_credit_limit() && welt->get_settings().bankruptcy_allowed() )
				{
					ai_bankrupt();
				}
			}
		}
	}
	else {
		finance->set_account_overdrawn( 0 );
	}

	if(  env_t::networkmode  &&  player_nr>1  &&  !automat  ) {
		// find out dummy companies (i.e. no vehicle running within x months)
		if(  welt->get_settings().get_remove_dummy_player_months()  &&  player_age >= welt->get_settings().get_remove_dummy_player_months()  )  {
			bool no_cnv = true;
			const uint16 months = min( MAX_PLAYER_HISTORY_MONTHS,  welt->get_settings().get_remove_dummy_player_months() );
			for(  uint16 m=0;  m<months  &&  no_cnv;  m++  ) {
				no_cnv &= finance->get_history_com_month(m, ATC_ALL_CONVOIS) ==0;
			}
			const uint16 years = max( MAX_PLAYER_HISTORY_YEARS,  (welt->get_settings().get_remove_dummy_player_months() - 1) / 12 );
			for(  uint16 y=0;  y<years  &&  no_cnv;  y++  ) {
				no_cnv &= finance->get_history_com_year(y, ATC_ALL_CONVOIS)==0;
			}
			// never run a convoi => dummy
			if(  no_cnv  ) {
				return false; // remove immediately
			}
		}

		// find out abandoned companies (no activity within x months)
		if(  welt->get_settings().get_unprotect_abandoned_player_months()  &&  player_age >= welt->get_settings().get_unprotect_abandoned_player_months()  )  {
			bool abandoned = true;
			const uint16 months = min( MAX_PLAYER_HISTORY_MONTHS,  welt->get_settings().get_unprotect_abandoned_player_months() );
			for(  uint16 m = 0;  m < months  &&  abandoned;  m++  ) {
				abandoned &= finance->get_history_veh_month(TT_ALL, m, ATV_NEW_VEHICLE)==0  &&  finance->get_history_veh_month(TT_ALL, m, ATV_CONSTRUCTION_COST)==0;
			}
			const uint16 years = min( MAX_PLAYER_HISTORY_YEARS, (welt->get_settings().get_unprotect_abandoned_player_months() - 1) / 12);
			for(  uint16 y = 0;  y < years  &&  abandoned;  y++  ) {
				abandoned &= finance->get_history_veh_year(TT_ALL, y, ATV_NEW_VEHICLE)==0  &&  finance->get_history_veh_year(TT_ALL, y, ATV_CONSTRUCTION_COST)==0;
			}
			// never changed convoi, never built => abandoned
			if(  abandoned  ) {
				pwd_hash.clear();
				locked = false;
				unlock_pending = false;
			}
		}
	}

	// subtract maintenance after bankruptcy check
	finance->book_account( -finance->get_maintenance_with_bits(TT_ALL) );
	// company gets older ...
	player_age ++;

	return true; // still active
}


void spieler_t::calc_assets()
{
	sint64 assets[TT_MAX];
	for(int i=0; i < TT_MAX; ++i){
		assets[i] = 0;
	}
	// all convois
	FOR(vector_tpl<convoihandle_t>, const cnv, welt->convoys()) {
		if(  cnv->get_besitzer() == this  ) {
			sint64 restwert = cnv->calc_restwert();
			assets[TT_ALL] += restwert;
			assets[finance->translate_waytype_to_tt(cnv->front()->get_waytype())] += restwert;
		}
	}

	// all vehikels stored in depot not part of a convoi
	FOR(slist_tpl<depot_t*>, const depot, depot_t::get_depot_list()) {
		if(  depot->get_player_nr() == player_nr  ) {
			FOR(slist_tpl<vehikel_t*>, const veh, depot->get_vehicle_list()) {
				sint64 restwert = veh->calc_restwert();
				assets[TT_ALL] += restwert;
				assets[finance->translate_waytype_to_tt(veh->get_waytype())] += restwert;
			}
		}
	}

	finance->set_assets(assets);
}


void spieler_t::update_assets(sint64 const delta, const waytype_t wt)
{
	finance->update_assets(delta, wt);
}


sint32 spieler_t::get_scenario_completion() const
{
	return finance->get_scenario_completed();
}


void spieler_t::set_scenario_completion(sint32 percent)
{
	finance->set_scenario_completed(percent);
}


bool spieler_t::check_owner( const spieler_t *owner, const spieler_t *test )
{
	return owner == test  ||  owner == NULL  ||  test == welt->get_spieler(1);
}


void spieler_t::ai_bankrupt()
{
	DBG_MESSAGE("spieler_t::ai_bankrupt()","Removing convois");

	for (size_t i = welt->convoys().get_count(); i-- != 0;) {
		convoihandle_t const cnv = welt->convoys()[i];
		if(cnv->get_besitzer()!=this) {
			continue;
		}

		linehandle_t line = cnv->get_line();

		if(  cnv->get_state() != convoi_t::INITIAL  ) {
			cnv->self_destruct();
			cnv->step();	// to really get rid of it
		}
		else {
			// convois in depots are directly destroyed
			cnv->self_destruct();
		}

		// last vehicle on that connection (no line => railroad)
		if(  !line.is_bound()  ||  line->count_convoys()==0  ) {
			simlinemgmt.delete_line( line );
		}
	}

	// remove headquarter pos
	headquarter_pos = koord::invalid;

	// remove all stops
	// first generate list of our stops
	slist_tpl<halthandle_t> halt_list;
	FOR(vector_tpl<halthandle_t>, const halt, haltestelle_t::get_alle_haltestellen()) {
		if(  halt->get_besitzer()==this  ) {
			halt_list.append(halt);
		}
	}
	// ... and destroy them
	while (!halt_list.empty()) {
		halthandle_t h = halt_list.remove_first();
		haltestelle_t::destroy( h );
	}

	// transfer all ways in public stops belonging to me to no one
	FOR(vector_tpl<halthandle_t>, const halt, haltestelle_t::get_alle_haltestellen()) {
		if(  halt->get_besitzer()==welt->get_spieler(1)  ) {
			// only concerns public stops tiles
			FOR(slist_tpl<haltestelle_t::tile_t>, const& i, halt->get_tiles()) {
				grund_t const* const gr = i.grund;
				for(  uint8 wnr=0;  wnr<2;  wnr++  ) {
					weg_t *w = gr->get_weg_nr(wnr);
					if(  w  &&  w->get_besitzer()==this  ) {
						// take ownership
						if (wnr>1  ||  (!gr->ist_bruecke()  &&  !gr->ist_tunnel())) {
							spieler_t::add_maintenance( this, -w->get_besch()->get_wartung(), w->get_besch()->get_finance_waytype() );
						}
						w->set_besitzer(NULL); // make public
					}
				}
			}
		}
	}

	// deactivate active tool (remove dummy grounds)
	welt->set_werkzeug(werkzeug_t::general_tool[WKZ_ABFRAGE], this);

	// next remove all ways, depot etc, that are not road or channels
	for( int y=0;  y<welt->get_size().y;  y++  ) {
		for( int x=0;  x<welt->get_size().x;  x++  ) {
			planquadrat_t *plan = welt->access(x,y);
			for (size_t b = plan->get_boden_count(); b-- != 0;) {
				grund_t *gr = plan->get_boden_bei(b);
				// remove tunnel and bridges first
				if(  gr->get_top()>0  &&  gr->obj_bei(0)->get_besitzer()==this   &&  (gr->ist_bruecke()  ||  gr->ist_tunnel())  ) {
					koord3d pos = gr->get_pos();

					waytype_t wt = gr->hat_wege() ? gr->get_weg_nr(0)->get_waytype() : powerline_wt;
					if (gr->ist_bruecke()) {
						brueckenbauer_t::remove( this, pos, wt );
						// fails if powerline bridge somehow connected to powerline bridge of another player
					}
					else {
						tunnelbauer_t::remove( this, pos, wt, true );
					}
					// maybe there are some objects left (station on bridge head etc)
					gr = plan->get_boden_in_hoehe(pos.z);
					if (gr == NULL) {
						continue;
					}
				}
				for (size_t i = gr->get_top(); i-- != 0;) {
					obj_t *obj = gr->obj_bei(i);
					if(obj->get_besitzer()==this) {
						switch(obj->get_typ()) {
							case obj_t::roadsign:
							case obj_t::signal:
							case obj_t::airdepot:
							case obj_t::bahndepot:
							case obj_t::monoraildepot:
							case obj_t::tramdepot:
							case obj_t::strassendepot:
							case obj_t::schiffdepot:
							case obj_t::senke:
							case obj_t::pumpe:
							case obj_t::wayobj:
							case obj_t::label:
								obj->entferne(this);
								delete obj;
								break;
							case obj_t::leitung:
								if(gr->ist_bruecke()) {
									add_maintenance( -((leitung_t*)obj)->get_besch()->get_wartung(), powerline_wt );
									// do not remove powerline from bridges
									obj->set_besitzer( welt->get_spieler(1) );
								}
								else {
									obj->entferne(this);
									delete obj;
								}
								break;
							case obj_t::gebaeude:
								hausbauer_t::remove( this, (gebaeude_t *)obj );
								break;
							case obj_t::way:
							{
								weg_t *w=(weg_t *)obj;
								if (gr->ist_bruecke()  ||  gr->ist_tunnel()) {
									w->set_besitzer( NULL );
								}
								else if(w->get_waytype()==road_wt  ||  w->get_waytype()==water_wt) {
									add_maintenance( -w->get_besch()->get_wartung(), w->get_waytype() );
									w->set_besitzer( NULL );
								}
								else {
									gr->weg_entfernen( w->get_waytype(), true );
								}
								break;
							}
							case obj_t::bruecke:
								add_maintenance( -((bruecke_t*)obj)->get_besch()->get_wartung(), obj->get_waytype() );
								obj->set_besitzer( NULL );
								break;
							case obj_t::tunnel:
								add_maintenance( -((tunnel_t*)obj)->get_besch()->get_wartung(), ((tunnel_t*)obj)->get_besch()->get_finance_waytype() );
								obj->set_besitzer( NULL );
								break;

							default:
								obj->set_besitzer( welt->get_spieler(1) );
						}
					}
				}
				// remove empty tiles (elevated ways)
				if (!gr->ist_karten_boden()  &&  gr->get_top()==0) {
					plan->boden_entfernen(gr);
				}
			}
		}
	}

	automat = false;
	// make account negative
	if (finance->get_account_balance() > 0) {
		finance->book_account( -finance->get_account_balance() -1 );
	}

	cbuffer_t buf;
	buf.printf( translator::translate("%s\nwas liquidated."), get_name() );
	welt->get_message()->add_message( buf, koord::invalid, message_t::ai, PLAYER_FLAG|player_nr );
}


/**
 * Speichert Zustand des Spielers
 * @param file Datei, in die gespeichert wird
 * @author Hj. Malthaner
 */
void spieler_t::rdwr(loadsave_t *file)
{
	xml_tag_t sss( file, "spieler_t" );

	if(file->get_version() < 112005) {
		sint64 konto = finance->get_account_balance();
		file->rdwr_longlong(konto);
		finance->set_account_balance(konto);

		sint32 account_overdrawn = finance->get_account_overdrawn();
		file->rdwr_long(account_overdrawn);
		finance->set_account_overdrawn( account_overdrawn );
	}

	if(file->get_version()<101000) {
		// ignore steps
		sint32 ldummy=0;
		file->rdwr_long(ldummy);
	}

	if(file->get_version()<99009) {
		sint32 farbe;
		file->rdwr_long(farbe);
		kennfarbe1 = (uint8)farbe*2;
		kennfarbe2 = kennfarbe1+24;
	}
	else {
		file->rdwr_byte(kennfarbe1);
		file->rdwr_byte(kennfarbe2);
	}

	sint32 halt_count=0;
	if(file->get_version()<99008) {
		file->rdwr_long(halt_count);
	}
	if(file->get_version()<=112002) {
		sint32 haltcount = 0;
		file->rdwr_long(haltcount);
	}

	// save all the financial statistics
	finance->rdwr( file );

	file->rdwr_bool(automat);

	// state is not saved anymore
	if(file->get_version()<99014) 
	{
		sint32 ldummy=0;
		file->rdwr_long(ldummy);
		file->rdwr_long(ldummy);
	}

	// the AI stuff is now saved directly by the different AI
	if(  file->get_version()<101000) 
	{
		sint32 ldummy = -1;
		file->rdwr_long(ldummy);
		file->rdwr_long(ldummy);
		file->rdwr_long(ldummy);
		file->rdwr_long(ldummy);
		koord k(-1,-1);
		k.rdwr( file );
		k.rdwr( file );
	}

	if(file->is_loading()) {

		// halt_count will be zero for newer savegames
DBG_DEBUG("spieler_t::rdwr()","player %i: loading %i halts.",welt->sp2num( this ),halt_count);
		for(int i=0; i<halt_count; i++) {
			haltestelle_t::create( file );
		}
		// empty undo buffer
		init_undo(road_wt,0);
	}

	// headquarter stuff
	if (file->get_version() < 86004)
	{
		headquarter_level = 0;
		headquarter_pos = koord::invalid;
	}
	else {
		file->rdwr_long(headquarter_level);
		headquarter_pos.rdwr( file );
		if(file->is_loading()) {
			if(headquarter_level<0) {
				headquarter_pos = koord::invalid;
				headquarter_level = 0;
			}
		}
	}

	// linemanagement
	if(file->get_version()>=88003) {
		simlinemgmt.rdwr(file,this);
	}

	if(file->get_version()>102002 && file->get_experimental_version() != 7) {
		// password hash
		for(  int i=0;  i<20;  i++  ) {
			file->rdwr_byte(pwd_hash[i]);
		}
		if(  file->is_loading()  ) {
			// disallow all actions, if password set (might be unlocked by password gui )
			locked = !pwd_hash.empty();
		}
	}

	// save the name too
	if(file->get_version()>102003 && (file->get_experimental_version() >= 9 || file->get_experimental_version() == 0)) 
	{
		file->rdwr_str( spieler_name_buf, lengthof(spieler_name_buf) );
	}

	if(file->get_version() >= 110007 && file->get_experimental_version() >= 10)
	{
		// Save the colour
		file->rdwr_byte(kennfarbe1);
		file->rdwr_byte(kennfarbe2);

		// Save access parameters
		uint8 max_players = MAX_PLAYER_COUNT;
		file->rdwr_byte(max_players);
		for(int i = 0; i < max_players; i ++)
		{
			file->rdwr_bool(access[i]);
		}
	}

	// save age
	if(  file->get_version() >= 112002  && (file->get_experimental_version() >= 11 || file->get_experimental_version() == 0) ) {
		file->rdwr_short( player_age );
	}
}

/**
 * called after game is fully loaded;
 */
void spieler_t::laden_abschliessen()
{
	simlinemgmt.laden_abschliessen();
	display_set_player_color_scheme( player_nr, kennfarbe1, kennfarbe2 );
	// recalculate vehicle value
	calc_assets();

	finance->calc_finance_history();
}


void spieler_t::rotate90( const sint16 y_size )
{
	simlinemgmt.rotate90( y_size );
	headquarter_pos.rotate90( y_size );
}


/**
 * R�ckruf, um uns zu informieren, dass ein Vehikel ein Problem hat
 * @author Hansj�rg Malthaner
 * @date 26-Nov-2001
 */
void spieler_t::bescheid_vehikel_problem(convoihandle_t cnv,const koord3d ziel)
{
	switch(cnv->get_state())
	{
		case convoi_t::NO_ROUTE:
DBG_MESSAGE("spieler_t::bescheid_vehikel_problem","Vehicle %s can't find a route to (%i,%i)!", cnv->get_name(),ziel.x,ziel.y);
			if(this==welt->get_active_player()) {
				cbuffer_t buf;
				buf.printf( translator::translate("Vehicle %s can't find a route!"), cnv->get_name());
				const uint32 max_axle_load = cnv->get_route()->get_max_axle_load();
				const uint32 cnv_weight = cnv->get_highest_axle_load();
				if (cnv_weight > max_axle_load) {
					buf.printf(" ");
					buf.printf(translator::translate("Vehicle weighs %it, but max weight is %it"), cnv_weight, max_axle_load); 
				}
				welt->get_message()->add_message( (const char *)buf, cnv->get_pos().get_2d(), message_t::problems, PLAYER_FLAG | player_nr, cnv->front()->get_basis_bild());
			}
			break;

		case convoi_t::WAITING_FOR_CLEARANCE_ONE_MONTH:
		case convoi_t::CAN_START_ONE_MONTH:
		case convoi_t::CAN_START_TWO_MONTHS:
DBG_MESSAGE("spieler_t::bescheid_vehikel_problem","Vehicle %s stucked!", cnv->get_name(),ziel.x,ziel.y);
			{
				cbuffer_t buf;
				buf.printf( translator::translate("Vehicle %s is stucked!"), cnv->get_name());
				welt->get_message()->add_message( (const char *)buf, cnv->get_pos().get_2d(), message_t::warnings, PLAYER_FLAG | player_nr, cnv->front()->get_basis_bild());
			}
			break;
		
		case convoi_t::OUT_OF_RANGE:
			{
				const uint16 distance = (shortest_distance(cnv->get_pos().get_2d(), ziel.get_2d()) * welt->get_settings().get_meters_per_tile()) / 1000u;
				const uint16 excess = distance - cnv->get_min_range();
				DBG_MESSAGE("spieler_t::bescheid_vehikel_problem","Vehicle %s cannot travel %ikm to (%i,%i) because it would exceed its range of %i by %ikm", cnv->get_name(), distance, ziel.x, ziel.y, cnv->get_min_range(), excess);
				if(this == welt->get_active_player())
				{
					cbuffer_t buf;
					const halthandle_t destination_halt = haltestelle_t::get_halt(ziel, welt->get_active_player());
					const char* name = destination_halt.is_bound() ? destination_halt->get_name() : translator::translate("unknown");
					buf.printf( translator::translate("Vehicle %s cannot travel %ikm to %s because that would exceed its range of %ikm by %ikm"), cnv->get_name(), distance, name, cnv->get_min_range(), excess);
					welt->get_message()->add_message( (const char *)buf, cnv->get_pos().get_2d(), message_t::warnings, PLAYER_FLAG | player_nr, cnv->front()->get_basis_bild());
				}
			}
			break;
		default:
DBG_MESSAGE("spieler_t::bescheid_vehikel_problem","Vehicle %s, state %i!", cnv->get_name(), cnv->get_state());
	}
	(void)ziel;
}


/* Here functions for UNDO
 * @date 7-Feb-2005
 * @author prissi
 */
void spieler_t::init_undo( waytype_t wtype, unsigned short max )
{
	// only human player
	// prissi: allow for UNDO for real player
DBG_MESSAGE("spieler_t::int_undo()","undo tiles %i",max);
	last_built.clear();
	last_built.resize(max+1);
	if(max>0) {
		undo_type = wtype;
	}

}


void spieler_t::add_undo(koord3d k)
{
	if(last_built.get_size()>0) {
//DBG_DEBUG("spieler_t::add_undo()","tile at (%i,%i)",k.x,k.y);
		last_built.append(k);
	}
}


sint64 spieler_t::undo()
{
	if (last_built.empty()) {
		// nothing to UNDO
		return false;
	}
	// check, if we can still do undo
	FOR(vector_tpl<koord3d>, const& i, last_built) {
		grund_t* const gr = welt->lookup(i);
		if(gr==NULL  ||  gr->get_typ()!=grund_t::boden) {
			// well, something was built here ... so no undo
			last_built.clear();
			return false;
		}
		// we allow ways, unimportant stuff but no vehicles, signals, wayobjs etc
		if(gr->obj_count()>0) {
			for( unsigned i=0;  i<gr->get_top();  i++  ) {
				switch(gr->obj_bei(i)->get_typ()) {
					// these are allowed
					case obj_t::zeiger:
					case obj_t::wolke:
					case obj_t::leitung:
					case obj_t::pillar:
					case obj_t::way:
					case obj_t::label:
					case obj_t::crossing:
					case obj_t::fussgaenger:
					case obj_t::verkehr:
					case obj_t::movingobj:
						break;
					// special case airplane
					// they can be everywhere, so we allow for everythign but runway undo
					case obj_t::aircraft: {
						if(undo_type!=air_wt) {
							break;
						}
						const aircraft_t* aircraft = obj_cast<aircraft_t>(gr->obj_bei(i));
						// flying aircrafts are ok
						if(!aircraft->is_on_ground()) {
							break;
						}
						// fall through !
					}
					// all other are forbidden => no undo any more
					default:
						last_built.clear();
						return false;
				}
			}
		}
	}

	// ok, now remove everything last built
	sint64 cost=0;
	FOR(vector_tpl<koord3d>, const& i, last_built) {
		grund_t* const gr = welt->lookup(i);
		if(  undo_type != powerline_wt  ) {
			cost += gr->weg_entfernen(undo_type, true);
			cost -= welt->get_land_value(gr->get_pos());
		}
		else {
			leitung_t* lt = gr->get_leitung();
			if (lt)
			{
				cost += lt->get_besch()->get_preis();
				lt->entferne(NULL);
				delete lt;
			}
		}
	}
	last_built.clear();
	return cost;
}


void spieler_t::tell_tool_result(werkzeug_t *tool, koord3d, const char *err, bool local)
{
	/* tools can return three kinds of messages
	 * NULL = success
	 * "" = failure, but just do not try again
	 * "bla" error message, which should be shown
	 */
	if (welt->get_active_player()==this  &&  local) {
		if(err==NULL) {
			if(tool->ok_sound!=NO_SOUND) {
				sound_play(tool->ok_sound);
			}
		}
		else if(*err!=0) {
			// something went really wrong
			sound_play(SFX_FAILURE);
			create_win( new news_img(err), w_time_delete, magic_none);
		}
	}
}


void spieler_t::book_convoi_number(int count)
{
	finance->book_convoi_number(count);
}


double spieler_t::get_konto_als_double() const
{
	return finance->get_account_balance() / 100.0;
}


int spieler_t::get_account_overdrawn() const
{
	return finance->get_account_overdrawn();
}


bool spieler_t::has_money_or_assets() const
{
	return finance->has_money_or_assets();
}
