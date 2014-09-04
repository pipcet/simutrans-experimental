/*
 * Copyright (c) 1997 - 2001 Hj. Malthaner
 *
 * This file is part of the Simutrans project under the artistic license.
 * (see license.txt)
 */

/*
 * Hauptklasse fuer Simutrans, Datenstruktur die alles Zusammenhaelt
 * Hj. Malthaner, 1997
 */

#include <algorithm>
#include <limits>
#include <functional>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include "path_explorer.h"

#include "simcity.h"
#include "simcolor.h"
#include "simconvoi.h"
#include "simdebug.h"
#include "simdepot.h"
#include "simfab.h"
#include "display/simgraph.h"
#include "display/viewport.h"
#include "simhalt.h"
#include "display/simimg.h"
#include "siminteraction.h"
#include "simintr.h"
#include "simio.h"
#include "simlinemgmt.h"
#include "simloadingscreen.h"
#include "simmenu.h"
#include "simmesg.h"
#include "simskin.h"
#include "simsound.h"
#include "simsys.h"
#include "simticker.h"
#include "simtools.h"
#include "simunits.h"
#include "simversion.h"
#include "display/simview.h"
#include "simwerkz.h"
#include "gui/simwin.h"
#include "simworld.h"

#include "tpl/vector_tpl.h"
#include "tpl/binary_heap_tpl.h"
#include "tpl/ordered_vector_tpl.h"
#include "tpl/stringhashtable_tpl.h"

#include "boden/boden.h"
#include "boden/wasser.h"

#include "old_blockmanager.h"
#include "vehicle/simvehikel.h"
#include "vehicle/simverkehr.h"
#include "vehicle/movingobj.h"
#include "boden/wege/schiene.h"

#include "obj/zeiger.h"
#include "obj/baum.h"
#include "obj/signal.h"
#include "obj/roadsign.h"
#include "obj/wayobj.h"
#include "obj/groundobj.h"
#include "obj/gebaeude.h"
#include "obj/leitung2.h"

#include "gui/password_frame.h"
#include "gui/messagebox.h"
#include "gui/help_frame.h"
#include "gui/karte.h"
#include "gui/player_frame_t.h"
#include "gui/components/gui_convoy_assembler.h"

#include "network/network.h"
#include "network/network_file_transfer.h"
#include "network/network_socket_list.h"
#include "network/network_cmd_ingame.h"
#include "dataobj/ribi.h"
#include "dataobj/translator.h"
#include "dataobj/loadsave.h"
#include "dataobj/scenario.h"
#include "dataobj/settings.h"
#include "dataobj/environment.h"
#include "dataobj/powernet.h"

#include "utils/cbuffer_t.h"
#include "utils/simstring.h"
#include "network/memory_rw.h"

#include "bauer/brueckenbauer.h"
#include "bauer/tunnelbauer.h"
#include "bauer/fabrikbauer.h"
#include "bauer/wegbauer.h"
#include "bauer/hausbauer.h"
#include "bauer/vehikelbauer.h"
#include "bauer/hausbauer.h"

#include "besch/grund_besch.h"
#include "besch/sound_besch.h"
#include "besch/tunnel_besch.h"
#include "besch/bruecke_besch.h"
#include "besch/stadtauto_besch.h"

#include "player/simplay.h"
#include "player/finance.h"
#include "player/ai_passenger.h"
#include "player/ai_goods.h"

#include "dataobj/tabfile.h" // For reload of simuconf.tab to override savegames

#ifdef DEBUG_SIMRAND_CALLS
bool karte_t::print_randoms = true;
int karte_t::random_calls = 0;
#endif
#ifdef DEBUG_SIMRAND_CALLS
static uint32 halt_index = 9999999;
static const char *station_name = "Newton Abbot Railway Station";
static uint32 old_menge = -1;
void station_check(const char *who, karte_t *welt)
{
	spieler_t *player = welt->get_active_player();
	if (halt_index >= (uint32)player->get_haltcount() || 
		strcmp(player->get_halt(halt_index)->get_name(), station_name))
	{
		old_menge = -1;
		for (halt_index = 0; halt_index < (uint32) player->get_haltcount(); ++halt_index)
			if (!strcmp(player->get_halt(halt_index)->get_name(), station_name))
				break;
	}
	if (halt_index < (uint32) player->get_haltcount())
	{
		const halthandle_t &station = player->get_halt(halt_index);
		uint32 menge = station->get_warray(0)->get_element(2198).menge;
		if (old_menge != menge)
		{
			dbg->warning(who, "station \"%s\" waren[0][2198].menge %u -> %u", station->get_name(), old_menge, menge);
			old_menge = menge;
		}
	}
}
#endif




// advance 201 ms per sync_step in fast forward mode
#define MAGIC_STEP (201)

// frame per second for fast forward
#define FF_PPS (10)


static uint32 last_clients = -1;
static uint8 last_active_player_nr = 0;
static std::string last_network_game;

karte_t* karte_t::world = NULL;

stringhashtable_tpl<karte_t::missing_level_t>missing_pak_names;

#ifdef MULTI_THREAD
#include "utils/simthread.h"
#include <semaphore.h>

bool spawned_world_threads=false; // global job indicator array
static simthread_barrier_t world_barrier_start;
static simthread_barrier_t world_barrier_end;


// to start a thread
typedef struct{
	karte_t *welt;
	int thread_num;
	sint16 x_step;
	sint16 x_world_max;
	sint16 y_min;
	sint16 y_max;
	sem_t* wait_for_previous;
	sem_t* signal_to_next;
	xy_loop_func function;
	bool keep_running;
} world_thread_param_t;


// now the paramters
static world_thread_param_t world_thread_param[MAX_THREADS];

void *karte_t::world_xy_loop_thread(void *ptr)
{
	world_thread_param_t *param = reinterpret_cast<world_thread_param_t *>(ptr);
	while(true) {
		if(param->keep_running) {
			simthread_barrier_wait( &world_barrier_start );	// wait for all to start
		}

		sint16 x_min = 0;
		sint16 x_max = param->x_step;

		while(  x_min < param->x_world_max  ) {
			// wait for predecessor to finish its block
			if(  param->wait_for_previous  ) {
				sem_wait( param->wait_for_previous );
			}
			(param->welt->*(param->function))(x_min, x_max, param->y_min, param->y_max);

			// signal to next thread that we finished one block
			if(  param->signal_to_next  ) {
				sem_post( param->signal_to_next );
			}
			x_min = x_max;
			x_max = min(x_max + param->x_step, param->x_world_max);
		}

		if(param->keep_running) {
			simthread_barrier_wait( &world_barrier_end );	// wait for all to finish
		}
		else {
			return NULL;
		}
	}

	return ptr;
}
#endif


void karte_t::world_xy_loop(xy_loop_func function, uint8 flags)
{
	const bool use_grids = (flags & GRIDS_FLAG) == GRIDS_FLAG;
	uint16 max_x = use_grids?(cached_grid_size.x+1):cached_grid_size.x;
	uint16 max_y = use_grids?(cached_grid_size.y+1):cached_grid_size.y;
#ifdef MULTI_THREAD
	set_random_mode( INTERACTIVE_RANDOM ); // do not allow simrand() here!

	const bool sync_x_steps = (flags & SYNCX_FLAG) == SYNCX_FLAG;

	// semaphores to synchronize progress in x direction
	sem_t sems[MAX_THREADS-1];

	for(  int t = 0;  t < env_t::num_threads;  t++  ) {
		if(  sync_x_steps  &&  t < env_t::num_threads - 1  ) {
			sem_init(&sems[t], 0, 0);
		}

   		world_thread_param[t].welt = this;
   		world_thread_param[t].thread_num = t;
		world_thread_param[t].x_step = min( 64, max_x / env_t::num_threads );
		world_thread_param[t].x_world_max = max_x;
		world_thread_param[t].y_min = (t * max_y) / env_t::num_threads;
		world_thread_param[t].y_max = ((t + 1) * max_y) / env_t::num_threads;
		world_thread_param[t].function = function;

		world_thread_param[t].wait_for_previous = sync_x_steps  &&  t > 0 ? &sems[t-1] : NULL;
		world_thread_param[t].signal_to_next    = sync_x_steps  &&  t < env_t::num_threads - 1 ? &sems[t] : NULL;

		world_thread_param[t].keep_running = t < env_t::num_threads - 1;
	}

	if(  !spawned_world_threads  ) {
		// we can do the parallel display using posix threads ...
		pthread_t thread[MAX_THREADS];
		/* Initialize and set thread detached attribute */
		pthread_attr_t attr;
		pthread_attr_init( &attr );
		pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
		// init barrier
		simthread_barrier_init( &world_barrier_start, NULL, env_t::num_threads );
		simthread_barrier_init( &world_barrier_end, NULL, env_t::num_threads );

		for(  int t = 0;  t < env_t::num_threads - 1;  t++  ) {
			if(  pthread_create( &thread[t], &attr, world_xy_loop_thread, (void *)&world_thread_param[t] )  ) {
				dbg->fatal( "karte_t::world_xy_loop()", "cannot multithread, error at thread #%i", t+1 );
				return;
			}
		}
		spawned_world_threads = true;
		pthread_attr_destroy( &attr );
	}

	// and start processing
	simthread_barrier_wait( &world_barrier_start );

	// the last we can run ourselves
	world_xy_loop_thread(&world_thread_param[env_t::num_threads-1]);

	simthread_barrier_wait( &world_barrier_end );

	// return from thread
	for(  int t = 0;  t < env_t::num_threads - 1;  t++  ) {
		if(  sync_x_steps  ) {
			sem_destroy(&sems[t]);
		}
	}

	clear_random_mode( INTERACTIVE_RANDOM ); // do not allow simrand() here!

#else
	// slow serial way of display
	(this->*function)( 0, max_x, 0, max_y );
#endif
}


checklist_t::checklist_t(uint32 _ss, uint32 _st, uint8 _nfc, uint32 _random_seed, uint16 _halt_entry, uint16 _line_entry, uint16 _convoy_entry, uint32 *_rands)
	: ss(_ss), st(_st), nfc(_nfc), random_seed(_random_seed), halt_entry(_halt_entry), line_entry(_line_entry), convoy_entry(_convoy_entry)
{
	for(  uint8 i = 0;  i < CHK_RANDS; i++  ) {
		rand[i]	 = _rands[i];
	}
}


void checklist_t::rdwr(memory_rw_t *buffer)
{
	buffer->rdwr_long(ss);
	buffer->rdwr_long(st);
	buffer->rdwr_byte(nfc);
	buffer->rdwr_long(random_seed);
	buffer->rdwr_short(halt_entry);
	buffer->rdwr_short(line_entry);
	buffer->rdwr_short(convoy_entry);

	// desync debug
	for(  uint8 i = 0;  i < CHK_RANDS;  i++  ) {
		buffer->rdwr_long(rand[i]);
	}
}



int checklist_t::print(char *buffer, const char *entity) const
{
	return sprintf(buffer, "%s=[ss=%u st=%u nfc=%u rand=%u halt=%u line=%u cnvy=%u ssr=%u,%u,%u,%u,%u,%u,%u,%u str=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u exr=%u,%u,%u,%u,%u,%u,%u,%u  ",
		entity, ss, st, nfc, random_seed, halt_entry, line_entry, convoy_entry,
		rand[0], rand[1], rand[2], rand[3], rand[4], rand[5], rand[6], rand[7],
		rand[8], rand[9], rand[10], rand[11], rand[12], rand[13], rand[14], rand[15], rand[16], rand[17], rand[18], rand[19], rand[20], rand[21], rand[22], rand[23],
		rand[24], rand[25], rand[26], rand[27], rand[28], rand[29], rand[30], rand[31]
	);
}


// changes the snowline height (for the seasons)
bool karte_t::recalc_snowline()
{
	static int mfactor[12] = { 99, 95, 80, 50, 25, 10, 0, 5, 20, 35, 65, 85 };
	static uint8 month_to_season[12] = { 2, 2, 2, 3, 3, 0, 0, 0, 0, 1, 1, 2 };

	// calculate snowline with day precision
	// use linear interpolation
	const sint64 ticks_this_month = get_zeit_ms() & (karte_t::ticks_per_world_month-1);
	const long faktor = (long) (mfactor[last_month] + (  ( (mfactor[(last_month+1)%12]-mfactor[last_month])*(ticks_this_month>>12) ) >> (karte_t::ticks_per_world_month_shift-12) ));

	// just remember them
	const sint16 old_snowline = snowline;
	const sint16 old_season = season;

	// and calculate new values
	season=month_to_season[last_month];   //  (2+last_month/3)&3; // summer always zero
	int const winterline = settings.get_winter_snowline();
	int const summerline = settings.get_climate_borders()[arctic_climate] + 1;
	snowline = summerline - (sint16)(((summerline-winterline)*faktor)/100);
	snowline = snowline + grundwasser;

	// changed => we update all tiles ...
	return (old_snowline!=snowline  ||  old_season!=season);
}


// read height data from bmp or ppm files
bool karte_t::get_height_data_from_file( const char *filename, sint8 grundwasser, sint8 *&hfield, sint16 &ww, sint16 &hh, bool update_only_values )
{
	if (FILE* const file = fopen(filename, "rb")) {
		char id[3];
		// parsing the header of this mixed file format is nottrivial ...
		id[0] = fgetc(file);
		id[1] = fgetc(file);
		id[2] = 0;
		if(strcmp(id, "P6")) {
			if(strcmp(id, "BM")) {
				fclose(file);
				dbg->error("karte_t::load_heightfield()","Heightfield has wrong image type %s instead P6/BM", id);
				return false;
			}
			// bitmap format
			fseek( file, 10, SEEK_SET );
			uint32 data_offset;
			sint32 w, h, format, table;
			sint16 bit_depth;
#ifdef SIM_BIG_ENDIAN
			uint32 l;
			uint16 s;
			fread( &l, 4, 1, file );
			data_offset = endian(l);
			fseek( file, 18, SEEK_SET );
			fread( &l, 4, 1, file );
			w = endian(l);
			fread( &l, 4, 1, file );
			h = endian(l);
			fseek( file, 28, SEEK_SET );
			fread( &s, 2, 1, file );
			bit_depth = endian(s);
			fread( &l, 4, 1, file );
			format = endian(l);
			fseek( file, 46, SEEK_SET );
			fread( &l, 4, 1, file );
			table = endian(l);
#else
			fread( &data_offset, 4, 1, file );
			fseek( file, 18, SEEK_SET );
			fread( &w, 4, 1, file );
			fread( &h, 4, 1, file );
			fseek( file, 28, SEEK_SET );
			fread( &bit_depth, 2, 1, file );
			fread( &format, 4, 1, file );
			fseek( file, 46, SEEK_SET );
			fread( &table, 4, 1, file );
#endif
			if((bit_depth!=8  &&  bit_depth!=24)  ||  format>1) {
				if(!update_only_values) {
					dbg->fatal("karte_t::get_height_data_from_file()","Can only use 8Bit (RLE or normal) or 24 bit bitmaps!");
				}
				fclose( file );
				return false;
			}

			// skip parsing body
			if(update_only_values) {
				ww = w;
				hh = abs(h);
				return true;
			}

			// now read the data and convert them on the fly
			hfield = new sint8[w*h];
			memset( hfield, grundwasser, w*h );
			if(bit_depth==8) {
				// convert color tables to height levels
				if(table==0) {
					table = 256;
				}
				sint8 h_table[256];
				fseek( file, 54, SEEK_SET );
				for( int i=0;  i<table;  i++  ) {
					int B = fgetc(file);
					int G = fgetc(file);
					int R = fgetc(file);
					fgetc(file);	// dummy
					h_table[i] = ((env_t::pak_height_conversion_factor*((R*2+G*3+B)/4 - 224)) & 0xFFF0)/16;
				}
				// now read the data
				fseek( file, data_offset, SEEK_SET );
				if(format==0) {
					// uncompressed (usually mirrored, if h>0)
					bool mirror = (h<0);
					h = abs(h);
					for(  sint32 y=0;  y<h;  y++  ) {
						sint32 offset = mirror ? y*w : (h-y-1)*w;
						for(  sint32 x=0;  x<w;  x++  ) {
							hfield[x+offset] = h_table[fgetc(file)];
						}
						// skip line offset
						if(w&1) {
							fgetc(file);
						}
					}
				}
				else {
					// compressed RLE (reverse y, since mirrored)
					sint32 x=0, y=h-1;
					while (!feof(file)) {
						uint8 Count= fgetc(file);
						uint8 ColorIndex = fgetc(file);

						if (Count > 0) {
							for( sint32 k = 0;  k < Count;  k++, x++  ) {
								hfield[x+(y*w)] = h_table[ColorIndex];
							}
						} else if (Count == 0) {
							sint32 Flag = ColorIndex;
							if (Flag == 0) {
								// goto next line
								x = 0;
								y--;
							}
							else if (Flag == 1) {
								// end of bitmap
								break;
							}
							else if (Flag == 2) {
								// skip with cursor
								x += (uint8)fgetc(file);
								y -= (uint8)fgetc(file);
							}
							else {
								// uncompressed run
								Count = Flag;
								for( sint32 k = 0;  k < Count;  k++, x++  ) {
									hfield[x+y*w] = h_table[(uint8)fgetc(file)];
								}
								if (ftell(file) & 1) {	// alway even offset in file
									fseek(file, 1, SEEK_CUR);
								}
							}
						}
					}
				}
			}
			else {
				// uncompressed 24 bits
				bool mirror = (h<0);
				h = abs(h);
				for(  sint32 y=0;  y<h;  y++  ) {
					sint32 offset = mirror ? y*w : (h-y-1)*w;
					for(  sint32 x=0;  x<w;  x++  ) {
						int B = fgetc(file);
						int G = fgetc(file);
						int R = fgetc(file);
						hfield[x+offset] = ((env_t::pak_height_conversion_factor*((R*2+G*3+B)/4 - 224)) & 0xFFF0)/16;
					}
					fseek( file, (4-((w*3)&3))&3, SEEK_CUR );	// skip superfluos bytes at the end of each scanline
				}
			}
			// success ...
			fclose(file);
			ww = w;
			hh = h;
			return true;
		}
		else {
			// ppm format
			char buf[255];
			char *c = id+2;
			sint32 param[3]={0,0,0};
			for(int index=0;  index<3;  ) {
				// the format is "P6[whitespace]width[whitespace]height[[whitespace bitdepth]]newline]
				// however, Photoshop is the first program, that uses space for the first whitespace ...
				// so we cater for Photoshop too
				while(*c  &&  *c<=32) {
					c++;
				}
				// usually, after P6 there comes a comment with the maker
				// but comments can be anywhere
				if(*c==0) {
					read_line(buf, sizeof(buf), file);
					c = buf;
					continue;
				}
				param[index++] = atoi(c);
				while(*c>='0'  &&  *c<='9') {
					c++;
				}
			}
			// now the data
			sint32 w = param[0];
			sint32 h = param[1];
			if(param[2]!=255) {
				fclose(file);
				if(!update_only_values) {
					dbg->fatal("karte_t::load_heightfield()","Heightfield has wrong color depth %d", param[2] );
				}
				return false;
			}

			// report only values
			if(update_only_values) {
				fclose(file);
				ww = w;
				hh = h;
				return true;
			}

			// ok, now read them in
			hfield = new sint8[w*h];
			memset( hfield, grundwasser, w*h );

			for(sint16 y=0; y<h; y++) {
				for(sint16 x=0; x<w; x++) {
					int R = fgetc(file);
					int G = fgetc(file);
					int B = fgetc(file);
					hfield[x+(y*w)] =  ((env_t::pak_height_conversion_factor*((R*2+G*3+B)/4 - 224)) & 0xFFF0)/16;
				}
			}

			// success ...
			fclose(file);
			ww = w;
			hh = h;
			return true;
		}
	}
	return false;
}


/**
 * copy of settings needed for perlin height loop
 * set in enlarge map before calling loop
 */

settings_t* perlin_sets;

void karte_t::perlin_hoehe_loop( sint16 x_min, sint16 x_max, sint16 y_min, sint16 y_max )
{
	for(  int y = y_min;  y < y_max;  y++  ) {
		for(  int x = x_min; x < x_max;  x++  ) {
			// loop all tiles
			koord k(x,y);
			sint16 const h = perlin_hoehe(&settings, k, koord(0, 0));
			set_grid_hgt( k, (sint8) h);
			if(  is_within_limits(k)  &&  h > get_water_hgt(k)  ) {
				set_water_hgt(k, grundwasser-4);
			}
		}
	}
}


/**
 * Hoehe eines Punktes der Karte mit "perlin noise"
 *
 * @param frequency in 0..1.0 roughness, the higher the rougher
 * @param amplitude in 0..160.0 top height of mountains, may not exceed 160.0!!!
 * @author Hj. Malthaner
 */
sint32 karte_t::perlin_hoehe(settings_t const* const sets, koord k, koord const size, sint32 map_size_max)
{
	// Hajo: to Markus: replace the fixed values with your
	// settings. Amplitude is the top highness of the
	// montains, frequency is something like landscape 'roughness'
	// amplitude may not be greater than 160.0 !!!
	// please don't allow frequencies higher than 0.8 it'll
	// break the AI's pathfinding. Frequency values of 0.5 .. 0.7
	// seem to be ok, less is boring flat, more is too crumbled
	// the old defaults are given here: f=0.6, a=160.0
	switch( sets->get_rotation() ) {
		// 0: do nothing
		case 1: k = koord(k.y,size.x-k.x); break;
		case 2: k = koord(size.x-k.x,size.y-k.y); break;
		case 3: k = koord(size.y-k.y,k.x); break;
	}
//    double perlin_noise_2D(double x, double y, double persistence);
//    return ((int)(perlin_noise_2D(x, y, 0.6)*160.0)) & 0xFFFFFFF0;
	k = k + koord(sets->get_origin_x(), sets->get_origin_y());
	return ((int)(perlin_noise_2D(k.x, k.y, sets->get_map_roughness(), map_size_max)*(double)sets->get_max_mountain_height())) / 16;
}

sint32 karte_t::perlin_hoehe(settings_t const* const sets, koord k, koord const size)
{
	return perlin_hoehe(sets, k, size, cached_size_max);
}


void karte_t::cleanup_grounds_loop( sint16 x_min, sint16 x_max, sint16 y_min, sint16 y_max )
{
	for(  int y = y_min;  y < y_max;  y++  ) {
		for(  int x = x_min; x < x_max;  x++  ) {
			planquadrat_t *pl = access_nocheck(x,y);
			grund_t *gr = pl->get_kartenboden();
			koord k(x,y);
			uint8 slope = calc_natural_slope(k);
			sint8 height = min_hgt_nocheck(k);
			sint8 water_hgt = get_water_hgt_nocheck(k);
			if(  height == water_hgt - 1  ) {
				if(  max_hgt_nocheck(k) == water_hgt + 1  ) {
					const sint8 disp_hn_sw = max( height + corner1(slope), water_hgt );
					const sint8 disp_hn_se = max( height + corner2(slope), water_hgt );
					const sint8 disp_hn_ne = max( height + corner3(slope), water_hgt );
					const sint8 disp_hn_nw = max( height + corner4(slope), water_hgt );
					height = get_water_hgt_nocheck(k);
					slope = (disp_hn_sw - height) + ((disp_hn_se - height) * 3) + ((disp_hn_ne - height) * 9) + ((disp_hn_nw - height) * 27);
				}
			}

			gr->set_pos( koord3d( k, max( height, water_hgt ) ) );
			if(  gr->get_typ() != grund_t::wasser  &&  max_hgt_nocheck(k) <= water_hgt  ) {
				// below water but ground => convert
				pl->kartenboden_setzen( new wasser_t(gr->get_pos()) );
			}
			else if(  gr->get_typ() == grund_t::wasser  &&  max_hgt_nocheck(k) > water_hgt  ) {
				// water above ground => to ground
				pl->kartenboden_setzen( new boden_t(gr->get_pos(), slope ) );
			}
			else {
				gr->set_grund_hang( slope );
			}
		}
	}
}


void karte_t::cleanup_karte( int xoff, int yoff )
{
	// we need a copy to smoothen the map to a realistic level
	const sint32 grid_size = (get_size().x+1)*(sint32)(get_size().y+1);
	sint8 *grid_hgts_cpy = new sint8[grid_size];
	memcpy( grid_hgts_cpy, grid_hgts, grid_size );

	// the trick for smoothing is to raise each tile by one
	sint32 i,j;
	for(j=0; j<=get_size().y; j++) {
		for(i=j>=yoff?0:xoff; i<=get_size().x; i++) {
			raise_grid_to(i,j, grid_hgts_cpy[i+j*(get_size().x+1)] + 1);
		}
	}
	delete [] grid_hgts_cpy;

	// but to leave the map unchanged, we lower the height again
	for(j=0; j<=get_size().y; j++) {
		for(i=j>=yoff?0:xoff; i<=get_size().x; i++) {
			grid_hgts[i+j*(get_size().x+1)] --;
		}
	}

	if(  xoff==0 && yoff==0  ) {
		world_xy_loop(&karte_t::cleanup_grounds_loop, 0);
	}
	else {
		cleanup_grounds_loop( 0, get_size().x, yoff, get_size().y );
		cleanup_grounds_loop( xoff, get_size().x, 0, yoff );
	}
}


void karte_t::destroy()
{
	is_sound = false; // karte_t::play_sound_area_clipped needs valid zeiger
DBG_MESSAGE("karte_t::destroy()", "destroying world");

	is_shutting_down = true;

	passenger_origins.clear();
	commuter_targets.clear();
	visitor_targets.clear();
	mail_origins_and_targets.clear();

	uint32 max_display_progress = 256+stadt.get_count()*10 + haltestelle_t::get_alle_haltestellen().get_count() + convoi_array.get_count() + (cached_size.x*cached_size.y)*2;
	uint32 old_progress = 0;

	loadingscreen_t ls( translator::translate("Destroying map ..."), max_display_progress, true );

	// rotate the map until it can be saved
	nosave_warning = false;
	if(  nosave  ) {
		max_display_progress += 256;
		for( int i=0;  i<4  &&  nosave;  i++  ) {
	DBG_MESSAGE("karte_t::destroy()", "rotating");
			rotate90();
		}
		old_progress += 256;
		ls.set_max( max_display_progress );
		ls.set_progress( old_progress );
	}
	if(nosave) {
		dbg->fatal( "karte_t::destroy()","Map cannot be cleanly destroyed in any rotation!" );
	}

DBG_MESSAGE("karte_t::destroy()", "label clear");
	labels.clear();

	if(zeiger) {
		zeiger->set_pos(koord3d::invalid);
		delete zeiger;
		zeiger = NULL;
	}

	old_progress += 256;
	ls.set_progress( old_progress );

	// alle convois aufraeumen
	while (!convoi_array.empty()) {
		convoihandle_t cnv = convoi_array.back();
		cnv->destroy();
		old_progress ++;
		if(  (old_progress&0x00FF) == 0  ) {
			ls.set_progress( old_progress );
		}
	}
	convoi_array.clear();
DBG_MESSAGE("karte_t::destroy()", "convois destroyed");

	// alle haltestellen aufraeumen
	old_progress += haltestelle_t::get_alle_haltestellen().get_count();
	haltestelle_t::destroy_all();
DBG_MESSAGE("karte_t::destroy()", "stops destroyed");
	ls.set_progress( old_progress );

	// delete towns first (will also delete all their houses)
	// for the next game we need to remember the desired number ...
	sint32 const no_of_cities = settings.get_anzahl_staedte();
	for(  uint32 i=0;  !stadt.empty();  i++  ) {
		rem_stadt(stadt.front());
		old_progress += 10;
		if(  (i&0x00F) == 0  ) {
			ls.set_progress( old_progress );
		}
	}
	settings.set_anzahl_staedte(no_of_cities);

DBG_MESSAGE("karte_t::destroy()", "towns destroyed");

	ls.set_progress( old_progress );
	old_progress += cached_size.x*cached_size.y;

	// removes all moving stuff from the sync_step
	while(!sync_list.empty()) {
#ifndef SYNC_VECTOR
		sync_steppable *ss = sync_list.remove_first();
		delete ss;
#else
		delete sync_list.back();
#endif
	}
	sync_list.clear();

	// now remove all pedestrians too ...
	while(!sync_eyecandy_list.empty()) {
		sync_steppable *ss = sync_eyecandy_list.remove_first();
		delete ss;
	}

	while(!sync_way_eyecandy_list.empty()) {
#ifndef SYNC_VECTOR
		sync_steppable *ss = sync_way_eyecandy_list.remove_first();
		delete ss;
#else
		delete sync_way_eyecandy_list.back();
#endif
	}

	ls.set_progress( old_progress );
DBG_MESSAGE("karte_t::destroy()", "sync list cleared");

	// dinge aufraeumen
	cached_grid_size.x = cached_grid_size.y = 1;
	cached_size.x = cached_size.y = 0;
	if(plan) {
		delete [] plan;
		plan = NULL;
	}
	DBG_MESSAGE("karte_t::destroy()", "planquadrat destroyed");

	old_progress += (cached_size.x*cached_size.y)/2;
	ls.set_progress( old_progress );

	// gitter aufraeumen
	if(grid_hgts) {
		delete [] grid_hgts;
		grid_hgts = NULL;
	}

	if(  water_hgts  ) {
		delete [] water_hgts;
		water_hgts = NULL;
	}

	// spieler aufraeumen
	for(int i=0; i<MAX_PLAYER_COUNT; i++) {
		if(spieler[i]) {
			delete spieler[i];
			spieler[i] = NULL;
		}
	}
DBG_MESSAGE("karte_t::destroy()", "player destroyed");

	old_progress += (cached_size.x*cached_size.y)/4;
	ls.set_progress( old_progress );

	// alle fabriken aufraeumen
	// Clean up all factories
	FOR(vector_tpl<fabrik_t*>, const f, fab_list) {
		delete f;
	}
	fab_list.clear();
DBG_MESSAGE("karte_t::destroy()", "factories destroyed");

	// hier nur entfernen, aber nicht loeschen
	ausflugsziele.clear();
DBG_MESSAGE("karte_t::destroy()", "attraction list destroyed");

	delete scenario;
	scenario = NULL;

	senke_t::neue_karte();
	pumpe_t::neue_karte();

	bool empty_depot_list = depot_t::get_depot_list().empty();
	assert( empty_depot_list );

DBG_MESSAGE("karte_t::destroy()", "world destroyed");
	
	is_shutting_down = false;

	// Added by : B.Gabriel
	route_t::TERM_NODES();

	// Added by : Knightly
	path_explorer_t::finalise();

	dbg->important("World destroyed.");
}


void karte_t::add_convoi(convoihandle_t const &cnv)
{
	assert(cnv.is_bound());
	convoi_array.append_unique(cnv);
}


void karte_t::rem_convoi(convoihandle_t const &cnv)
{
	convoi_array.remove(cnv);
}


void karte_t::add_stadt(stadt_t *s)
{
	settings.set_anzahl_staedte(settings.get_anzahl_staedte() + 1);
	stadt.append(s, s->get_einwohner());
}


bool karte_t::rem_stadt(stadt_t *s)
{
	if(s == NULL  ||  stadt.empty()) {
		// no town there to delete ...
		return false;
	}

	// reduce number of towns
	if(s->get_name()) {
		DBG_MESSAGE("karte_t::rem_stadt()", "%s", s->get_name());
	}
	stadt.remove(s);
	DBG_DEBUG4("karte_t::rem_stadt()", "reduce city to %i", settings.get_anzahl_staedte() - 1);
	settings.set_anzahl_staedte(settings.get_anzahl_staedte() - 1);

	// ok, we can delete this
	DBG_MESSAGE("karte_t::rem_stadt()", "delete" );
	delete s;

	return true;
}


// just allocates space;
void karte_t::init_felder()
{
	assert(plan==0);

	uint32 const x = get_size().x;
	uint32 const y = get_size().y;
	plan      = new planquadrat_t[x * y];
	grid_hgts = new sint8[(x + 1) * (y + 1)];
	MEMZERON(grid_hgts, (x + 1) * (y + 1));
	water_hgts = new sint8[x * y];
	MEMZERON(water_hgts, x * y);

	win_set_world( this );
	reliefkarte_t::get_karte()->init();

	for(int i=0; i<MAX_PLAYER_COUNT ; i++) {
		// old default: AI 3 passenger, other goods
		spieler[i] = (i<2) ? new spieler_t(this,i) : NULL;
	}
	active_player = spieler[0];
	active_player_nr = 0;

	// defaults without timeline
	average_speed[0] = 60;
	average_speed[1] = 80;
	average_speed[2] = 40;
	average_speed[3] = 350;

	// clear world records
	records->clear_speed_records();

	// make timer loop invalid
	for( int i=0;  i<32;  i++ ) {
		last_frame_ms[i] = 0x7FFFFFFFu;
		last_step_nr[i] = 0xFFFFFFFFu;
	}
	last_frame_idx = 0;
	pending_season_change = 0;

	// init global history
	for (int year=0; year<MAX_WORLD_HISTORY_YEARS; year++) {
		for (int cost_type=0; cost_type<MAX_WORLD_COST; cost_type++) {
			finance_history_year[year][cost_type] = 0;
		}
	}
	for (int month=0; month<MAX_WORLD_HISTORY_MONTHS; month++) {
		for (int cost_type=0; cost_type<MAX_WORLD_COST; cost_type++) {
			finance_history_month[month][cost_type] = 0;
		}
	}
	last_month_bev = 0;

	tile_counter = 0;

	convoihandle_t::init( 1024 );
	linehandle_t::init( 1024 );

	halthandle_t::init( 1024 );

	vehikel_basis_t::set_overtaking_offsets( get_settings().is_drive_left() );

	scenario = new scenario_t(this);

	nosave_warning = nosave = false;

	if (env_t::server) {
		nwc_auth_player_t::init_player_lock_server(this);
	}
}


void karte_t::set_scenario(scenario_t *s)
{
	if (scenario != s) {
		delete scenario;
	}
	scenario = s;
}


void karte_t::create_rivers( sint16 number )
{
	// First check, wether there is a canal:
	const weg_besch_t* river_besch = wegbauer_t::get_besch( env_t::river_type[env_t::river_types-1], 0 );
	if(  river_besch == NULL  ) {
		// should never reaching here ...
		dbg->warning("karte_t::create_rivers()","There is no river defined!\n");
		return;
	}

	// create a vector of the highest points
	vector_tpl<koord> water_tiles;
	weighted_vector_tpl<koord> mountain_tiles;

	koord last_koord(0,0);

	// trunk of 16 will ensure that rivers are long enough apart ...
	for(  sint16 y = 8;  y < cached_size.y;  y+=16  ) {
		for(  sint16 x = 8;  x < cached_size.x;  x+=16  ) {
			koord k(x,y);
			grund_t *gr = lookup_kartenboden_nocheck(k);
			const sint8 h = gr->get_hoehe() - get_water_hgt_nocheck(k);
			if(  gr->ist_wasser()  ) {
				// may be good to start a river here
				water_tiles.append(k);
			}
			else {
				mountain_tiles.append( k, h * h );
			}
		}
	}
	if (water_tiles.empty()) {
		dbg->message("karte_t::create_rivers()","There aren't any water tiles!\n");
		return;
	}

	// now make rivers
	int river_count = 0;
	sint16 retrys = number*2;
	while(  number > 0  &&  !mountain_tiles.empty()  &&  retrys>0  ) {

		// start with random coordinates
		koord const start = pick_any_weighted(mountain_tiles);
		mountain_tiles.remove( start );

		// build a list of matchin targets
		vector_tpl<koord> valid_water_tiles;

		for(  uint32 i=0;  i<water_tiles.get_count();  i++  ) {
			sint16 dist = koord_distance(start,water_tiles[i]);
			if(  settings.get_min_river_length() < dist  &&  dist < settings.get_max_river_length()  ) {
				valid_water_tiles.append( water_tiles[i] );
			}
		}

		// now try 512 random locations
		for(  sint32 i=0;  i<512  &&  !valid_water_tiles.empty();  i++  ) {
			koord const end = pick_any(valid_water_tiles);
			valid_water_tiles.remove( end );
			wegbauer_t riverbuilder(spieler[1]);
			riverbuilder.route_fuer(wegbauer_t::river, river_besch);
			sint16 dist = koord_distance(start,end);
			riverbuilder.set_maximum( dist*50 );
			riverbuilder.calc_route( lookup_kartenboden(end)->get_pos(), lookup_kartenboden(start)->get_pos() );
			if(  riverbuilder.get_count() >= (uint32)settings.get_min_river_length()  ) {
				// do not built too short rivers
				riverbuilder.baue();
				river_count++;
				number--;
				retrys++;
				break;
			}
		}

		retrys--;
	}
	// we gave up => tell the user
	if(  number>0  ) {
		dbg->warning( "karte_t::create_rivers()","Too many rivers requested! (only %i rivers placed)", river_count );
	}
}

void karte_t::remove_queued_city(stadt_t* city)
{
	cities_awaiting_private_car_route_check.remove(city);
}

void karte_t::add_queued_city(stadt_t* city)
{
	cities_awaiting_private_car_route_check.append(city);
}

void karte_t::distribute_cities( settings_t const * const sets, sint16 old_x, sint16 old_y)
{
	sint32 new_anzahl_staedte = abs(sets->get_anzahl_staedte());

	const uint32 number_of_big_cities = env_t::number_of_big_cities;

	const uint32 max_city_size = sets->get_max_city_size();
	const uint32 max_small_city_size = sets->get_max_small_city_size();

	dbg->important("Creating cities ...");
	DBG_DEBUG("karte_t::distribute_groundobjs_cities()","prepare cities sizes");

	const sint32 city_population_target_count = stadt.empty() ? new_anzahl_staedte : new_anzahl_staedte + stadt.get_count() + 1;

	vector_tpl<sint32> city_population(city_population_target_count);
	sint32 median_population = abs(sets->get_mittlere_einwohnerzahl());

	// Generate random sizes to fit a Pareto distribution: P(x) = x_m / x^2 dx.
	// This ensures that Zipf's law is satisfied in a random fashion, and
	// arises from the observation that city distribution is self-similar.
	// The median of a Pareto distribution is twice the lower cut-off, x_m.
	// We can generate a Pareto deviate from a uniform deviate on range [0,1)
	// by taking m_x/u where u is the uniform deviate.

	while(city_population.get_count() < city_population_target_count) {
		uint32 population;
		do {
			uint32 rand;
			do {
				rand = simrand_plain();
			} while (rand == 0);

			population = ((double)median_population / 2) / ((double)rand / 0xffffffff);
		} while ( city_population.get_count() <  number_of_big_cities && (population <= max_small_city_size  || population > max_city_size) ||
			  city_population.get_count() >= number_of_big_cities &&  population >  max_small_city_size );

		city_population.insert_ordered( population, std::greater<sint32>() );
	}


#ifdef DEBUG
	for (sint32 i =0; i< city_population_target_count; i++) 
	{
		DBG_DEBUG("karte_t::distribute_groundobjs_cities()", "City rank %d -- %d", i, city_population[i]);
	}

	DBG_DEBUG("karte_t::distribute_groundobjs_cities()","prepare cities");
#endif

	vector_tpl<koord> *pos = stadt_t::random_place(this, &city_population, old_x, old_y);

	if ( pos->empty() ) {
		// could not generate any town
		if(pos) {
			delete pos;
		}
		settings.set_anzahl_staedte(stadt.get_count()); // new number of towns (if we did not find enough positions)
		return;
	}
		// Extra indentation here is to allow for better diff files; it used to be in a block

		const sint32 old_anzahl_staedte = stadt.get_count();
		if (pos->get_count() < new_anzahl_staedte) {
			new_anzahl_staedte = pos->get_count();
			// Under no circumstances increase the number of new cities!
		}
		dbg->important("Creating cities: %d", new_anzahl_staedte);

		// prissi if we could not generate enough positions ...
		settings.set_anzahl_staedte(old_anzahl_staedte);
		int old_progress = 16;

		// Ansicht auf erste Stadt zentrieren
		if(  old_x+old_y == 0  ) {
			viewport->change_world_position( koord3d((*pos)[0], min_hgt((*pos)[0])) );
		}
		uint32 max_progress = 16 + 2 * (old_anzahl_staedte + new_anzahl_staedte) + 2 * new_anzahl_staedte + (old_x == 0 ? settings.get_factory_count() : 0);
		loadingscreen_t ls( translator::translate( "distributing cities" ), max_progress, true, true );

		{
			// Loop only new cities:
			uint32 tbegin = dr_time();
			for(  unsigned i=0;  i<new_anzahl_staedte;  i++  ) {
				stadt_t* s = new stadt_t(spieler[1], (*pos)[i], 1 );
				DBG_DEBUG("karte_t::distribute_groundobjs_cities()","Erzeuge stadt %i with %ld inhabitants",i,(s->get_city_history_month())[HIST_CITICENS] );
				if (s->get_buildings() > 0) {
					add_stadt(s);
				}
				else {
					delete(s);
				}
			}

			delete pos;
			DBG_DEBUG("karte_t::distribute_groundobjs_cities()","took %lu ms for all towns", dr_time()-tbegin );

			uint32 game_start = current_month;
			// townhalls available since?
			FOR(vector_tpl<haus_besch_t const*>, const besch, *hausbauer_t::get_list(haus_besch_t::rathaus)) {
				uint32 intro_year_month = besch->get_intro_year_month();
				if(  intro_year_month<game_start  ) {
					game_start = intro_year_month;
				}
			}
			// streets since when?
			game_start = max( game_start, wegbauer_t::get_earliest_way(road_wt)->get_intro_year_month() );

			uint32 original_start_year = current_month;
			uint32 original_industry_gorwth = settings.get_industry_increase_every();
			settings.set_industry_increase_every( 0 );

			for(  uint32 i=old_anzahl_staedte;  i<stadt.get_count();  i++  ) {
				// Hajo: do final init after world was loaded/created
				stadt[i]->laden_abschliessen();

				const uint32 citizens = city_population.get_count() > i ? city_population[i] : city_population.get_element(simrand(city_population.get_count() - 1, "void karte_t::distribute_groundobjs_cities"));

				sint32 diff = (original_start_year-game_start)/2;
				sint32 growth = 32;
				sint32 current_bev = stadt[i]->get_einwohner();

				/* grow gradually while aging
				 * the difference to the current end year will be halved,
				 * while the growth step is doubled
				 */
				current_month = game_start;
				bool not_updated = false;
				bool new_town = true;
				while(  current_bev < citizens  ) {
					growth = min( citizens-current_bev, growth*2 );
					current_bev = stadt[i]->get_einwohner();
					stadt[i]->change_size( growth, new_town );
					// Only "new" for the first change_size call
					new_town = false;
					if(  current_bev > citizens/2  &&  not_updated  ) {
						ls.set_progress( ++old_progress );
						not_updated = true;
					}
					current_month += diff;
					diff >>= 1;
				}

				// the growth is slow, so update here the progress bar
				ls.set_progress( ++old_progress );
			}

			current_month = original_start_year;
			settings.set_industry_increase_every( original_industry_gorwth );
			msg->clear();
		}

		finance_history_year[0][WORLD_TOWNS] = finance_history_month[0][WORLD_TOWNS] = stadt.get_count();
		finance_history_year[0][WORLD_CITICENS] = finance_history_month[0][WORLD_CITICENS] = 0;
		finance_history_year[0][WORLD_JOBS] = finance_history_month[0][WORLD_JOBS] = 0;
		finance_history_year[0][WORLD_VISITOR_DEMAND] = finance_history_month[0][WORLD_VISITOR_DEMAND] = 0;

		FOR(weighted_vector_tpl<stadt_t*>, const city, stadt) 
		{
			finance_history_year[0][WORLD_CITICENS] += city->get_finance_history_month(0, HIST_CITICENS);  
			finance_history_month[0][WORLD_CITICENS] += city->get_finance_history_year(0, HIST_CITICENS);

			finance_history_month[0][WORLD_JOBS] += city->get_finance_history_month(0, HIST_JOBS);
			finance_history_year[0][WORLD_JOBS] += city->get_finance_history_year(0, HIST_JOBS);

			finance_history_month[0][WORLD_VISITOR_DEMAND] += city->get_finance_history_month(0, HIST_VISITOR_DEMAND);
			finance_history_year[0][WORLD_VISITOR_DEMAND] += city->get_finance_history_year(0, HIST_VISITOR_DEMAND);
		}

		

		// Hajo: connect some cities with roads
		ls.set_what(translator::translate("Connecting cities ..."));
		weg_besch_t const* besch = settings.get_intercity_road_type(get_timeline_year_month());
		if(besch == NULL) 
		{
			// Hajo: try some default (might happen with timeline ... )
			besch = wegbauer_t::weg_search(road_wt,80,5,get_timeline_year_month(),weg_t::type_flat);
		}

		wegbauer_t bauigel (NULL);
		bauigel.route_fuer(wegbauer_t::strasse | wegbauer_t::terraform_flag, besch, tunnelbauer_t::find_tunnel(road_wt,15,get_timeline_year_month()), brueckenbauer_t::find_bridge(road_wt,15,get_timeline_year_month()) );
		bauigel.set_keep_existing_ways(true);
		bauigel.set_maximum(env_t::intercity_road_length);

		// **** intercity road construction
		int count = 0;
		sint32 const n_cities  = settings.get_anzahl_staedte();
		int    const max_count = n_cities * (n_cities - 1) / 2 - old_anzahl_staedte * (old_anzahl_staedte - 1) / 2;
		// something to do??
		if(  max_count > 0  ) {
			// print("Building intercity roads ...\n");
			ls.set_max( 16 + 2 * (old_anzahl_staedte + new_anzahl_staedte) + 2 * new_anzahl_staedte + (old_x == 0 ? settings.get_factory_count() : 0) );
			// find townhall of city i and road in front of it
			vector_tpl<koord3d> k;
			for (int i = 0;  i < settings.get_anzahl_staedte(); ++i) {
				koord k1(stadt[i]->get_townhall_road());
				if (lookup_kartenboden(k1)  &&  lookup_kartenboden(k1)->hat_weg(road_wt)) {
					k.append(lookup_kartenboden(k1)->get_pos());
				}
				else {
					// look for a road near the townhall
					gebaeude_t const* const gb = obj_cast<gebaeude_t>(lookup_kartenboden(stadt[i]->get_pos())->first_obj());
					bool ok = false;
					if(  gb  &&  gb->ist_rathaus()  ) {
						koord k_check = stadt[i]->get_pos() + koord(-1,-1);
						const koord size = gb->get_tile()->get_besch()->get_groesse(gb->get_tile()->get_layout());
						koord inc(1,0);
						// scan all adjacent tiles, take the first that has a road
						for(sint32 i=0; i<2*size.x+2*size.y+4  &&  !ok; i++) {
							grund_t *gr = lookup_kartenboden(k_check);
							if (gr  &&  gr->hat_weg(road_wt)) {
								k.append(gr->get_pos());
								ok = true;
							}
							k_check = k_check + inc;
							if (i==size.x+1) {
								inc = koord(0,1);
							}
							else if (i==size.x+size.y+2) {
								inc = koord(-1,0);
							}
							else if (i==2*size.x+size.y+3) {
								inc = koord(0,-1);
							}
						}
					}
					if (!ok) {
						k.append( koord3d::invalid );
					}
				}
			}
			// compute all distances
			uint8 conn_comp=1; // current connection component for phase 0
			vector_tpl<uint8> city_flag; // city already connected to the graph? >0 nr of connection component
			array2d_tpl<sint32> city_dist(settings.get_anzahl_staedte(), settings.get_anzahl_staedte());
			for (sint32 i = 0; i < settings.get_anzahl_staedte(); ++i) {
				city_dist.at(i,i) = 0;
				for (sint32 j = i + 1; j < settings.get_anzahl_staedte(); ++j) {
					city_dist.at(i,j) = koord_distance(k[i], k[j]);
					city_dist.at(j,i) = city_dist.at(i,j);
					// count unbuildable connections to new cities
					if(  j>=old_anzahl_staedte && city_dist.at(i,j) >= env_t::intercity_road_length  ) {
						count++;
					}
				}
				city_flag.append( i < old_anzahl_staedte ? conn_comp : 0 );

				// progress bar stuff
				ls.set_progress( 16 + 2 * new_anzahl_staedte + count * settings.get_anzahl_staedte() * 2 / max_count );
			}
			// mark first town as connected
			if (old_anzahl_staedte==0) {
				city_flag[0]=conn_comp;
			}

			// get a default vehikel
			route_t verbindung;
			vehikel_t* test_driver;
			vehikel_besch_t test_drive_besch(road_wt, 500, vehikel_besch_t::diesel );
			test_driver = vehikelbauer_t::baue(koord3d(), spieler[1], NULL, &test_drive_besch);
			test_driver->set_flag( obj_t::not_on_map );

			bool ready=false;
			uint8 phase=0;
			// 0 - first phase: built minimum spanning tree (edge weights: city distance)
			// 1 - second phase: try to complete the graph, avoid edges that
			// == have similar length then already existing connection
			// == lead to triangles with an angle >90 deg

			while( phase < 2  ) {
				ready = true;
				koord conn = koord::invalid;
				sint32 best = env_t::intercity_road_length;

				if(  phase == 0  ) {
					// loop over all unconnected cities
					for (int i = 0; i < settings.get_anzahl_staedte(); ++i) {
						if(  city_flag[i] == conn_comp  ) {
							// loop over all connections to connected cities
							for (int j = old_anzahl_staedte; j < settings.get_anzahl_staedte(); ++j) {
								if(  city_flag[j] == 0  ) {
									ready=false;
									if(  city_dist.at(i,j) < best  ) {
										best = city_dist.at(i,j);
										conn = koord(i,j);
									}
								}
							}
						}
					}
					// did we completed a connection component?
					if(  !ready  &&  best == env_t::intercity_road_length  ) {
						// next component
						conn_comp++;
						// try the first not connected city
						ready = true;
						for(  int i = old_anzahl_staedte;  i < settings.get_anzahl_staedte();  ++i  ) {
							if(  city_flag[i] ==0  ) {
								city_flag[i] = conn_comp;
								ready = false;
								break;
							}
						}
					}
				}
				else {
					// loop over all unconnected cities
					for (int i = 0; i < settings.get_anzahl_staedte(); ++i) {
						for (int j = max(old_anzahl_staedte, i + 1);  j < settings.get_anzahl_staedte(); ++j) {
							if(  city_dist.at(i,j) < best  &&  city_flag[i] == city_flag[j]  ) {
								bool ok = true;
								// is there a connection i..l..j ? forbid stumpfe winkel
								for (int l = 0; l < settings.get_anzahl_staedte(); ++l) {
									if(  city_flag[i] == city_flag[l]  &&  city_dist.at(i,l) == env_t::intercity_road_length  &&  city_dist.at(j,l) == env_t::intercity_road_length  ) {
										// cosine < 0 ?
										koord3d d1 = k[i]-k[l];
										koord3d d2 = k[j]-k[l];
										if(  d1.x*d2.x + d1.y*d2.y < 0  ) {
											city_dist.at(i,j) = env_t::intercity_road_length+1;
											city_dist.at(j,i) = env_t::intercity_road_length+1;
											ok = false;
											count ++;
											break;
										}
									}
								}
								if(ok) {
									ready = false;
									best = city_dist.at(i,j);
									conn = koord(i,j);
								}
							}
						}
					}
				}
				// valid connection?
				if(  conn.x >= 0  ) {
					// is there a connection already
					const bool connected = (  phase==1  &&  verbindung.calc_route( this, k[conn.x], k[conn.y], test_driver, 0, 0, 0 )  );
					// build this connestion?
					bool build = false;
					// set appropriate max length for way builder
					if(  connected  ) {
						if(  2*verbindung.get_count() > (uint32)city_dist.at(conn)  ) {
							bauigel.set_maximum(verbindung.get_count() / 2);
							build = true;
						}
					}
					else {
						bauigel.set_maximum(env_t::intercity_road_length);
						build = true;
					}

					if(  build  ) {
						bauigel.calc_route(k[conn.x],k[conn.y]);
					}

					if(  build  &&  bauigel.get_count() >= 2  ) {
						bauigel.baue();
						if (phase==0) {
							city_flag[ conn.y ] = conn_comp;
						}
						// mark as built
						city_dist.at(conn) =  env_t::intercity_road_length;
						city_dist.at(conn.y, conn.x) =  env_t::intercity_road_length;
						count ++;
					}
					else {
						// do not try again
						city_dist.at(conn) =  env_t::intercity_road_length+1;
						city_dist.at(conn.y, conn.x) =  env_t::intercity_road_length+1;
						count ++;

						if(  phase == 0  ) {
							// do not try to connect to this connected component again
							for(  int i = 0;  i < settings.get_anzahl_staedte();  ++i  ) {
								if(  city_flag[i] == conn_comp  && city_dist.at(i, conn.y)<env_t::intercity_road_length) {
									city_dist.at(i, conn.y) =  env_t::intercity_road_length+1;
									city_dist.at(conn.y, i) =  env_t::intercity_road_length+1;
									count++;
								}
							}
						}
					}
				}

				// progress bar stuff
				ls.set_progress( 16 + 2 * new_anzahl_staedte + count * settings.get_anzahl_staedte() * 2 / max_count );

				// next phase?
				if(ready) {
					phase++;
					ready = false;
				}
			}
			delete test_driver;
		}
}

void karte_t::distribute_groundobjs_cities( settings_t const * const sets, sint16 old_x, sint16 old_y)
{
	DBG_DEBUG("karte_t::distribute_groundobjs_cities()","distributing groundobjs");

	if (env_t::river_types > 0 && settings.get_river_number() > 0) {
		create_rivers(settings.get_river_number());
	}

	sint32 new_anzahl_staedte = abs(sets->get_anzahl_staedte());
	// Do city and road creation if (and only if) cities were requested.
	if (new_anzahl_staedte > 0) {
		this->distribute_cities(sets, old_x, old_y);
	}

	DBG_DEBUG("karte_t::distribute_groundobjs_cities()","distributing groundobjs");
	if(  env_t::ground_object_probability > 0  ) {
		// add eyecandy like rocky, moles, flowers, ...
		koord k;
		const uint32 max_queried = env_t::ground_object_probability*2-1; 
		sint32 queried = simrand(max_queried, "karte_t::distribute_groundobjs_cities()");

		for(  k.y=0;  k.y<get_size().y;  k.y++  ) {
			for(  k.x=(k.y<old_y)?old_x:0;  k.x<get_size().x;  k.x++  ) {
				grund_t *gr = lookup_kartenboden_nocheck(k);
				if(  gr->get_typ()==grund_t::boden  &&  !gr->hat_wege()  ) {
					queried --;
					if(  queried<0  ) {
						const groundobj_besch_t *besch = groundobj_t::random_groundobj_for_climate( get_climate(k), gr->get_grund_hang() );
						if(besch) {
							queried = simrand(max_queried, "karte_t::distribute_groundobjs_cities()");
							gr->obj_add( new groundobj_t( gr->get_pos(), besch ) );
						}
					}
				}
			}
		}
	}

DBG_DEBUG("karte_t::distribute_groundobjs_cities()","distributing movingobjs");
	if(  env_t::moving_object_probability > 0  ) {
		// add animals and so on (must be done after growing and all other objects, that could change ground coordinates)
		koord k;

		bool has_water = movingobj_t::random_movingobj_for_climate( water_climate )!=NULL;	
		const uint32 max_queried = env_t::moving_object_probability*2-1; 
		sint32 queried = simrand(max_queried, "karte_t::distribute_groundobjs_cities()");
		// no need to test the borders, since they are mostly slopes anyway
		for(k.y=1; k.y<get_size().y-1; k.y++) {
			for(k.x=(k.y<old_y)?old_x:1; k.x<get_size().x-1; k.x++) {
				grund_t *gr = lookup_kartenboden_nocheck(k);
				// flat ground or open water
				if(  gr->get_top()==0  &&  (  (gr->get_typ()==grund_t::boden  &&  gr->get_grund_hang()==hang_t::flach)  ||  (has_water  &&  gr->ist_wasser())  )  ) {
					queried --;
					if(  queried<0  ) {
						const groundobj_besch_t *besch = movingobj_t::random_movingobj_for_climate( get_climate(k) );
						if(  besch  &&  ( besch->get_waytype() != water_wt  ||  gr->get_hoehe() <= get_water_hgt_nocheck(k) )  ) {
							if(besch->get_speed()!=0) {
								queried = simrand(max_queried, "karte_t::distribute_groundobjs_cities()");
								gr->obj_add( new movingobj_t( gr->get_pos(), besch ) );
							}
						}
					}
				}
			}
		}
	}
}


void karte_t::init(settings_t* const sets, sint8 const* const h_field)
{
	clear_random_mode( 7 );
	mute_sound(true);
	if (env_t::networkmode) {
		if (env_t::server) {
			network_reset_server();
		}
		else {
			network_core_shutdown();
		}
	}
	step_mode  = PAUSE_FLAG;
	intr_disable();

	if(plan) {
		destroy();

		// Added by : Knightly
		path_explorer_t::initialise(this);
	}

	for(  uint i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
		werkzeug[i] = werkzeug_t::general_tool[WKZ_ABFRAGE];
	}
	if(is_display_init()) {
		display_show_pointer(false);
	}
	viewport->change_world_position( koord(0,0), 0, 0 );

	settings = *sets;
	// names during creation time
	settings.set_name_language_iso(env_t::language_iso);
	settings.set_use_timeline(settings.get_use_timeline() & 1);

	ticks = 0;
	last_step_ticks = ticks;
	// ticks = 0x7FFFF800;  // Testing the 31->32 bit step

	last_month = 0;
	last_year = settings.get_starting_year();
	current_month = last_month + (last_year*12);
	set_ticks_per_world_month_shift(settings.get_bits_per_month());
	next_month_ticks =  karte_t::ticks_per_world_month;
	season=(2+last_month/3)&3; // summer always zero
	steps = 0;
	network_frame_count = 0;
	sync_steps = 0;
	map_counter = 0;
	recalc_average_speed();	// resets timeline

	grundwasser = (sint8)sets->get_grundwasser();      //29-Nov-01     Markus Weber    Changed

	init_height_to_climate();
	snowline = sets->get_winter_snowline() + grundwasser;

	if(sets->get_beginner_mode()) {
		warenbauer_t::set_multiplier(settings.get_beginner_price_factor(), settings.get_meters_per_tile());
		settings.set_just_in_time( 0 );
	}
	else {
		warenbauer_t::set_multiplier(1000, settings.get_meters_per_tile());
	}
	// Must do this just after set_multiplier, since it depends on warenbauer_t having registered all wares:
	settings.cache_speedbonuses();

	recalc_snowline();

	stadt.clear();

DBG_DEBUG("karte_t::init()","hausbauer_t::neue_karte()");
	// Call this before building cities
	hausbauer_t::neue_karte();

	cached_grid_size.x = 0;
	cached_grid_size.y = 0;

DBG_DEBUG("karte_t::init()","init_felder");
	init_felder();

	enlarge_map(&settings, h_field);

DBG_DEBUG("karte_t::init()","distributing trees");
	if (!settings.get_no_trees()) {
		baum_t::distribute_trees(3);
	}

DBG_DEBUG("karte_t::init()","built timeline");
	stadtauto_t::built_timeline_liste(this);

	nosave_warning = nosave = false;

	dbg->important("Creating factories ...");
	fabrikbauer_t::neue_karte();

	int consecutive_build_failures = 0;

	loadingscreen_t ls( translator::translate("distributing factories"), 16 + settings.get_anzahl_staedte() * 4 + settings.get_factory_count(), true, true );

	while(  fab_list.get_count() < (uint32)settings.get_factory_count()  ) {
		if(  !fabrikbauer_t::increase_industry_density( false )  ) {
			if(  ++consecutive_build_failures > 3  ) {
				// Industry chain building starts failing consecutively as map approaches full.
				break;
			}
		}
		else {
			consecutive_build_failures = 0;
		}
		ls.set_progress( 16 + settings.get_anzahl_staedte() * 4 + min(fab_list.get_count(),settings.get_factory_count()) );
	}

	settings.set_factory_count( fab_list.get_count() );
	finance_history_year[0][WORLD_FACTORIES] = finance_history_month[0][WORLD_FACTORIES] = fab_list.get_count();

	// tourist attractions
	ls.set_what(translator::translate("Placing attractions ..."));
	// Not worth actually constructing a progress bar, very fast
	fabrikbauer_t::verteile_tourist(settings.get_tourist_attractions());

	ls.set_what(translator::translate("Finalising ..."));
	// Not worth actually constructing a progress bar, very fast
	dbg->important("Preparing startup ...");
	if(zeiger == 0) {
		zeiger = new zeiger_t(koord3d::invalid, NULL );
	}

	// finishes the line preparation and sets id 0 to invalid ...
	spieler[0]->simlinemgmt.laden_abschliessen();

	set_werkzeug( werkzeug_t::general_tool[WKZ_ABFRAGE], get_active_player() );

	recalc_average_speed();

	// @author: jamespetts
	calc_generic_road_time_per_tile_city();
	calc_generic_road_time_per_tile_intercity();
	calc_max_road_check_depth();

	for (int i = 0; i < MAX_PLAYER_COUNT; i++) {
		if(  spieler[i]  ) {
			spieler[i]->set_active(settings.automaten[i]);
		}
	}

	active_player_nr = 0;
	active_player = spieler[0];
	werkzeug_t::update_toolbars();

	set_dirty();
	step_mode = PAUSE_FLAG;
	simloops = 60;
	reset_timer();

	if(is_display_init()) {
		display_show_pointer(true);
	}
	mute_sound(false);

	// Added by : Knightly
	path_explorer_t::full_instant_refresh();

	// Set the actual industry density and industry density proportion
	actual_industry_density = 0;
	uint32 weight;
	ITERATE(fab_list, i)
	{
		const fabrik_besch_t* factory_type = fab_list[i]->get_besch();
		if(!factory_type->is_electricity_producer())
		{
			// Power stations are excluded from the target weight:
			// a different system is used for them.
			weight = factory_type->get_gewichtung();
			actual_industry_density += (100 / weight);
		}
	}
	// The population is not counted at this point, so cannot set this here.
	industry_density_proportion = 0;

	settings.update_max_alternative_destinations_commuting(commuter_targets.get_sum_weight());
	settings.update_max_alternative_destinations_visiting(visitor_targets.get_sum_weight());
}

#define array_koord(px,py) (px + py * get_size().x)


/* Lakes:
 * For each height from grundwasser+1 to max_lake_height we loop over
 * all tiles in the map trying to increase water height to this value
 * To start with every tile in the map is checked - but when we fail for
 * a tile then it is excluded from subsequent checks
 */
void karte_t::create_lakes(  int xoff, int yoff  )
{
	if(  xoff > 0  ||  yoff > 0  ) {
		// too complicated to add lakes to an already existing world...
		return;
	}

	const sint8 max_lake_height = grundwasser + 8;
	const uint16 size_x = get_size().x;
	const uint16 size_y = get_size().y;

	sint8 *max_water_hgt = new sint8[size_x * size_y];
	memset( max_water_hgt, 1, sizeof(sint8) * size_x * size_y );

	sint8 *stage = new sint8[size_x * size_y];
	sint8 *new_stage = new sint8[size_x * size_y];
	sint8 *local_stage = new sint8[size_x * size_y];

	for(  sint8 h = grundwasser+1; h<max_lake_height; h++  ) {
		bool need_to_flood = false;
		memset( stage, -1, sizeof(sint8) * size_x * size_y );
		for(  uint16 y = 1;  y < size_y-1;  y++  ) {
			for(  uint16 x = 1;  x < size_x-1;  x++  ) {
				uint32 offset = array_koord(x,y);
				if(  max_water_hgt[offset]==1  &&  stage[offset]==-1  ) {

					sint8 hgt = lookup_hgt_nocheck( x, y );
					const sint8 water_hgt = water_hgts[offset]; // optimised <- get_water_hgt_nocheck(x, y);
					const sint8 new_water_hgt = max(hgt, water_hgt);
					if(  new_water_hgt>max_lake_height  ) {
						max_water_hgt[offset] = 0;
					}
					else if(  h>new_water_hgt  ) {
						koord k(x,y);
						memcpy( new_stage, stage, sizeof(sint8) * size_x * size_y );
						if(can_flood_to_depth(  k, h, new_stage, local_stage )) {
							sint8 *tmp_stage = new_stage;
							new_stage = stage;
							stage = tmp_stage;
							need_to_flood = true;
						}
						else {
							for(  uint16 iy = 1;  iy<size_y - 1;  iy++  ) {
								uint32 offset_end = array_koord(size_x - 1,iy);
								for(  uint32 local_offset = array_koord(0,iy);  local_offset<offset_end;  local_offset++  ) {
									if(  local_stage[local_offset] > -1  ) {
										max_water_hgt[local_offset] = 0;
									}
								}
							}
						}
					}
				}
			}
		}
		if(need_to_flood) {
			flood_to_depth(  h, stage  );
		}
		else {
			break;
		}
	}

	delete [] max_water_hgt;
	delete [] stage;
	delete [] new_stage;
	delete [] local_stage;

	for (planquadrat_t *pl = plan; pl < (plan + size_x * size_y); pl++) {
		pl->correct_water();
	}
}


bool karte_t::can_flood_to_depth(  koord k, sint8 new_water_height, sint8 *stage, sint8 *our_stage  ) const
{
	bool succeeded = true;
	if(  k == koord::invalid  ) {
		return false;
	}

	if(  new_water_height < get_grundwasser() - 3  ) {
		return false;
	}

	// make a list of tiles to change
	// cannot use a recursive method as stack is not large enough!

	sint8 *from_dir = new sint8[get_size().x * get_size().y];
	bool local_stage = (our_stage==NULL);

	if(  local_stage  ) {
		our_stage = new sint8[get_size().x * get_size().y];
	}

	memset( from_dir, -1, sizeof(sint8) * get_size().x * get_size().y );
	memset( our_stage, -1, sizeof(sint8) * get_size().x * get_size().y );
	uint32 offset = array_koord(k.x,k.y);
	stage[offset]=0;
	our_stage[offset]=0;
	do {
		for(  int i = our_stage[offset];  i < 8;  i++  ) {
			koord k_neighbour = k + koord::neighbours[i];
			if(  is_within_limits(k_neighbour)  ) {
				const uint32 neighbour_offset = array_koord(k_neighbour.x,k_neighbour.y);

				// already visited
				if(our_stage[neighbour_offset] != -1) goto next_neighbour;

				// water height above
				if(water_hgts[neighbour_offset] >= new_water_height) goto next_neighbour;

				grund_t *gr2 = lookup_kartenboden_nocheck(k_neighbour);
				if(  !gr2  ) goto next_neighbour;

				sint8 neighbour_height = gr2->get_hoehe();

				// land height above
				if(neighbour_height >= new_water_height) goto next_neighbour;

				//move on to next tile
				from_dir[neighbour_offset] = i;
				stage[neighbour_offset] = 0;
				our_stage[neighbour_offset] = 0;
				our_stage[offset] = i;
				k = k_neighbour;
				offset = array_koord(k.x,k.y);
				break;
			}
			else {
				// edge of map - we keep iterating so we can mark all connected tiles as failing
				succeeded = false;
			}
			next_neighbour:
			//return back to previous tile
			if(  i==7  ) {
				k = k - koord::neighbours[from_dir[offset]];
			}
		}
		offset = array_koord(k.x,k.y);
	} while(  from_dir[offset] != -1  );

	delete [] from_dir;

	if(  local_stage  ) {
		delete [] our_stage;
	}

	return succeeded;
}


void karte_t::flood_to_depth( sint8 new_water_height, sint8 *stage )
{
	const uint16 size_x = get_size().x;
	const uint16 size_y = get_size().y;

	uint32 offset_max = size_x*size_y;
	for(  uint32 offset = 0;  offset < offset_max;  offset++  ) {
		if(  stage[offset] == -1  ) {
			continue;
		}
		water_hgts[offset] = new_water_height;
	}
}


void karte_t::create_beaches(  int xoff, int yoff  )
{
	const uint16 size_x = get_size().x;
	const uint16 size_y = get_size().y;

//printf("%d: creating beaches\n",dr_time());
	// bays have wide beaches
	for(  uint16 iy = 0;  iy < size_y;  iy++  ) {
		for(  uint16 ix = (iy >= yoff - 19) ? 0 : max( xoff - 19, 0 );  ix < size_x;  ix++  ) {
			grund_t *gr = lookup_kartenboden_nocheck(ix,iy);
			if(  gr->ist_wasser()  && gr->get_hoehe()==grundwasser  ) {
				koord k( ix, iy );
				uint8 neighbour_water = 0;
				bool water[8];
				// check whether nearby tiles are water
				for(  int i = 0;  i < 8;  i++  ) {
					grund_t *gr2 = lookup_kartenboden( k + koord::neighbours[i] );
					water[i] = (!gr2  ||  gr2->ist_wasser());
				}

				// make a count of nearby tiles - where tiles on opposite (+-1 direction) sides are water these count much more so we don't block straits
				for(  int i = 0;  i < 8;  i++  ) {
					if(  water[i]  ) {
						neighbour_water++;
						if(  water[(i + 3) & 7]  ||  water[(i + 4) & 7]  ||  water[(i + 5) & 7]  ) {
							neighbour_water++;
						}
					}
				}

				// if not much nearby water then turn into a beach
				if(  neighbour_water < 4  ) {
					set_water_hgt( k, gr->get_hoehe() - 1 );
					raise_grid_to( ix, iy, gr->get_hoehe() );
					raise_grid_to( ix + 1, iy, gr->get_hoehe() );
					raise_grid_to( ix, iy + 1, gr->get_hoehe() );
					raise_grid_to( ix + 1, iy + 1 , gr->get_hoehe() );
					access_nocheck(k)->correct_water();
					access_nocheck(k)->set_climate( desert_climate );
				}
			}
		}
	}

//printf("%d: removing beaches from headlands\n",dr_time());
	// headlands should not have beaches at all
	for(  uint16 iy = 0;  iy < size_y;  iy++  ) {
		for(  uint16 ix = (iy >= yoff - 19) ? 0 : max( xoff - 19, 0 );  ix < size_x;  ix++  ) {
			koord k( ix, iy );
			grund_t *gr = lookup_kartenboden_nocheck(k);
			if(  !gr->ist_wasser()  &&  gr->get_pos().z == grundwasser  ) {
				uint8 neighbour_water = 0;
				for(  int i = 0;  i < 8;  i++  ) {
					grund_t *gr2 = lookup_kartenboden( k + koord::neighbours[i] );
					if(  !gr2  ||  gr2->ist_wasser()  ) {
						neighbour_water++;
					}
				}
				// if a lot of water nearby we are a headland
				if(  neighbour_water > 3  ) {
					access_nocheck(k)->set_climate( get_climate_at_height( grundwasser + 1 ) );
				}
			}
		}
	}

//printf("%d: removing isloated beaches\n",dr_time());
	// remove any isolated 1 tile beaches
	for(  uint16 iy = 0;  iy < size_y;  iy++  ) {
		for(  uint16 ix = (iy >= yoff - 19) ? 0 : max( xoff - 19, 0 );  ix < size_x;  ix++  ) {
			koord k( ix, iy );
			if(  access_nocheck(k)->get_climate()  ==  desert_climate  ) {
				uint8 neighbour_beach = 0;
				//look up neighbouring climates
				climate neighbour_climate[8];
				for(  int i = 0;  i < 8;  i++  ) {
					koord k_neighbour = k + koord::neighbours[i];
					if(  !is_within_limits(k_neighbour)  ) {
						k_neighbour = get_closest_coordinate(k_neighbour);
					}
					neighbour_climate[i] = get_climate( k_neighbour );
				}

				// get transition climate - look for each corner in turn
				for( int i = 0;  i < 4;  i++  ) {
					climate transition_climate = (climate) max( max( neighbour_climate[(i * 2 + 1) & 7], neighbour_climate[(i * 2 + 3) & 7] ), neighbour_climate[(i * 2 + 2) & 7] );
					climate min_climate = (climate) min( min( neighbour_climate[(i * 2 + 1) & 7], neighbour_climate[(i * 2 + 3) & 7] ), neighbour_climate[(i * 2 + 2) & 7] );
					if(  min_climate <= desert_climate  &&  transition_climate == desert_climate  ) {
						neighbour_beach++;
					}
				}
				if(  neighbour_beach == 0  ) {
					access_nocheck(k)->set_climate( get_climate_at_height( grundwasser + 1 ) );
				}
			}
		}
	}
}


void karte_t::init_height_to_climate()
{
	// create height table
	sint16 climate_border[MAX_CLIMATES];
	memcpy(climate_border, get_settings().get_climate_borders(), sizeof(climate_border));
	for( int cl=0;  cl<MAX_CLIMATES-1;  cl++ ) {
		if(climate_border[cl]>climate_border[arctic_climate]) {
			// unused climate
			climate_border[cl] = 0;
		}
	}
	// now arrange the remaining ones
	for( int h=0;  h<32;  h++  ) {
		sint16 current_height = 999;	// current maximum
		sint16 current_cl = arctic_climate;			// and the climate
		for( int cl=0;  cl<MAX_CLIMATES;  cl++ ) {
			if(climate_border[cl]>=h  &&  climate_border[cl]<current_height) {
				current_height = climate_border[cl];
				current_cl = cl;
			}
		}
		height_to_climate[h] = (uint8)current_cl;
	}
}


void karte_t::enlarge_map(settings_t const* sets, sint8 const* const h_field)
{
//printf("%d: enlarge map\n",dr_time());
	sint16 new_groesse_x = sets->get_groesse_x();
	sint16 new_groesse_y = sets->get_groesse_y();
	//const sint32 map_size = max (new_groesse_x, new_groesse_y);

	if(  cached_grid_size.y>0  &&  cached_grid_size.y!=new_groesse_y  ) {
		// to keep the labels
		grund_t::enlarge_map( new_groesse_x, new_groesse_y );
	}

	planquadrat_t *new_plan = new planquadrat_t[new_groesse_x*new_groesse_y];
	sint8 *new_grid_hgts = new sint8[(new_groesse_x + 1) * (new_groesse_y + 1)];
	sint8 *new_water_hgts = new sint8[new_groesse_x * new_groesse_y];

	memset( new_grid_hgts, grundwasser, sizeof(sint8) * (new_groesse_x + 1) * (new_groesse_y + 1) );
	memset( new_water_hgts, grundwasser, sizeof(sint8) * new_groesse_x * new_groesse_y );

	sint16 old_x = cached_grid_size.x;
	sint16 old_y = cached_grid_size.y;

	settings.set_groesse_x(new_groesse_x);
	settings.set_groesse_y(new_groesse_y);
	cached_grid_size.x = new_groesse_x;
	cached_grid_size.y = new_groesse_y;
	cached_size_max = max(cached_grid_size.x,cached_grid_size.y);
	cached_size.x = cached_grid_size.x-1;
	cached_size.y = cached_grid_size.y-1;

	intr_disable();

	bool reliefkarte = reliefkarte_t::is_visible;

	uint32 max_display_progress;

	// If this is not called by karte_t::init
	if(  old_x != 0  ) {
		mute_sound(true);
		reliefkarte_t::is_visible = false;

		if(is_display_init()) {
			display_show_pointer(false);
		}

// Copy old values:
		for (sint16 iy = 0; iy<old_y; iy++) {
			for (sint16 ix = 0; ix<old_x; ix++) {
				uint32 nr = ix+(iy*old_x);
				uint32 nnr = ix+(iy*new_groesse_x);
				swap(new_plan[nnr], plan[nr]);
				new_water_hgts[nnr] = water_hgts[nr];
			}
		}
		for (sint16 iy = 0; iy<=old_y; iy++) {
			for (sint16 ix = 0; ix<=old_x; ix++) {
				uint32 nr = ix+(iy*(old_x+1));
				uint32 nnr = ix+(iy*(new_groesse_x+1));
				new_grid_hgts[nnr] = grid_hgts[nr];
			}
		}
		max_display_progress = 16 + sets->get_anzahl_staedte()*2 + stadt.get_count()*4;
	}
	else {
		max_display_progress = 16 + sets->get_anzahl_staedte() * 4 + settings.get_factory_count();
	}
	loadingscreen_t ls( translator::translate( old_x ? "enlarge map" : "Init map ..."), max_display_progress, true, true );

	delete [] plan;
	plan = new_plan;
	delete [] grid_hgts;
	grid_hgts = new_grid_hgts;
	delete [] water_hgts;
	water_hgts = new_water_hgts;

	setsimrand(0xFFFFFFFF, settings.get_karte_nummer());
	clear_random_mode( 0xFFFF );
	set_random_mode( MAP_CREATE_RANDOM );

	if(  old_x == 0  &&  !settings.heightfield.empty()  ) {
		// init from file
		for(int y=0; y<cached_grid_size.y; y++) {
			for(int x=0; x<cached_grid_size.x; x++) {
				grid_hgts[x + y*(cached_grid_size.x+1)] = h_field[x+(y*(sint32)cached_grid_size.x)]+1;
			}
			grid_hgts[cached_grid_size.x + y*(cached_grid_size.x+1)] = grid_hgts[cached_grid_size.x-1 + y*(cached_grid_size.x+1)];
		}
		// lower border
		memcpy( grid_hgts+(cached_grid_size.x+1)*(sint32)cached_grid_size.y, grid_hgts+(cached_grid_size.x+1)*(sint32)(cached_grid_size.y-1), cached_grid_size.x+1 );
		ls.set_progress(2);
	}
	else {
		if(  sets->get_rotation()==0  &&  sets->get_origin_x()==0  &&  sets->get_origin_y()==0) {
			// otherwise negative offsets may occur, so we cache only non-rotated maps
			init_perlin_map(new_groesse_x,new_groesse_y);
		}
		if (  old_x > 0  &&  old_y > 0  ) {
			// loop only new tiles:
			for(  sint16 y = 0;  y<=new_groesse_y;  y++  ) {
				for(  sint16 x = (y>old_y) ? 0 : old_x+1;  x<=new_groesse_x;  x++  ) {
					koord k(x,y);
					sint16 const h = perlin_hoehe(&settings, k, koord(old_x, old_y));
					set_grid_hgt( k, (sint8) h);
					if(  is_within_limits(k)  &&  h>get_water_hgt(k)  ) {
						set_water_hgt(k, grundwasser-4);
					}
				}
				ls.set_progress( (y*16)/new_groesse_y );
			}
		}
		else {
			world_xy_loop(&karte_t::perlin_hoehe_loop, GRIDS_FLAG);
			ls.set_progress(2);
		}
		exit_perlin_map();
	}

	/** @note First we'll copy the border heights to the adjacent tile.
	 * The best way I could find is raising the first new grid point to
	 * the same height the adjacent old grid point was and lowering to the
	 * same height again. This doesn't preserve the old area 100%, but it respects it
	 * somehow.
	 */

	sint32 i;
	grund_t *gr;
	sint8 h;

	if ( old_x > 0  &&  old_y > 0){
		for(i=0; i<old_x; i++) {
			gr = lookup_kartenboden_nocheck(i, old_y-1);
			h = gr->get_hoehe(hang_t::corner_SW);
			raise_grid_to(i, old_y+1, h);
		}
		for(i=0; i<old_y; i++) {
			gr = lookup_kartenboden_nocheck(old_x-1, i);
			h = gr->get_hoehe(hang_t::corner_NE);
			raise_grid_to(old_x+1, i, h);
		}
		for(i=0; i<old_x; i++) {
			gr = lookup_kartenboden_nocheck(i, old_y-1);
			h = gr->get_hoehe(hang_t::corner_SW);
			lower_grid_to(i, old_y+1, h );
		}
		for(i=0; i<old_y; i++) {
			gr = lookup_kartenboden_nocheck(old_x-1, i);
			h = gr->get_hoehe(hang_t::corner_NE);
			lower_grid_to(old_x+1, i, h);
		}
		gr = lookup_kartenboden_nocheck(old_x-1, old_y -1);
		h = gr ->get_hoehe(hang_t::corner_SE);
		raise_grid_to(old_x+1, old_y+1, h);
		lower_grid_to(old_x+1, old_y+1, h);
	}

	if (  old_x > 0  &&  old_y > 0  ) {
		// create grounds on new part
		for (sint16 iy = 0; iy<new_groesse_y; iy++) {
			for (sint16 ix = (iy>=old_y)?0:old_x; ix<new_groesse_x; ix++) {
				koord k(ix,iy);
				access_nocheck(k)->kartenboden_setzen( new boden_t( koord3d( ix, iy, max( min_hgt_nocheck(k), get_water_hgt_nocheck(k) ) ), 0 ) );
			}
		}
	}
	else {
		world_xy_loop(&karte_t::create_grounds_loop, 0);
		ls.set_progress(3);
	}

	// smooth the new part, reassign slopes on new part
	cleanup_karte( old_x, old_y );
	if (  old_x == 0  &&  old_y == 0  ) {
		ls.set_progress(4);
	}

	if(  sets->get_lake()  ) {
		create_lakes( old_x, old_y );
	}

	if (  old_x == 0  &&  old_y == 0  ) {
		ls.set_progress(13);
	}

	// set climates in new area and old map near seam
	for(  sint16 iy = 0;  iy < new_groesse_y;  iy++  ) {
		for(  sint16 ix = (iy >= old_y - 19) ? 0 : max( old_x - 19, 0 );  ix < new_groesse_x;  ix++  ) {
			calc_climate( koord( ix, iy ), false );
		}
	}
	if (  old_x == 0  &&  old_y == 0  ) {
		ls.set_progress(14);
	}

	create_beaches( old_x, old_y );
	if (  old_x == 0  &&  old_y == 0  ) {
		ls.set_progress(15);
	}

	if (  old_x > 0  &&  old_y > 0  ) {
		// and calculate transitions in a 1 tile larger area
		for(  sint16 iy = 0;  iy < new_groesse_y;  iy++  ) {
			for(  sint16 ix = (iy >= old_y - 20) ? 0 : max( old_x - 20, 0 );  ix < new_groesse_x;  ix++  ) {
				recalc_transitions( koord( ix, iy ) );
			}
		}
	}
	else {
		// new world -> calculate all transitions
		world_xy_loop(&karte_t::recalc_transitions_loop, 0);

		ls.set_progress(16);
	}

	// now recalc the images of the old map near the seam ...
	for(  sint16 y = 0;  y < old_y - 20;  y++  ) {
		for(  sint16 x = max( old_x - 20, 0 );  x < old_x;  x++  ) {
			lookup_kartenboden_nocheck(x,y)->calc_bild();
		}
	}
	for(  sint16 y = max( old_y - 20, 0 );  y < old_y;  y++) {
		for(  sint16 x = 0;  x < old_x;  x++  ) {
			lookup_kartenboden_nocheck(x,y)->calc_bild();
		}
	}

	// eventual update origin
	switch(  settings.get_rotation()  ) {
		case 1: {
			settings.set_origin_y( settings.get_origin_y() - new_groesse_y + old_y );
			break;
		}
		case 2: {
			settings.set_origin_x( settings.get_origin_x() - new_groesse_x + old_x );
			settings.set_origin_y( settings.get_origin_y() - new_groesse_y + old_y );
			break;
		}
		case 3: {
			settings.set_origin_x( settings.get_origin_x() - new_groesse_y + old_y );
			break;
		}
	}

	distribute_groundobjs_cities(sets, old_x, old_y);

	// hausbauer_t::neue_karte(); <- this would reinit monuments! do not do this!
	fabrikbauer_t::neue_karte();

	// Modified by : Knightly
	path_explorer_t::refresh_all_categories(true);

	set_schedule_counter();

	// Refresh the haltlist for the affected tiles / stations.
	// It is enough to check the tile just at the border ...
	uint16 const cov = settings.get_station_coverage();
	if(  old_y < new_groesse_y  ) {
		for(  sint16 y=0;  y<old_y;  y++  ) {
			for(  sint16 x=old_x-cov;  x<old_x;  x++  ) {
				const planquadrat_t* pl = access_nocheck(x,y);
				for(  uint8 i=0;  i < pl->get_boden_count();  i++  ) {
					halthandle_t h = pl->get_boden_bei(i)->get_halt();
					if(  h.is_bound()  ) {
						for(  sint16 xp=max(0,x-cov);  xp<x+cov+1;  xp++  ) {
							for(  sint16 yp=y;  yp<y+cov+1;  yp++  ) {
								access_nocheck(xp,yp)->add_to_haltlist(h);
							}
						}
					}
				}
			}
		}
	}
	if(  old_x < new_groesse_x  ) {
		for(  sint16 y=old_y-cov;  y<old_y;  y++  ) {
			for(  sint16 x=0;  x<old_x;  x++  ) {
				const planquadrat_t* pl = access_nocheck(x,y);
				for(  uint8 i=0;  i < pl->get_boden_count();  i++  ) {
					halthandle_t h = pl->get_boden_bei(i)->get_halt();
					if(  h.is_bound()  ) {
						for(  sint16 xp=x;  xp<x+cov+1;  xp++  ) {
							for(  sint16 yp=max(0,y-cov);  yp<y+cov+1;  yp++  ) {
								access_nocheck(xp,yp)->add_to_haltlist(h);
							}
						}
					}
				}
			}
		}
	}
	// After refreshing the haltlists for the map,
	// refresh the haltlist for all factories.
	// Don't try to be clever; we don't do map enlargements often.
	FOR(vector_tpl<fabrik_t*>, const fab, fab_list)
	{
		fab->recalc_nearby_halts();
	}
	clear_random_mode( MAP_CREATE_RANDOM );

	if ( old_x != 0 ) {
		if(is_display_init()) {
			display_show_pointer(true);
		}
		mute_sound(false);

		reliefkarte_t::is_visible = reliefkarte;
		reliefkarte_t::get_karte()->init();
		reliefkarte_t::get_karte()->calc_map();
		reliefkarte_t::get_karte()->set_mode( reliefkarte_t::get_karte()->get_mode() );

		set_dirty();
		reset_timer();
	}
	// update main menue
	werkzeug_t::update_toolbars();
}



karte_t::karte_t() :
	settings(env_t::default_settings),
	convoi_array(0),
	ausflugsziele(16),
	stadt(0),
	idle_time(0),
	is_shutting_down(false),
	speed_factors_are_set(false)
{
	// length of day and other time stuff
	ticks_per_world_month_shift = 20;
	ticks_per_world_month = (1LL << ticks_per_world_month_shift);
	last_step_ticks = 0;
	server_last_announce_time = 0;
	last_interaction = dr_time();
	step_mode = PAUSE_FLAG;
	time_multiplier = 16;
	next_step_time = last_step_time = 0;
	fix_ratio_frame_time = 200;
	idle_time = 0;
	network_frame_count = 0;
	sync_steps = 0;
	next_step_passenger = 0;
	next_step_mail = 0;

	for(  uint i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
		werkzeug[i] = werkzeug_t::general_tool[WKZ_ABFRAGE];
	}

	viewport = new viewport_t(this);

	set_dirty();

	// for new world just set load version to current savegame version
	load_version = loadsave_t::int_version( env_t::savegame_version_str, NULL, NULL );

	// standard prices
	warenbauer_t::set_multiplier( 1000, settings.get_meters_per_tile() );
	// Must do this just after set_multiplier, since it depends on warenbauer_t having registered all wares:
	settings.cache_speedbonuses();

	zeiger = 0;
	plan = 0;

	grid_hgts = 0;
	water_hgts = 0;
	schedule_counter = 0;
	nosave_warning = nosave = false;

	recheck_road_connexions = true;
	actual_industry_density = industry_density_proportion = 0;

	loaded_rotation = 0;
	last_year = 1930;
	last_month = 0;

	for(int i=0; i<MAX_PLAYER_COUNT ; i++) {
		spieler[i] = NULL;
		MEMZERO(player_password_hash[i]);
	}

	// no distance to show at first ...
	show_distance = koord3d::invalid;
	scenario = NULL;

	map_counter = 0;

	msg = new message_t(this);
	cached_size.x = 0;
	cached_size.y = 0;

	base_pathing_counter = 0;

	citycar_speed_average = 50;

	city_road = NULL;

	// @author: jamespetts
	set_scale();

	// Added by : Knightly
	path_explorer_t::initialise(this);
	records = new records_t(this->msg);

	// generate ground textures once
	grund_besch_t::init_ground_textures(this);

	// set single instance
	world = this;
}

#ifdef DEBUG_SIMRAND_CALLS
	vector_tpl<const char*> karte_t::random_callers;
#endif


karte_t::~karte_t()
{
	is_sound = false;

	destroy();

	// not deleting the werkzeuge of this map ...
	delete viewport;
	delete msg;
	delete records;

	// unset single instance
	if (world == this) {
		world = NULL;
	}
}

void karte_t::set_scale()
{
	const uint16 scale_factor = get_settings().get_meters_per_tile();
	
	// Vehicles
	for(int i = road_wt; i <= air_wt; i++) 
	{
		if(&vehikelbauer_t::get_info((waytype_t)i) != NULL)
		{
			FOR(slist_tpl<vehikel_besch_t*>, & info, vehikelbauer_t::get_info((waytype_t)i))
			{
				info->set_scale(scale_factor);
			}
		}
	}

	// Ways
	stringhashtable_tpl <weg_besch_t *> * ways = wegbauer_t::get_all_ways();

	if(ways != NULL)
	{
		FOR(stringhashtable_tpl<weg_besch_t *>, & info, *ways)
		{
			info.value->set_scale(scale_factor);
		}
	}

	// Tunnels
	stringhashtable_tpl <tunnel_besch_t *> * tunnels = tunnelbauer_t::get_all_tunnels();

	if(tunnels != NULL)
	{
		FOR(stringhashtable_tpl<tunnel_besch_t *>, & info, *tunnels)
		{
			info.value->set_scale(scale_factor);
		}
	}

	// Bridges
	stringhashtable_tpl <bruecke_besch_t *> * bridges = brueckenbauer_t::get_all_bridges();

	if(bridges != NULL)
	{
		FOR(stringhashtable_tpl<bruecke_besch_t *>, & info, *bridges)
		{
			info.value->set_scale(scale_factor);
		}
	}

	// Way objects
	FOR(stringhashtable_tpl<way_obj_besch_t *>, & info, *wayobj_t::get_all_wayobjects())
	{
		info.value->set_scale(scale_factor);
	}

	// Stations
	ITERATE(hausbauer_t::modifiable_station_buildings, n)
	{
		hausbauer_t::modifiable_station_buildings[n]->set_scale(scale_factor); 
	}

	// Goods
	const uint16 goods_count = warenbauer_t::get_waren_anzahl();
	for(uint16 i = 0; i < goods_count; i ++)
	{
		warenbauer_t::get_modifiable_info(i)->set_scale(scale_factor);
	}

	// Settings
	settings.set_scale();

	// Cached speed factors need recalc
	speed_factors_are_set = false;
}


const char* karte_t::can_lower_plan_to(const spieler_t *sp, sint16 x, sint16 y, sint8 h) const
{
	const planquadrat_t *plan = access(x,y);

	if(  plan==NULL  ) {
		return "";
	}

	if(  h < grundwasser - 3  ) {
		return "";
	}

	const sint8 hmax = plan->get_kartenboden()->get_hoehe();
	if(  (hmax == h  ||  hmax == h - 1)  &&  (plan->get_kartenboden()->get_grund_hang() == 0  ||  is_plan_height_changeable( x, y ))  ) {
		return NULL;
	}

	if(  !is_plan_height_changeable(x, y)  ) {
		return "";
	}

	// tunnel slope below?
	grund_t *gr = plan->get_boden_in_hoehe( h - 1 );
	if(  !gr  ) {
		gr = plan->get_boden_in_hoehe( h - 2 );
	}
	if(  !gr  &&  env_t::pak_height_conversion_factor == 2  ) {
		gr = plan->get_boden_in_hoehe( h - 2 );
	}
	if(  gr  &&  h-gr->get_pos().z + hang_t::max_diff( gr->get_weg_hang() ) < env_t::pak_height_conversion_factor  ) {
		return "";
	}

	// tunnel below?
	while(h < hmax) {
		if(plan->get_boden_in_hoehe(h)) {
			return "";
		}
		h ++;
	}

	// check allowance by scenario
	if (get_scenario()->is_scripted()) {
		return get_scenario()->is_work_allowed_here(sp, WKZ_LOWER_LAND|GENERAL_TOOL, ignore_wt, plan->get_kartenboden()->get_pos());
	}

	return NULL;
}


const char* karte_t::can_raise_plan_to(const spieler_t *sp, sint16 x, sint16 y, sint8 h) const
{
	const planquadrat_t *plan = access(x,y);
	if(  plan == 0  ||  !is_plan_height_changeable(x, y)  ) {
		return "";
	}

	// irgendwo eine Bruecke im Weg?
	int hmin = plan->get_kartenboden()->get_hoehe();
	while(h > hmin) {
		if(plan->get_boden_in_hoehe(h)) {
			return "";
		}
		h --;
	}

	// check allowance by scenario
	if (get_scenario()->is_scripted()) {
		return get_scenario()->is_work_allowed_here(sp, WKZ_RAISE_LAND|GENERAL_TOOL, ignore_wt, plan->get_kartenboden()->get_pos());
	}

	return NULL;
}


bool karte_t::is_plan_height_changeable(sint16 x, sint16 y) const
{
	const planquadrat_t *plan = access(x,y);
	bool ok = true;

	if(plan != NULL) {
		grund_t *gr = plan->get_kartenboden();

		ok = (gr->ist_natur() || gr->ist_wasser())  &&  !gr->hat_wege()  &&  !gr->is_halt();

		for(  int i=0; ok  &&  i<gr->get_top(); i++  ) {
			const obj_t *obj = gr->obj_bei(i);
			assert(obj != NULL);
			ok =
				obj->get_typ() == obj_t::baum  ||
				obj->get_typ() == obj_t::zeiger  ||
				obj->get_typ() == obj_t::wolke  ||
				obj->get_typ() == obj_t::sync_wolke  ||
				obj->get_typ() == obj_t::async_wolke  ||
				obj->get_typ() == obj_t::groundobj;
		}
	}

	return ok;
}


bool karte_t::terraformer_t::node_t::comp(const karte_t::terraformer_t::node_t& a, const karte_t::terraformer_t::node_t& b)
{
	int diff = a.x- b.x;
	if (diff == 0) {
		diff = a.y - b.y;
	}
	return diff<0;
}


void karte_t::terraformer_t::add_node(bool raise, sint16 x, sint16 y, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw)
{
	if (!welt->is_within_limits(x,y)) {
		return;
	}
	node_t test(x, y, hsw, hse, hne, hnw, actual_flag^3);
	node_t *other = list.insert_unique_ordered(test, node_t::comp);

	sint8 factor = raise ? +1 : -1;

	if (other) {
		for(int i=0; i<4; i++) {
			if (factor*other->h[i] < factor*test.h[i]) {
				other->h[i] = test.h[i];
				other->changed |= actual_flag^3;
				ready = false;
			}
		}
	}
	else {
		ready = false;
	}
}

void karte_t::terraformer_t::add_raise_node(sint16 x, sint16 y, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw)
{
	add_node(true, x, y, hsw, hse, hne, hnw);
}

void karte_t::terraformer_t::add_lower_node(sint16 x, sint16 y, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw)
{
	add_node(false, x, y, hsw, hse, hne, hnw);
}

void karte_t::terraformer_t::iterate(bool raise)
{
	while( !ready) {
		actual_flag ^= 3; // flip bits
		// clear new_flag bit
		FOR(vector_tpl<node_t>, &i, list) {
			i.changed &= actual_flag;
		}
		// process nodes with actual_flag set
		ready = true;
		for(uint32 j=0; j < list.get_count(); j++) {
			node_t& i = list[j];
			if (i.changed & actual_flag) {
				i.changed &= ~ actual_flag;
				if (raise) {
					welt->prepare_raise(*this, i.x, i.y, i.h[0], i.h[1], i.h[2], i.h[3]);
				}
				else {
					welt->prepare_lower(*this, i.x, i.y, i.h[0], i.h[1], i.h[2], i.h[3]);
				}
			}
		}
	}
}


const char* karte_t::terraformer_t::can_raise_all(const spieler_t *sp, bool allow_deep_water, bool keep_water) const
{
	const char* err = NULL;
	FOR(vector_tpl<node_t>, const &i, list) {
		err = welt->can_raise_to(sp, i.x, i.y, keep_water, allow_deep_water, i.h[0], i.h[1], i.h[2], i.h[3]);
		if (err) return err;
	}
	return NULL;
}

const char* karte_t::terraformer_t::can_lower_all(const spieler_t *sp) const
{
	const char* err = NULL;
	FOR(vector_tpl<node_t>, const &i, list) {
		err = welt->can_lower_to(sp, i.x, i.y, i.h[0], i.h[1], i.h[2], i.h[3]);
		if (err) {
			return err;
		}
	}
	return NULL;
}

int karte_t::terraformer_t::raise_all()
{
	int n=0;
	FOR(vector_tpl<node_t>, &i, list) {
		n += welt->raise_to(i.x, i.y, i.h[0], i.h[1], i.h[2], i.h[3]);
	}
	return n;
}

int karte_t::terraformer_t::lower_all()
{
	int n=0;
	FOR(vector_tpl<node_t>, &i, list) {
		n += welt->lower_to(i.x, i.y, i.h[0], i.h[1], i.h[2], i.h[3]);
	}
	return n;
}


const char* karte_t::can_raise_to(const spieler_t *sp, sint16 x, sint16 y, bool keep_water, bool allow_deep_water, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw) const
{
	assert(is_within_limits(x,y));
	grund_t *gr = lookup_kartenboden_nocheck(x,y);
	const sint8 water_hgt = get_water_hgt_nocheck(x,y);

	const sint8 max_hgt = max(max(hsw,hse),max(hne,hnw));
	const sint8 min_hgt = min(min(hsw,hse),min(hne,hnw));

	if(  gr->ist_wasser()  &&  keep_water  &&  max_hgt > water_hgt  ) {
		return "";
	}

	if(min_hgt < grundwasser && !allow_deep_water) 
	{
		return "";
	}

	const char* err = can_raise_plan_to(sp, x, y, max_hgt);

	return err;
}


void karte_t::prepare_raise(terraformer_t& digger, sint16 x, sint16 y, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw)
{
	assert(is_within_limits(x,y));
	grund_t *gr = lookup_kartenboden_nocheck(x,y);
	const sint8 water_hgt = get_water_hgt_nocheck(x,y);
	const sint8 h0 = gr->get_hoehe();
	// old height
	const sint8 h0_sw = gr->ist_wasser() ? min(water_hgt, lookup_hgt_nocheck(x,y+1) )   : h0 + corner1( gr->get_grund_hang() );
	const sint8 h0_se = gr->ist_wasser() ? min(water_hgt, lookup_hgt_nocheck(x+1,y+1) ) : h0 + corner2( gr->get_grund_hang() );
	const sint8 h0_ne = gr->ist_wasser() ? min(water_hgt, lookup_hgt_nocheck(x+1,y) )   : h0 + corner3( gr->get_grund_hang() );
	const sint8 h0_nw = gr->ist_wasser() ? min(water_hgt, lookup_hgt_nocheck(x,y) )     : h0 + corner4( gr->get_grund_hang() );

	// new height
	const sint8 hn_sw = max(hsw, h0_sw);
	const sint8 hn_se = max(hse, h0_se);
	const sint8 hn_ne = max(hne, h0_ne);
	const sint8 hn_nw = max(hnw, h0_nw);
	// nothing to do?
	if (!gr->ist_wasser()  &&  h0_sw >= hsw  &&  h0_se >= hse  &&  h0_ne >= hne  &&  h0_nw >= hnw) return;
	// calc new height and slope
	const sint8 hneu = min( min( hn_sw, hn_se ), min( hn_ne, hn_nw ) );
	const sint8 hmaxneu = max( max( hn_sw, hn_se ), max( hn_ne, hn_nw ) );

	const uint8 max_hdiff = grund_besch_t::double_grounds ? 2 : 1;

	bool ok = (hmaxneu - hneu <= max_hdiff); // may fail on water tiles since lookup_hgt might be modified from previous raise_to calls
	if (!ok && !gr->ist_wasser()) {
		assert(false);
	}

	// sw
	if (h0_sw < hsw) {
		digger.add_raise_node( x - 1, y + 1, hsw - max_hdiff, hsw - max_hdiff, hsw, hsw - max_hdiff );
	}
	// s
	if (h0_sw < hsw  ||  h0_se < hse) {
		const sint8 hs = max( hse, hsw ) - max_hdiff;
		digger.add_raise_node( x, y + 1, hs, hs, hse, hsw );
	}
	// se
	if (h0_se < hse) {
		digger.add_raise_node( x + 1, y + 1, hse - max_hdiff, hse - max_hdiff, hse - max_hdiff, hse );
	}
	// e
	if (h0_se < hse  ||  h0_ne < hne) {
		const sint8 he = max( hse, hne ) - max_hdiff;
		digger.add_raise_node( x + 1, y, hse, he, he, hne );
	}
	// ne
	if (h0_ne < hne) {
		digger.add_raise_node( x + 1,y - 1, hne, hne - max_hdiff, hne - max_hdiff, hne - max_hdiff );
	}
	// n
	if (h0_nw < hnw  ||  h0_ne < hne) {
		const sint8 hn = max( hnw, hne ) - max_hdiff;
		digger.add_raise_node( x, y - 1, hnw, hne, hn, hn );
	}
	// nw
	if (h0_nw < hnw) {
		digger.add_raise_node( x - 1, y - 1, hnw - max_hdiff, hnw, hnw - max_hdiff, hnw - max_hdiff );
	}
	// w
	if (h0_sw < hsw  ||  h0_nw < hnw) {
		const sint8 hw = max( hnw, hsw ) - max_hdiff;
		digger.add_raise_node( x - 1, y, hw, hsw, hnw, hw );
	}
}


int karte_t::raise_to(sint16 x, sint16 y, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw)
{
	int n=0;
	assert(is_within_limits(x,y));
	grund_t *gr = lookup_kartenboden_nocheck(x,y);
	const sint8 water_hgt = get_water_hgt_nocheck(x,y);
	const sint8 h0 = gr->get_hoehe();
	// old height
	const sint8 h0_sw = gr->ist_wasser() ? min(water_hgt, lookup_hgt_nocheck(x,y+1) )   : h0 + corner1( gr->get_grund_hang() );
	const sint8 h0_se = gr->ist_wasser() ? min(water_hgt, lookup_hgt_nocheck(x+1,y+1) ) : h0 + corner2( gr->get_grund_hang() );
	const sint8 h0_ne = gr->ist_wasser() ? min(water_hgt, lookup_hgt_nocheck(x+1,y) )   : h0 + corner3( gr->get_grund_hang() );
	const sint8 h0_nw = gr->ist_wasser() ? min(water_hgt, lookup_hgt_nocheck(x,y) )     : h0 + corner4( gr->get_grund_hang() );

	// new height
	const sint8 hn_sw = max(hsw, h0_sw);
	const sint8 hn_se = max(hse, h0_se);
	const sint8 hn_ne = max(hne, h0_ne);
	const sint8 hn_nw = max(hnw, h0_nw);
	// nothing to do?
	if (!gr->ist_wasser()  &&  h0_sw >= hsw  &&  h0_se >= hse  &&  h0_ne >= hne  &&  h0_nw >= hnw) return 0;
	// calc new height and slope
	const sint8 hneu = min( min( hn_sw, hn_se ), min( hn_ne, hn_nw ) );
	const sint8 hmaxneu = max( max( hn_sw, hn_se ), max( hn_ne, hn_nw ) );

	const uint8 max_hdiff = grund_besch_t::double_grounds ? 2 : 1;
	const sint8 disp_hneu = max( hneu, water_hgt );
	const sint8 disp_hn_sw = max( hn_sw, water_hgt );
	const sint8 disp_hn_se = max( hn_se, water_hgt );
	const sint8 disp_hn_ne = max( hn_ne, water_hgt );
	const sint8 disp_hn_nw = max( hn_nw, water_hgt );
	const uint8 sneu = (disp_hn_sw - disp_hneu) + ((disp_hn_se - disp_hneu) * 3) + ((disp_hn_ne - disp_hneu) * 9) + ((disp_hn_nw - disp_hneu) * 27);

	bool ok = (hmaxneu - hneu <= max_hdiff); // may fail on water tiles since lookup_hgt might be modified from previous raise_to calls
	if (!ok && !gr->ist_wasser()) {
		assert(false);
	}
	// change height and slope, for water tiles only if they will become land
	if(  !gr->ist_wasser()  ||  (hmaxneu > water_hgt)  ) {
		gr->set_pos( koord3d( x, y, disp_hneu ) );
		gr->set_grund_hang( (hang_t::typ)sneu );
		access_nocheck(x,y)->angehoben();
		set_water_hgt(x, y, grundwasser-4);
	}

	// update north point in grid
	set_grid_hgt(x, y, hn_nw);
	calc_climate(koord(x,y), true);
	if ( x == cached_size.x ) {
		// update eastern grid coordinates too if we are in the edge.
		set_grid_hgt(x+1, y, hn_ne);
		set_grid_hgt(x+1, y+1, hn_se);
	}
	if ( y == cached_size.y ) {
		// update southern grid coordinates too if we are in the edge.
		set_grid_hgt(x, y+1, hn_sw);
		set_grid_hgt(x+1, y+1, hn_se);
	}

	n += hn_sw - h0_sw + hn_se - h0_se + hn_ne - h0_ne  + hn_nw - h0_nw;

	lookup_kartenboden_nocheck(x,y)->calc_bild();
	if ( (x+1) < cached_size.x ) {
		lookup_kartenboden_nocheck(x+1,y)->calc_bild();
	}
	if ( (y+1) < cached_size.y ) {
		lookup_kartenboden_nocheck(x,y+1)->calc_bild();
	}

	return n;
}


// raise height in the hgt-array
void karte_t::raise_grid_to(sint16 x, sint16 y, sint8 h)
{
	if(is_within_grid_limits(x,y)) {
		const sint32 offset = x + y*(cached_grid_size.x+1);

		if(  grid_hgts[offset] < h  ) {
			grid_hgts[offset] = h;

			const sint8 hh = h - (grund_besch_t::double_grounds ? 2 : 1);

			// set new height of neighbor grid points
			raise_grid_to(x-1, y-1, hh);
			raise_grid_to(x  , y-1, hh);
			raise_grid_to(x+1, y-1, hh);
			raise_grid_to(x-1, y  , hh);
			raise_grid_to(x+1, y  , hh);
			raise_grid_to(x-1, y+1, hh);
			raise_grid_to(x  , y+1, hh);
			raise_grid_to(x+1, y+1, hh);
		}
	}
}


int karte_t::grid_raise(const spieler_t *sp, koord k, bool allow_deep_water, const char*&err)
{
	int n = 0;

	if(is_within_grid_limits(k)) {

		const grund_t *gr = lookup_kartenboden_gridcoords(k);
		const hang_t::typ corner_to_raise = get_corner_to_operate(k);

		const sint16 x = gr->get_pos().x;
		const sint16 y = gr->get_pos().y;
		const sint8 hgt = gr->get_hoehe(corner_to_raise);

		const sint8 f = grund_besch_t::double_grounds ?  2 : 1;
		const sint8 o = grund_besch_t::double_grounds ?  1 : 0;
		const sint8 hsw = hgt - o + scorner1( corner_to_raise ) * f;
		const sint8 hse = hgt - o + scorner2( corner_to_raise ) * f;
		const sint8 hne = hgt - o + scorner3( corner_to_raise ) * f;
		const sint8 hnw = hgt - o + scorner4( corner_to_raise ) * f;

		terraformer_t digger(this);
		digger.add_raise_node(x, y, hsw, hse, hne, hnw);
		digger.iterate(true);

		err = digger.can_raise_all(sp, allow_deep_water);
		if (err) return 0;

		n = digger.raise_all();

		// force world full redraw, or background could be dirty.
		set_dirty();

	}
	return (n+3)>>2;
}


void karte_t::prepare_lower(terraformer_t& digger, sint16 x, sint16 y, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw)
{
	assert(is_within_limits(x,y));
	grund_t *gr = lookup_kartenboden_nocheck(x,y);
	const sint8 water_hgt = get_water_hgt_nocheck(x,y);
	const sint8 h0 = gr->get_hoehe();
	// which corners have to be raised?
	const sint8 h0_sw = gr->ist_wasser() ? min( water_hgt, lookup_hgt_nocheck(x,y+1) )   : h0 + corner1( gr->get_grund_hang() );
	const sint8 h0_se = gr->ist_wasser() ? min( water_hgt, lookup_hgt_nocheck(x+1,y+1) ) : h0 + corner2( gr->get_grund_hang() );
	const sint8 h0_ne = gr->ist_wasser() ? min( water_hgt, lookup_hgt_nocheck(x,y+1) )   : h0 + corner3( gr->get_grund_hang() );
	const sint8 h0_nw = gr->ist_wasser() ? min( water_hgt, lookup_hgt_nocheck(x,y) )     : h0 + corner4( gr->get_grund_hang() );

	const uint8 max_hdiff = grund_besch_t::double_grounds ?  2 : 1;

	// sw
	if (h0_sw > hsw) {
		digger.add_lower_node(x - 1, y + 1, hsw + max_hdiff, hsw + max_hdiff, hsw, hsw + max_hdiff);
	}
	// s
	if (h0_se > hse || h0_sw > hsw) {
		const sint8 hs = min( hse, hsw ) + max_hdiff;
		digger.add_lower_node( x, y + 1, hs, hs, hse, hsw);
	}
	// se
	if (h0_se > hse) {
		digger.add_lower_node( x + 1, y + 1, hse + max_hdiff, hse + max_hdiff, hse + max_hdiff, hse);
	}
	// e
	if (h0_se > hse || h0_ne > hne) {
		const sint8 he = max( hse, hne ) + max_hdiff;
		digger.add_lower_node( x + 1,y, hse, he, he, hne);
	}
	// ne
	if (h0_ne > hne) {
		digger.add_lower_node( x + 1, y - 1, hne, hne + max_hdiff, hne + max_hdiff, hne + max_hdiff);
	}
	// n
	if (h0_nw > hnw  ||  h0_ne > hne) {
		const sint8 hn = min( hnw, hne ) + max_hdiff;
		digger.add_lower_node( x, y - 1, hnw, hne, hn, hn);
	}
	// nw
	if (h0_nw > hnw) {
		digger.add_lower_node( x - 1, y - 1, hnw + max_hdiff, hnw, hnw + max_hdiff, hnw + max_hdiff);
	}
	// w
	if (h0_nw > hnw || h0_sw > hsw) {
		const sint8 hw = min( hnw, hsw ) + max_hdiff;
		digger.add_lower_node( x - 1, y, hw, hsw, hnw, hw);
	}
}

// lower plan
// new heights for each corner given
// only test corners in ctest to avoid infinite loops
const char* karte_t::can_lower_to(const spieler_t* sp, sint16 x, sint16 y, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw) const
{
	assert(is_within_limits(x,y));

	const sint8 hneu = min( min( hsw, hse ), min( hne, hnw ) );
	// water heights
	// check if need to lower water height for higher neighbouring tiles
	for(  sint16 i = 0 ;  i < 8 ;  i++  ) {
		const koord neighbour = koord( x, y ) + koord::neighbours[i];
		if(  is_within_limits(neighbour)  &&  get_water_hgt_nocheck(neighbour) > hneu  ) {
			if (!is_plan_height_changeable( neighbour.x, neighbour.y )) {
				return "";
			}
		}
	}

	return can_lower_plan_to(sp, x, y, hneu );
}


int karte_t::lower_to(sint16 x, sint16 y, sint8 hsw, sint8 hse, sint8 hne, sint8 hnw)
{
	int n=0;
	assert(is_within_limits(x,y));
	grund_t *gr = lookup_kartenboden_nocheck(x,y);
	sint8 water_hgt = get_water_hgt_nocheck(x,y);
	const sint8 h0 = gr->get_hoehe();
	// old height
	const sint8 h0_sw = gr->ist_wasser() ? min( water_hgt, lookup_hgt_nocheck(x,y+1) )   : h0 + corner1( gr->get_grund_hang() );
	const sint8 h0_se = gr->ist_wasser() ? min( water_hgt, lookup_hgt_nocheck(x+1,y+1) ) : h0 + corner2( gr->get_grund_hang() );
	const sint8 h0_ne = gr->ist_wasser() ? min( water_hgt, lookup_hgt_nocheck(x+1,y) )   : h0 + corner3( gr->get_grund_hang() );
	const sint8 h0_nw = gr->ist_wasser() ? min( water_hgt, lookup_hgt_nocheck(x,y) )     : h0 + corner4( gr->get_grund_hang() );
	// new height
	const sint8 hn_sw = min(hsw, h0_sw);
	const sint8 hn_se = min(hse, h0_se);
	const sint8 hn_ne = min(hne, h0_ne);
	const sint8 hn_nw = min(hnw, h0_nw);
	// nothing to do?
	if(  gr->ist_wasser()  ) {
		if(  h0_nw <= hnw  ) {
			return 0;
		}
	}
	else {
		if(  h0_sw <= hsw  &&  h0_se <= hse  &&  h0_ne <= hne  &&  h0_nw <= hnw  ) {
			return 0;
		}
	}

	// calc new height and slope
	const sint8 hneu = min( min( hn_sw, hn_se ), min( hn_ne, hn_nw ) );
	const sint8 hmaxneu = max( max( hn_sw, hn_se ), max( hn_ne, hn_nw ) );

	if(  hneu >= water_hgt  ) {
		// calculate water table from surrounding tiles - start off with height on this tile
		sint8 water_table = water_hgt >= h0 ? water_hgt : grundwasser - 4;

		/* we test each corner in turn to see whether it is at the base height of the tile
			if it is we then mark the 3 surrounding tiles for that corner for checking
			surrounding tiles are indicated by bits going anti-clockwise from
			(binary) 00000001 for north-west through to (binary) 10000000 for north
			as this is the order of directions used by koord::neighbours[] */

		uint8 neighbour_flags = 0;

		if(  hn_nw == hneu  ) {
			neighbour_flags |= 0x83;
		}
		if(  hn_ne == hneu  ) {
			neighbour_flags |= 0xe0;
		}
		if(  hn_se == hneu  ) {
			neighbour_flags |= 0x38;
		}
		if(  hn_sw == hneu  ) {
			neighbour_flags |= 0x0e;
		}

		for(  sint16 i = 0;  i < 8 ;  i++  ) {
			const koord neighbour = koord( x, y ) + koord::neighbours[i];

			// here we look at the bit in neighbour_flags for this direction
			// we shift it i bits to the right and test the least significant bit

			if(  is_within_limits( neighbour )  &&  ((neighbour_flags >> i) & 1)  ) {
				grund_t *gr2 = lookup_kartenboden_nocheck( neighbour );
				const sint8 water_hgt_neighbour = get_water_hgt_nocheck( neighbour );
				if(  gr2  &&  (water_hgt_neighbour >= gr2->get_hoehe())  &&  water_hgt_neighbour <= hneu  ) {
					water_table = max( water_table, water_hgt_neighbour );
				}
			}
		}

		// only allow water table to be lowered (except for case of sea level)
		// this prevents severe (errors!
		if(  water_table < get_water_hgt_nocheck(x,y)/* || water_table==grundwasser*/) {
			water_hgt = water_table;
			set_water_hgt(x, y, water_table );
		}
	}

	// calc new height and slope
	const sint8 disp_hneu = max( hneu, water_hgt );
	const sint8 disp_hn_sw = max( hn_sw, water_hgt );
	const sint8 disp_hn_se = max( hn_se, water_hgt );
	const sint8 disp_hn_ne = max( hn_ne, water_hgt );
	const sint8 disp_hn_nw = max( hn_nw, water_hgt );
	const uint8 sneu = (disp_hn_sw - disp_hneu) + ((disp_hn_se - disp_hneu) * 3) + ((disp_hn_ne - disp_hneu) * 9) + ((disp_hn_nw - disp_hneu) * 27);

	// change height and slope for land tiles only
	if(  !gr->ist_wasser()  ||  (hmaxneu > water_hgt)  ) {
		gr->set_pos( koord3d( x, y, disp_hneu ) );
		gr->set_grund_hang( (hang_t::typ)sneu );
		access_nocheck(x,y)->abgesenkt();
	}
	// update north point in grid
	set_grid_hgt(x, y, hn_nw);
	if ( x == cached_size.x ) {
		// update eastern grid coordinates too if we are in the edge.
		set_grid_hgt(x+1, y, hn_ne);
		set_grid_hgt(x+1, y+1, hn_se);
	}
	if ( y == cached_size.y ) {
		// update southern grid coordinates too if we are in the edge.
		set_grid_hgt(x, y+1, hn_sw);
		set_grid_hgt(x+1, y+1, hn_se);
	}

	// water heights
	// lower water height for higher neighbouring tiles
	// find out how high water is
	for(  sint16 i = 0;  i < 8;  i++  ) {
		const koord neighbour = koord( x, y ) + koord::neighbours[i];
		if(  is_within_limits( neighbour )  ) {
			const sint8 water_hgt_neighbour = get_water_hgt_nocheck( neighbour );
			if(water_hgt_neighbour > hneu  ) {
				if(  min_hgt_nocheck( neighbour ) < water_hgt_neighbour  ) {
					// convert to flat ground before lowering water level
					raise_grid_to( neighbour.x, neighbour.y, water_hgt_neighbour );
					raise_grid_to( neighbour.x + 1, neighbour.y, water_hgt_neighbour );
					raise_grid_to( neighbour.x, neighbour.y + 1, water_hgt_neighbour );
					raise_grid_to( neighbour.x + 1, neighbour.y + 1, water_hgt_neighbour );
				}
				set_water_hgt( neighbour, hneu );
				access_nocheck(neighbour)->correct_water();
			}
		}
	}

	calc_climate( koord( x, y ), false );
	for(  sint16 i = 0;  i < 8;  i++  ) {
		const koord neighbour = koord( x, y ) + koord::neighbours[i];
		calc_climate( neighbour, false );
	}

	// recalc landscape images - need to extend 2 in each direction
	for(  sint16 j = y - 2;  j <= y + 2;  j++  ) {
		for(  sint16 i = x - 2;  i <= x + 2;  i++  ) {
			if(  is_within_limits( i, j )  /*&&  (i != x  ||  j != y)*/  ) {
				recalc_transitions( koord (i, j ) );
			}
		}
	}

	n += h0_sw-hn_sw + h0_se-hn_se + h0_ne-hn_ne + h0_nw-hn_nw;

	lookup_kartenboden_nocheck(x,y)->calc_bild();
	if( (x+1) < cached_size.x ) {
		lookup_kartenboden_nocheck(x+1,y)->calc_bild();
	}
	if( (y+1) < cached_size.y ) {
		lookup_kartenboden_nocheck(x,y+1)->calc_bild();
	}
	return n;
}


void karte_t::lower_grid_to(sint16 x, sint16 y, sint8 h)
{
	if(is_within_grid_limits(x,y)) {
		const sint32 offset = x + y*(cached_grid_size.x+1);

		if(  grid_hgts[offset] > h  ) {
			grid_hgts[offset] = h;
			sint8 hh = h + 2;
			// set new height of neighbor grid points
			lower_grid_to(x-1, y-1, hh);
			lower_grid_to(x  , y-1, hh);
			lower_grid_to(x+1, y-1, hh);
			lower_grid_to(x-1, y  , hh);
			lower_grid_to(x+1, y  , hh);
			lower_grid_to(x-1, y+1, hh);
			lower_grid_to(x  , y+1, hh);
			lower_grid_to(x+1, y+1, hh);
		}
	}
}


int karte_t::grid_lower(const spieler_t *sp, koord k, const char*&err)
{
	int n = 0;

	if(is_within_grid_limits(k)) {

		const grund_t *gr = lookup_kartenboden_gridcoords(k);
		const hang_t::typ corner_to_lower = get_corner_to_operate(k);

		const sint16 x = gr->get_pos().x;
		const sint16 y = gr->get_pos().y;
		const sint8 hgt = gr->get_hoehe(corner_to_lower);

		const sint8 f = grund_besch_t::double_grounds ?  2 : 1;
		const sint8 o = grund_besch_t::double_grounds ?  1 : 0;
		const sint8 hsw = hgt + o - scorner1( corner_to_lower ) * f;
		const sint8 hse = hgt + o - scorner2( corner_to_lower ) * f;
		const sint8 hne = hgt + o - scorner3( corner_to_lower ) * f;
		const sint8 hnw = hgt + o - scorner4( corner_to_lower ) * f;

		terraformer_t digger(this);
		digger.add_lower_node(x, y, hsw, hse, hne, hnw);
		digger.iterate(false);

		err = digger.can_lower_all(sp);
		if (err) {
			return 0;
		}

		n = digger.lower_all();
		err = NULL;
	}
	return (n+3)>>2;
}


bool karte_t::can_ebne_planquadrat(spieler_t *sp, koord k, sint8 hgt, bool keep_water, bool make_underwater_hill)
{
	return ebne_planquadrat(sp, k, hgt, keep_water, make_underwater_hill, true /* justcheck */);
}


// make a flat level at this position (only used for AI at the moment)
bool karte_t::ebne_planquadrat(spieler_t *sp, koord k, sint8 hgt, bool keep_water, bool make_underwater_hill, bool justcheck)
{
	int n = 0;
	bool ok = true;
	const grund_t *gr = lookup_kartenboden(k);
	const hang_t::typ slope = gr->get_grund_hang();
	const sint8 old_hgt = make_underwater_hill  &&  gr->ist_wasser() ? min_hgt(k) : gr->get_hoehe();
	const sint8 max_hgt = old_hgt + hang_t::max_diff(slope);
	if(  max_hgt > hgt  ) {

		terraformer_t digger(this);
		digger.add_lower_node(k.x, k.y, hgt, hgt, hgt, hgt);
		digger.iterate(false);

		ok = digger.can_lower_all(sp) == NULL;

		if (ok  &&  !justcheck) {
			n += digger.lower_all();
		}
	}
	if(  ok  &&  old_hgt < hgt  ) {

		terraformer_t digger(this);
		digger.add_raise_node(k.x, k.y, hgt, hgt, hgt, hgt);
		digger.iterate(true);

		ok = digger.can_raise_all(sp, keep_water) == NULL;

		if (ok  &&  !justcheck) {
			n += digger.raise_all();
		}
	}
	// was changed => pay for it
	if(n>0) {
		n = (n+3) >> 2;
		spieler_t::book_construction_costs(sp, n * settings.cst_alter_land, k, ignore_wt);
	}
	return ok;
}


void karte_t::store_player_password_hash( uint8 player_nr, const pwd_hash_t& hash )
{
	player_password_hash[player_nr] = hash;
}


void karte_t::clear_player_password_hashes()
{
	for(int i=0; i<MAX_PLAYER_COUNT ; i++) {
		player_password_hash[i].clear();
		if (spieler[i]) {
			spieler[i]->check_unlock(player_password_hash[i]);
		}
	}
}


void karte_t::rdwr_player_password_hashes(loadsave_t *file)
{
	pwd_hash_t dummy;
	for(  int i=0;  i<PLAYER_UNOWNED; i++  ) {
		pwd_hash_t *p = spieler[i] ? &spieler[i]->access_password_hash() : &dummy;
		for(  uint8 j=0; j<20; j++) {
			file->rdwr_byte( (*p)[j] );
		}
	}
}


void karte_t::call_change_player_tool(uint8 cmd, uint8 player_nr, uint16 param, bool scripted_call)
{
	if (env_t::networkmode) {
		nwc_chg_player_t *nwc = new nwc_chg_player_t(sync_steps, map_counter, cmd, player_nr, param, scripted_call);

		network_send_server(nwc);
	}
	else {
		change_player_tool(cmd, player_nr, param, !get_spieler(1)->is_locked()  ||  scripted_call, true);
		// update the window
		ki_kontroll_t* playerwin = (ki_kontroll_t*)win_get_magic(magic_ki_kontroll_t);
		if (playerwin) {
			playerwin->update_data();
		}
	}
}


bool karte_t::change_player_tool(uint8 cmd, uint8 player_nr, uint16 param, bool public_player_unlocked, bool exec)
{
	switch(cmd) {
		case new_player: {
			// only public player can start AI
			if(  (param != spieler_t::HUMAN  &&  !public_player_unlocked)  ||  param >= spieler_t::MAX_AI  ) {
				return false;
			}
			// range check, player already existent?
			if(  player_nr >= PLAYER_UNOWNED  ||   get_spieler(player_nr)  ) {
				return false;
			}
			if(exec) {
				new_spieler( player_nr, (uint8) param );
				// activate/deactivate AI immediately
				spieler_t *sp = get_spieler(player_nr);
				if (param != spieler_t::HUMAN  &&  sp) {
					sp->set_active(true);
					settings.set_player_active(player_nr, sp->is_active());
				}
			}
			return true;
		}
		case toggle_freeplay: {
			// only public player can change freeplay mode
			if (!public_player_unlocked  ||  !settings.get_allow_player_change()) {
				return false;
			}
			if (exec) {
				settings.set_freeplay( !settings.is_freeplay() );
			}
			return true;
		}
		case delete_player: {
			// range check, player existent?
			if ( player_nr >= PLAYER_UNOWNED  ||   get_spieler(player_nr)==NULL ) {
				return false;
			}
			if (exec) {
				remove_player(player_nr);
			}
			return true;
		}
		// unknown command: delete
		default: ;
	}
	return false;
}


void karte_t::set_werkzeug( werkzeug_t *w, spieler_t *sp )
{
	if(  get_random_mode()&LOAD_RANDOM  ) {
		dbg->warning("karte_t::set_werkzeug", "Ignored tool %i during loading.", w->get_id() );
		return;
	}
	bool scripted_call = w->is_scripted();
	// check for scenario conditions
	if(  !scripted_call  &&  !scenario->is_tool_allowed(sp, w->get_id(), w->get_waytype())  ) {
		return;
	}

	spieler_t* action_player = sp;
	if(w->get_id() == (WKZ_ACCESS_TOOL | SIMPLE_TOOL))
	{
		uint16 id_setting_player;
		uint16 id_receiving_player;
		sint16 allow_access;

		if(3 != sscanf(w->get_default_param(), "g%hi,%hi,%hi", &id_setting_player, &id_receiving_player, &allow_access)) 
		{
			dbg->error( "karte_t::set_werkzeug", "could not perform (%s)", w->get_default_param() );
			return;
		}
		action_player = this->get_spieler(id_setting_player);
	}

	// check for password-protected players
	if(  (!w->is_init_network_save()  ||  !w->is_work_network_save())  &&  !scripted_call  &&
		 !(w->get_id()==(WKZ_SET_PLAYER_TOOL|SIMPLE_TOOL)  ||  w->get_id()==(WKZ_ADD_MESSAGE_TOOL|SIMPLE_TOOL))  &&
		 action_player  &&  action_player->is_locked()  ) {
		// player is currently password protected => request unlock first
		create_win( -1, -1, new password_frame_t(action_player), w_info, magic_pwd_t + action_player->get_player_nr() );
		return;
	}
	w->flags |= event_get_last_control_shift();
	if(!env_t::networkmode  ||  w->is_init_network_save()  ) {
		local_set_werkzeug(w, sp);
	}
	else {
		// queue tool for network
		nwc_tool_t *nwc = new nwc_tool_t(sp, w, zeiger->get_pos(), steps, map_counter, true);
		network_send_server(nwc);
	}
}


// set a new tool on our client, calls init
void karte_t::local_set_werkzeug( werkzeug_t *w, spieler_t * sp )
{
	w->flags |= werkzeug_t::WFL_LOCAL;

	if (get_scenario()->is_scripted()  &&  !get_scenario()->is_tool_allowed(sp, w->get_id()) ) {
		w->flags = 0;
		return;
	}
	// now call init
	bool init_result = w->init(sp);
	// for unsafe tools init() must return false
	assert(w->is_init_network_save()  ||  !init_result);

	if (sp && init_result) {

		set_dirty();
		werkzeug_t *sp_wkz = werkzeug[sp->get_player_nr()];
		if(w != sp_wkz) {

			// reinit same tool => do not play sound twice
			sound_play(SFX_SELECT);

			// only exit, if it is not the same tool again ...

			sp_wkz->flags |= werkzeug_t::WFL_LOCAL;
			sp_wkz->exit(sp);
			sp_wkz->flags =0;
		}
		else {
			// init again, to interrupt dragging
			werkzeug[sp->get_player_nr()]->init(active_player);
		}
		
		if(  sp==active_player  ) {
			// reset pointer
			koord3d zpos = zeiger->get_pos();
			// remove marks
			zeiger->change_pos( koord3d::invalid );
			// set new cursor properties
			w->init_cursor(zeiger);
			// .. and mark again (if the position is acceptable for the tool)
			if( w->check_valid_pos(zpos.get_2d())) {
				zeiger->change_pos( zpos );
			}
			else {
				zeiger->change_pos( koord3d::invalid );
			}
		}
		werkzeug[sp->get_player_nr()] = w;
	}
	w->flags = 0;
}


sint8 karte_t::min_hgt_nocheck(const koord k) const
{
	// more optimised version of min_hgt code
	const sint8 * p = &grid_hgts[k.x + k.y*(sint32)(cached_grid_size.x+1)];

	const int h1 = *p;
	const int h2 = *(p+1);
	const int h3 = *(p+get_size().x+2);
	const int h4 = *(p+get_size().x+1);

	return min(min(h1,h2), min(h3,h4));
}


sint8 karte_t::max_hgt_nocheck(const koord k) const
{
	// more optimised version of max_hgt code
	const sint8 * p = &grid_hgts[k.x + k.y*(sint32)(cached_grid_size.x+1)];

	const int h1 = *p;
	const int h2 = *(p+1);
	const int h3 = *(p+get_size().x+2);
	const int h4 = *(p+get_size().x+1);

	return max(max(h1,h2), max(h3,h4));
}


sint8 karte_t::min_hgt(const koord k) const
{
	const sint8 h1 = lookup_hgt(k);
	const sint8 h2 = lookup_hgt(k+koord(1, 0));
	const sint8 h3 = lookup_hgt(k+koord(1, 1));
	const sint8 h4 = lookup_hgt(k+koord(0, 1));

	return min(min(h1,h2), min(h3,h4));
}


sint8 karte_t::max_hgt(const koord k) const
{
	const sint8 h1 = lookup_hgt(k);
	const sint8 h2 = lookup_hgt(k+koord(1, 0));
	const sint8 h3 = lookup_hgt(k+koord(1, 1));
	const sint8 h4 = lookup_hgt(k+koord(0, 1));

	return max(max(h1,h2), max(h3,h4));
}


planquadrat_t *rotate90_new_plan;
sint8 *rotate90_new_water;

void karte_t::rotate90_plans(sint16 x_min, sint16 x_max, sint16 y_min, sint16 y_max)
{
	const int LOOP_BLOCK = 64;
	if(  (loaded_rotation + settings.get_rotation()) & 1  ) {  // 1 || 3
		for(  int yy = y_min;  yy < y_max;  yy += LOOP_BLOCK  ) {
			for(  int xx = x_min;  xx < x_max;  xx += LOOP_BLOCK  ) {
				for(  int y = yy;  y < min(yy + LOOP_BLOCK, y_max);  y++  ) {
					for(  int x = xx;  x < min(xx + LOOP_BLOCK, x_max);  x++  ) {
						const int nr = x + (y * cached_grid_size.x);
						const int new_nr = (cached_size.y - y) + (x * cached_grid_size.y);
						// first rotate everything on the ground(s)
						for(  uint i = 0;  i < plan[nr].get_boden_count();  i++  ) {
							plan[nr].get_boden_bei(i)->rotate90();
						}
						// rotate climate transitions
						rotate_transitions( koord( x, y ) );
						// now: rotate all things on the map
						swap(rotate90_new_plan[new_nr], plan[nr]);
					}
				}
			}
		}
	}
	else {
		// first: rotate all things on the map
		for(  int xx = x_min;  xx < x_max;  xx += LOOP_BLOCK  ) {
			for(  int yy = y_min;  yy < y_max;  yy += LOOP_BLOCK  ) {
				for(  int x = xx;  x < min(xx + LOOP_BLOCK, x_max);  x++  ) {
					for(  int y=yy;  y < min(yy + LOOP_BLOCK, y_max);  y++  ) {
						// rotate climate transitions
						rotate_transitions( koord( x, y ) );
						const int nr = x + (y * cached_grid_size.x);
						const int new_nr = (cached_size.y - y) + (x * cached_grid_size.y);
						swap(rotate90_new_plan[new_nr], plan[nr]);
					}
				}
			}
		}
		// now rotate everything on the ground(s)
		for(  int xx = x_min;  xx < x_max;  xx += LOOP_BLOCK  ) {
			for(  int yy = y_min;  yy < y_max;  yy += LOOP_BLOCK  ) {
				for(  int x = xx;  x < min(xx + LOOP_BLOCK, x_max);  x++  ) {
					for(  int y = yy;  y < min(yy + LOOP_BLOCK, y_max);  y++  ) {
						const int new_nr = (cached_size.y - y) + (x * cached_grid_size.y);
						for(  uint i = 0;  i < rotate90_new_plan[new_nr].get_boden_count();  i++  ) {
							rotate90_new_plan[new_nr].get_boden_bei(i)->rotate90();
						}
					}
				}
			}
		}
	}

	// rotate water
	for(  int xx = 0;  xx < cached_grid_size.x;  xx += LOOP_BLOCK  ) {
		for(  int yy = y_min;  yy < y_max;  yy += LOOP_BLOCK  ) {
			for(  int x = xx;  x < min( xx + LOOP_BLOCK, cached_grid_size.x );  x++  ) {
				int nr = x + (yy * cached_grid_size.x);
				int new_nr = (cached_size.y - yy) + (x * cached_grid_size.y);
				for(  int y = yy;  y < min( yy + LOOP_BLOCK, y_max );  y++  ) {
					rotate90_new_water[new_nr] = water_hgts[nr];
					nr += cached_grid_size.x;
					new_nr--;
				}
			}
		}
	}
}


void karte_t::rotate90()
{
DBG_MESSAGE( "karte_t::rotate90()", "called" );
	// asumme we can save this rotation
	nosave_warning = nosave = false;

	//announce current target rotation
	settings.rotate90();

	// clear marked region
	zeiger->change_pos( koord3d::invalid );

	// preprocessing, detach stops from factories to prevent crash
	FOR(vector_tpl<halthandle_t>, const s, haltestelle_t::get_alle_haltestellen()) {
		s->release_factory_links();
	}

	//rotate plans in parallel posix thread ...
	rotate90_new_plan = new planquadrat_t[cached_grid_size.y * cached_grid_size.x];
	rotate90_new_water = new sint8[cached_grid_size.y * cached_grid_size.x];

	world_xy_loop(&karte_t::rotate90_plans, 0);

	grund_t::finish_rotate90();

	delete [] plan;
	plan = rotate90_new_plan;
	delete [] water_hgts;
	water_hgts = rotate90_new_water;

	// rotate heightmap
	sint8 *new_hgts = new sint8[(cached_grid_size.x+1)*(cached_grid_size.y+1)];
	const int LOOP_BLOCK = 64;
	for(  int yy=0;  yy<=cached_grid_size.y;  yy+=LOOP_BLOCK  ) {
		for(  int xx=0;  xx<=cached_grid_size.x;  xx+=LOOP_BLOCK  ) {
			for(  int x=xx;  x<=min(xx+LOOP_BLOCK,cached_grid_size.x);  x++  ) {
				for(  int y=yy;  y<=min(yy+LOOP_BLOCK,cached_grid_size.y);  y++  ) {
					const int nr = x+(y*(cached_grid_size.x+1));
					const int new_nr = (cached_grid_size.y-y)+(x*(cached_grid_size.y+1));
					new_hgts[new_nr] = grid_hgts[nr];
				}
			}
		}
	}
	delete [] grid_hgts;
	grid_hgts = new_hgts;

	// rotate borders
	sint16 xw = cached_size.x;
	cached_size.x = cached_size.y;
	cached_size.y = xw;

	int wx = cached_grid_size.x;
	cached_grid_size.x = cached_grid_size.y;
	cached_grid_size.y = wx;

	// now step all towns (to generate passengers)
	FOR(weighted_vector_tpl<stadt_t*>, const i, stadt) {
		i->rotate90(cached_size.x);
	}

	//fixed order fabrik, halts, convois
	FOR(vector_tpl<fabrik_t*>, const f, fab_list) {
		f->rotate90(cached_size.x);
	}
	// after rotation of factories, rotate everything that holds freight: stations and convoys
	FOR(vector_tpl<halthandle_t>, const s, haltestelle_t::get_alle_haltestellen()) {
		s->rotate90(cached_size.x);
	}
	// Factories need their halt lists recalculated after the halts are rotated.  Yuck!
	FOR(vector_tpl<fabrik_t*>, const f, fab_list) {
		f->recalc_nearby_halts();
	}

	FOR(vector_tpl<convoihandle_t>, const i, convoi_array) {
		i->rotate90(cached_size.x);
	}

	for(  int i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
		if(  spieler[i]  ) {
			spieler[i]->rotate90( cached_size.x );
		}
	}

	// rotate label texts
	FOR(slist_tpl<koord>, & l, labels) {
		l.rotate90(cached_size.x);
	}

	// rotate view
	viewport->rotate90( cached_size.x );

	// rotate messages
	msg->rotate90( cached_size.x );

	// rotate view in dialog windows
	win_rotate90( cached_size.x );

	if( cached_grid_size.x != cached_grid_size.y ) {
		// the map must be reinit
		reliefkarte_t::get_karte()->init();
	}

	//  rotate map search array
	fabrikbauer_t::neue_karte();

	// update minimap
	if(reliefkarte_t::is_visible) {
		reliefkarte_t::get_karte()->set_mode( reliefkarte_t::get_karte()->get_mode() );
	}

	get_scenario()->rotate90( cached_size.x );

	// finally recalculate schedules for goods in transit ...
	// Modified by : Knightly
	path_explorer_t::refresh_all_categories(true);

	set_dirty();
}
// -------- Verwaltung von Fabriken -----------------------------


bool karte_t::add_fab(fabrik_t *fab)
{
//DBG_MESSAGE("karte_t::add_fab()","fab = %p",fab);
	assert(fab != NULL);
	//fab_list.insert( fab );
	fab_list.append(fab);
	goods_in_game.clear(); // Force rebuild of goods list
	return true;
}


// beware: must remove also links from stops and towns
bool karte_t::rem_fab(fabrik_t *fab)
{
	if(!fab_list.is_contained(fab))
	{
		return false;
	}
	else
	{
		fab_list.remove(fab);
	}

	// Force rebuild of goods list
	goods_in_game.clear();

	// now all the interwoven connections must be cleared
	// This is hairy; a cleaner method would be desirable --neroden
	vector_tpl<koord> tile_list;
	fab->get_tile_list(tile_list);
	FOR (vector_tpl<koord>, const k, tile_list) {
		planquadrat_t* tile = access(k);
		if(tile)
		{
			// we need a copy, since the verbinde fabriken will modify the list
			const uint8 count = tile->get_haltlist_count();
			vector_tpl<nearby_halt_t> tmp_list;
			// Make it an appropriate size.
			tmp_list.resize(count);
			for(  uint8 i = 0;  i < count;  i++  ) {
				tmp_list.append( tile->get_haltlist()[i] );
			};
			for(  uint8 i = 0;  i < count;  i++  ) {
				// first remove all the tiles that do not connect
				// This will only remove if it is no longer connected
				tile->remove_from_haltlist( tmp_list[i].halt );
			}
		}
	}

	// OK, now stuff where we need not check every tile
	// Still double-check in case we were not on the map (which should not happen)
	koord pos = fab->get_pos().get_2d();
 	const planquadrat_t* tile = access(pos);
	if (tile) {

		// finally delete it
		delete fab;

		// recalculate factory position map
		fabrikbauer_t::neue_karte();
	}
	return true;
}

/*----------------------------------------------------------------------------------------------------------------------*/
/* same procedure for tourist attractions */


void karte_t::add_ausflugsziel(gebaeude_t *gb)
{
	assert(gb != NULL);
	ausflugsziele.append(gb, gb->get_adjusted_visitor_demand());
}


void karte_t::remove_ausflugsziel(gebaeude_t *gb)
{
	assert(gb != NULL);
	ausflugsziele.remove(gb);
	stadt_t* city = get_city(gb->get_pos().get_2d());
	if(!city)
	{
		remove_building_from_world_list(gb);
	}
}


// -------- Verwaltung von Staedten -----------------------------
// "look for next city" (Babelfish)

stadt_t *karte_t::suche_naechste_stadt(const koord k) const
{
	uint32 min_dist = 99999999;
	bool contains = false;
	stadt_t *best = NULL;	// within city limits

	if(  is_within_limits(k)  ) {
		FOR(  weighted_vector_tpl<stadt_t*>,  const s,  stadt  ) {
			if(  k.x >= s->get_linksoben().x  &&  k.y >= s->get_linksoben().y  &&  k.x < s->get_rechtsunten().x  &&  k.y < s->get_rechtsunten().y  ) {
				const long dist = koord_distance( k, s->get_center() );
				if(  !contains  ) {
					// no city within limits => this is best
					best = s;
					min_dist = dist;
				}
				else if(  (unsigned) dist < min_dist  ) {
					best = s;
					min_dist = dist;
				}
				contains = true;
			}
			else if(  !contains  ) {
				// so far no cities found within its city limit
				const uint32 dist = koord_distance( k, s->get_center() );
				if(  dist < min_dist  ) {
					best = s;
					min_dist = dist;
				}
			}
		}
	}
	return best;
}


stadt_t *karte_t::get_city(const koord pos) const
{
	stadt_t* city = NULL;

	if(is_within_limits(pos)) 
	{
		int cities = 0;
		FOR(weighted_vector_tpl<stadt_t*>, const c, stadt) 
		{
			if(c->is_within_city_limits(pos))
			{
				cities ++;
				if(cities > 1)
				{
					// We have a city within a city. Make sure to return the *inner* city.
					if(city->is_within_city_limits(c->get_pos()))
					{
						// "c" is the inner city: c's town hall is within the city limits of "city".
						city = c;
					}
				}
				else
				{
					city = c;
				}
			}
		}
	}
	return city;
}

// -------- Verwaltung von synchronen Objekten ------------------

static volatile bool sync_step_running = false;
static volatile bool sync_step_eyecandy_running = false;
static volatile bool sync_way_eyecandy_running = false;

// handling animations and the like
bool karte_t::sync_eyecandy_add(sync_steppable *obj)
{
	if(  sync_step_eyecandy_running  ) {
		sync_eyecandy_add_list.insert( obj );
	}
	else {
		sync_eyecandy_list.put( obj, obj );
	}
	return true;
}


bool karte_t::sync_eyecandy_remove(sync_steppable *obj)	// entfernt alle dinge == obj aus der Liste
{
	if(  sync_step_eyecandy_running  ) {
		sync_eyecandy_remove_list.append(obj);
	}
	else {
		if(  sync_eyecandy_add_list.remove(obj)  ) {
			return true;
		}
		return sync_eyecandy_list.remove(obj)!=NULL;
	}
	return false;
}


void karte_t::sync_eyecandy_step(long delta_t)
{
	sync_step_eyecandy_running = true;
	// first add everything
	while(  !sync_eyecandy_add_list.empty()  ) {
		sync_steppable *obj = sync_eyecandy_add_list.remove_first();
		sync_eyecandy_list.put( obj, obj );
	}
	// now remove everything from last time
	sync_step_eyecandy_running = false;
	while(  !sync_eyecandy_remove_list.empty()  ) {
		sync_eyecandy_list.remove( sync_eyecandy_remove_list.remove_first() );
	}
	// now step ...
	sync_step_eyecandy_running = true;
	for(  ptrhashtable_tpl<sync_steppable*,sync_steppable*>::iterator iter = sync_eyecandy_list.begin();  iter != sync_eyecandy_list.end();  ) {
		// if false, then remove
		sync_steppable *ss = iter->key;
		if(!ss->sync_step(delta_t)) {
			iter = sync_eyecandy_list.erase(iter);
			delete ss;
		}
		else {
			++iter;
		}
	}
	// now remove everything from last time
	sync_step_eyecandy_running = false;
	while(  !sync_eyecandy_remove_list.empty()  ) {
		sync_eyecandy_list.remove( sync_eyecandy_remove_list.remove_first() );
	}
	sync_step_eyecandy_running = false;
}


// and now the same for pedestrians
bool karte_t::sync_way_eyecandy_add(sync_steppable *obj)
{
	if(  sync_way_eyecandy_running  ) {
		sync_way_eyecandy_add_list.insert( obj );
	}
	else {
		sync_way_eyecandy_list.append( obj );
	}
	return true;
}


bool karte_t::sync_way_eyecandy_remove(sync_steppable *obj)	// entfernt alle dinge == obj aus der Liste
{
	if(  sync_way_eyecandy_running  ) {
		sync_way_eyecandy_remove_list.append(obj);
	}
	else {
		if(  sync_way_eyecandy_add_list.remove(obj)  ) {
			return true;
		}
		return sync_way_eyecandy_list.remove(obj);
	}
	return false;
}


void karte_t::sync_way_eyecandy_step(long delta_t)
{
	sync_way_eyecandy_running = true;
	// first add everything
	while(  !sync_way_eyecandy_add_list.empty()  ) {
		sync_steppable *obj = sync_way_eyecandy_add_list.remove_first();
		sync_way_eyecandy_list.append( obj );
	}
	// now remove everything from last time
	sync_way_eyecandy_running = false;
	while(  !sync_way_eyecandy_remove_list.empty()  ) {
		sync_way_eyecandy_list.remove( sync_way_eyecandy_remove_list.remove_first() );
	}
	// now the actualy stepping
	sync_way_eyecandy_running = true;
#ifndef SYNC_VECTOR
	for(  slist_tpl<sync_steppable*>::iterator i=sync_way_eyecandy_list.begin();  !i.end();  ) {
		// if false, then remove
		sync_steppable *ss = *i;
		if(!ss->sync_step(delta_t)) {
			i = sync_list.erase(i);
			delete ss;
		}
		else {
			++i;
		}
	}
#else
	static vector_tpl<sync_steppable *> sync_way_eyecandy_list_copy;
	sync_way_eyecandy_list_copy.resize( (uint32) (sync_way_eyecandy_list.get_count()*1.1) );
	FOR(vector_tpl<sync_steppable*>, const ss, sync_way_eyecandy_list) {
		// if false, then remove
		if(!ss->sync_step(delta_t)) {
			delete ss;
		}
		else {
			sync_way_eyecandy_list_copy.append( ss );
		}
	}
	swap( sync_way_eyecandy_list_copy, sync_way_eyecandy_list );
	sync_way_eyecandy_list_copy.clear();
#endif
	// now remove everything from last time
	sync_way_eyecandy_running = false;
	while(  !sync_way_eyecandy_remove_list.empty()  ) {
		sync_way_eyecandy_list.remove( sync_way_eyecandy_remove_list.remove_first() );
	}
}


// ... and now all regular stuff, which needs to are in the same order on any plattform
// Thus we are using (slower) lists/vectors and no pointerhashtables
bool karte_t::sync_add(sync_steppable *obj)
{
	if(  sync_step_running  ) {
		sync_add_list.insert( obj );
	}
	else {
		sync_list.append( obj );
	}
	return true;
}


bool karte_t::sync_remove(sync_steppable *obj)	// entfernt alle dinge == obj aus der Liste
{
	if(  sync_step_running  ) {
		sync_remove_list.append(obj);
	}
	else {
		if(sync_add_list.remove(obj)) {
			return true;
		}
		return sync_list.remove(obj);
	}
	return false;
}


/*
 * this routine is called before an image is displayed
 * it moves vehicles and pedestrians
 * only time consuming thing are done in step()
 * everything else is done here
 */
void karte_t::sync_step(long delta_t, bool sync, bool display )
{
rands[0] = get_random_seed();
rands[2] = 0;
rands[3] = 0;
rands[4] = 0;
rands[5] = 0;
rands[6] = 0;
rands[7] = 0;
	set_random_mode( SYNC_STEP_RANDOM );
	haltestelle_t::pedestrian_limit = 0;
	if(sync) {
		// only omitted, when called to display a new frame during fast forward

		// just for progress
		if(  delta_t > 10000  ) {
			dbg->error( "karte_t::sync_step()", "delta_t too large: %li", delta_t );
			delta_t = 10000;
		}
		ticks += delta_t;

		set_random_mode( INTERACTIVE_RANDOM );

		/* animations do not require exact sync
		 * foundations etc are added removed freuently during city growth
		 * => they are now in a hastable!
		 */
		sync_eyecandy_step( delta_t );

		/* pedestrians do not require exact sync and are added/removed frequently
		 * => they are now in a hastable!
		 */
		sync_way_eyecandy_step( delta_t );

		clear_random_mode( INTERACTIVE_RANDOM );

		/* and now the rest for the other moving stuff */
		sync_step_running = true;
#ifndef SYNC_VECTOR
		// insert new objects created during last sync_step (eg vehicle smoke)
		if(!sync_add_list.empty()) {
			sync_list.append_list(sync_add_list);
		}
#else
		while(  !sync_add_list.empty()  ) {
			sync_list.append( sync_add_list.remove_first() );
		}
#endif

		// now remove everything from last time
		sync_step_running = false;
		while(  !sync_remove_list.empty()  ) {
			sync_list.remove( sync_remove_list.remove_first() );
		}

		sync_step_running = true;
#ifndef SYNC_VECTOR
		for(  slist_tpl<sync_steppable*>::iterator i=sync_list.begin();  !i.end();  /* Note no ++i */ ) {
			// if false, then remove
			sync_steppable *ss = *i;
			if(!ss->sync_step(delta_t)) {
				i = sync_list.erase(i);
				delete ss;
			}
			else {
				++i;
			}
		}
#else
		static vector_tpl<sync_steppable *> sync_list_copy;
		sync_list_copy.resize( sync_list.get_count() );
		FOR(vector_tpl<sync_steppable*>, const ss, sync_list) {
			// if false, then remove
			if(!ss->sync_step(delta_t)) {
				delete ss;
			}
			else {
				sync_list_copy.append( ss );
			}
		}
		swap( sync_list_copy, sync_list );
		sync_list_copy.clear();
#endif

		// now remove everything from this time
		sync_step_running = false;
		while(!sync_remove_list.empty()) {
			sync_list.remove( sync_remove_list.remove_first() );
		}

		sync_step_running = false;
	}
rands[1] = get_random_seed();

	if(display) {
		// only omitted in fast forward mode for the magic steps

		for(int x=0; x<MAX_PLAYER_COUNT-1; x++) {
			if(spieler[x]) {
				spieler[x]->age_messages(delta_t);
			}
		}

		// change view due to following a convoi?
		convoihandle_t follow_convoi = viewport->get_follow_convoi();
		if(follow_convoi.is_bound()  &&  follow_convoi->get_vehikel_anzahl()>0) {
			vehikel_t const& v       = *follow_convoi->front();
			koord3d   const  new_pos = v.get_pos();
			if(new_pos!=koord3d::invalid) {
				const sint16 rw = get_tile_raster_width();
				int new_xoff = 0;
				int new_yoff = 0;
				v.get_screen_offset( new_xoff, new_yoff, get_tile_raster_width() );
				new_xoff -= tile_raster_scale_x(-v.get_xoff(), rw);
				new_yoff -= tile_raster_scale_y(-v.get_yoff(), rw) + tile_raster_scale_y(new_pos.z * TILE_HEIGHT_STEP, rw);
				viewport->change_world_position( new_pos.get_2d(), -new_xoff, -new_yoff );
			}
		}

		// display new frame with water animation
		intr_refresh_display( false );
		update_frame_sleep_time(delta_t);
	}
	clear_random_mode( SYNC_STEP_RANDOM );
}


// does all the magic about frame timing
void karte_t::update_frame_sleep_time(long /*delta*/)
{
	// get average frame time
	uint32 last_ms = dr_time();
	last_frame_ms[last_frame_idx] = last_ms;
	last_frame_idx = (last_frame_idx+1) % 32;
	if(last_frame_ms[last_frame_idx]<last_ms) {
		realFPS = (32000u) / (last_ms-last_frame_ms[last_frame_idx]);
	}
	else {
		realFPS = env_t::fps;
		simloops = 60;
	}

	if(  step_mode&PAUSE_FLAG  ) {
		// not changing pauses
		next_step_time = dr_time()+100;
		idle_time = 100;
	}
	else if(  step_mode==FIX_RATIO) {
		simloops = realFPS;
	}
	else if(step_mode==NORMAL) {
		// calculate simloops
		uint16 last_step = (steps+31)%32;
		if(last_step_nr[last_step]>last_step_nr[steps%32]) {
			simloops = (10000*32l)/(last_step_nr[last_step]-last_step_nr[steps%32]);
		}
		// (de-)activate faster redraw
		env_t::simple_drawing = (env_t::simple_drawing_normal >= get_tile_raster_width());

		// calaculate and activate fast redraw ..
		if(  realFPS > (env_t::fps*17/16)  ) {
			// decrease fast tile zoom by one
			if(  env_t::simple_drawing_normal > env_t::simple_drawing_default  ) {
				env_t::simple_drawing_normal --;
			}
		}
		else if(  realFPS < env_t::fps/2  ) {
			// activate simple redraw
			env_t::simple_drawing_normal = max( env_t::simple_drawing_normal, get_tile_raster_width()+1 );
		}
		else if(  realFPS < (env_t::fps*15)/16  )  {
			// increase fast tile redraw by one if below current tile size
			if(  env_t::simple_drawing_normal <= (get_tile_raster_width()*3)/2  ) {
				env_t::simple_drawing_normal ++;
			}
		}
		else if(  idle_time > 0  ) {
			// decrease fast tile zoom by one
			if(  env_t::simple_drawing_normal > env_t::simple_drawing_default  ) {
				env_t::simple_drawing_normal --;
			}
		}
		env_t::simple_drawing = (env_t::simple_drawing_normal >= get_tile_raster_width());

		// way too slow => try to increase time ...
		if(  last_ms-last_interaction > 100  ) {
			if(  last_ms-last_interaction > 500  ) {
				set_frame_time( 1+get_frame_time() );
				// more than 1s since last zoom => check if zoom out is a way to improve it
				if(  last_ms-last_interaction > 5000  &&  get_current_tile_raster_width() < 32  ) {
					zoom_factor_up();
					set_dirty();
					last_interaction = last_ms-1000;
				}
			}
			else {
				increase_frame_time();
				increase_frame_time();
				increase_frame_time();
				increase_frame_time();
			}
		}
		else {
			// change frame spacing ... (pause will be changed by step() directly)
			if(realFPS>(env_t::fps*17/16)) {
				increase_frame_time();
			}
			else if(realFPS<env_t::fps) {
				if(  1000u/get_frame_time() < 2*realFPS  ) {
					if(  realFPS < (env_t::fps/2)  ) {
						set_frame_time( get_frame_time()-1 );
						next_step_time = last_ms;
					}
					else {
						reduce_frame_time();
					}
				}
				else {
					// do not set time too short!
					set_frame_time( 500/max(1,realFPS) );
					next_step_time = last_ms;
				}
			}
		}
	}
	else  { // here only with fyst forward ...
		// try to get 10 fps or lower rate (if set)
		sint32 frame_intervall = max( 100, 1000/env_t::fps );
		if(get_frame_time()>frame_intervall) {
			reduce_frame_time();
		}
		else {
			increase_frame_time();
		}
		// (de-)activate faster redraw
		env_t::simple_drawing = env_t::simple_drawing_fast_forward  ||  (env_t::simple_drawing_normal >= get_tile_raster_width());
	}
}


// add an amout to a subcategory
void karte_t::buche(sint64 const betrag, player_cost const type)
{
	assert(type < MAX_WORLD_COST);
	finance_history_year[0][type] += betrag;
	finance_history_month[0][type] += betrag;
	// to do: check for dependecies
}


inline sint32 get_population(stadt_t const* const c)
{
	return c->get_einwohner();
}


void karte_t::new_month()
{
	update_history();

	// advance history ...
	last_month_bev = finance_history_month[0][WORLD_CITICENS];
	for(  int hist=0;  hist<karte_t::MAX_WORLD_COST;  hist++  ) {
		for( int y=MAX_WORLD_HISTORY_MONTHS-1; y>0;  y--  ) {
			finance_history_month[y][hist] = finance_history_month[y-1][hist];
		}
	}

	finance_history_month[0][WORLD_CITYCARS] = 0;

	current_month ++;
	last_month ++;
	if( last_month > 11 ) {
		last_month = 0;
	}
	DBG_MESSAGE("karte_t::new_month()","Month (%d/%d) has started", (last_month%12)+1, last_month/12 );
	DBG_MESSAGE("karte_t::new_month()","sync_step %u objects", sync_list.get_count() );

	// this should be done before a map update, since the map may want an update of the way usage
//	DBG_MESSAGE("karte_t::new_month()","ways");
	FOR(slist_tpl<weg_t*>, const w, weg_t::get_alle_wege()) {
		w->neuer_monat();
	}

//	DBG_MESSAGE("karte_t::new_month()","depots");
	// Bernd Gabriel - call new month for depots	
	FOR(slist_tpl<depot_t *>, const dep, depot_t::get_depot_list()) {
		dep->neuer_monat();
	}

	// recalc old settings (and maybe update the stops with the current values)
	reliefkarte_t::get_karte()->neuer_monat();

	INT_CHECK("simworld 3042");

//	DBG_MESSAGE("karte_t::new_month()","convois");
	// hsiegeln - call new month for convois
	FOR(vector_tpl<convoihandle_t>, const cnv, convoi_array) {
		cnv->new_month();
	}

	base_pathing_counter ++;

	INT_CHECK("simworld 3053"); 
	 

//	DBG_MESSAGE("karte_t::new_month()","factories");
	uint32 total_electric_demand = 1;
	uint32 electric_productivity = 0;
	closed_factories_this_month.clear();
	uint32 closed_factories_count = 0;
	FOR(vector_tpl<fabrik_t*>, const fab, fab_list)
	{
		if(!closed_factories_this_month.is_contained(fab))
		{
			fab->neuer_monat();
			// Check to see whether the factory has closed down - if so, the pointer will be dud.
			if(closed_factories_count == closed_factories_this_month.get_count())
			{
				if(fab->get_besch()->is_electricity_producer())
				{
					electric_productivity += fab->get_scaled_electric_amount();
				} 
				else 
				{
					total_electric_demand += fab->get_scaled_electric_amount();
				}
			}
			else
			{
				closed_factories_count = closed_factories_this_month.get_count();
			}
		}
	}

	FOR(vector_tpl<fabrik_t*>, const fab, closed_factories_this_month)
	{
		if(fab_list.is_contained(fab)) 
		{
			gebaeude_t* gb = fab->get_building();
			hausbauer_t::remove(get_spieler(1), gb);
		}
	}

	// Check to see whether more factories need to be added
	// to replace ones that have closed.
	// @author: jamespetts

	if(industry_density_proportion == 0 && finance_history_month[0][WORLD_CITICENS] > 0)
	{
		// Set the industry density proportion for the first time when the number of citizens is populated.
		industry_density_proportion = (uint32)((sint64)actual_industry_density * 1000000ll) / finance_history_month[0][WORLD_CITICENS];
	}
	const uint32 target_industry_density = get_target_industry_density();
	if(actual_industry_density < target_industry_density)
	{
		// Only add one chain per month, and randomise (with a minimum of 8% chance to ensure that any industry deficiency is, on average, remedied in about a year).
		const uint32 percentage = max((((target_industry_density - actual_industry_density) * 100) / target_industry_density), 8);
		const uint32 chance = simrand(100, "void karte_t::new_month()");
		if(chance < percentage)
		{
			fabrikbauer_t::increase_industry_density(true, true);
		}
	}

	INT_CHECK("simworld 3105");

	// Check attractions' road connexions
	FOR(weighted_vector_tpl<gebaeude_t*>, const &i, ausflugsziele)
	{
		i->check_road_tiles(false);
	}


	//	DBG_MESSAGE("karte_t::new_month()","cities");
	stadt.update_weights(get_population);
	FOR(weighted_vector_tpl<stadt_t*>, const s, stadt) 
	{
		if(recheck_road_connexions) 
		{
			cities_awaiting_private_car_route_check.append_unique(s);
		}
		s->neuer_monat(recheck_road_connexions);
		//INT_CHECK("simworld 3117");
		total_electric_demand += s->get_power_demand();
	}
	recheck_road_connexions = false;

	if(fabrikbauer_t::power_stations_available() && total_electric_demand && (((sint64)electric_productivity * 4000l) / total_electric_demand) < (sint64)get_settings().get_electric_promille())
	{
		// Add industries if there is a shortage of electricity - power stations will be built.
		// Also, check whether power stations are available, or else large quantities of other industries will
		// be built instead every month.
		fabrikbauer_t::increase_industry_density(true, true, true);
	}

	INT_CHECK("simworld 3130");

	// spieler
	for(uint i=0; i<MAX_PLAYER_COUNT; i++) {
		if( last_month == 0  &&  !settings.is_freeplay() ) {
			// remove all player (but first and second) who went bankrupt during last year
			if(  spieler[i] != NULL  &&  spieler[i]->get_finance()->is_bankrupted()  )
			{
				remove_player(i);
			}
		}

		if(  spieler[i] != NULL  ) {
			// if returns false -> remove player
			if (!spieler[i]->neuer_monat()) {
				remove_player(i);
			}
		}
	}
	// update the window
	ki_kontroll_t* playerwin = (ki_kontroll_t*)win_get_magic(magic_ki_kontroll_t);
	if(  playerwin  ) {
		playerwin->update_data();
	}

	INT_CHECK("simworld 3175");

//	DBG_MESSAGE("karte_t::neuer_monat()","halts");
	FOR(vector_tpl<halthandle_t>, const s, haltestelle_t::get_alle_haltestellen()) {
		s->neuer_monat();
		INT_CHECK("simworld 1877");
	}

	INT_CHECK("simworld 2522");
	FOR(slist_tpl<depot_t *>, const& iter, depot_t::get_depot_list())
	{
		iter->neuer_monat();
	}

	scenario->new_month();

	// now switch year to get the right year for all timeline stuff ...
	if( last_month == 0 ) {
		new_year();
		INT_CHECK("simworld 1299");
	}

	wegbauer_t::neuer_monat();
	INT_CHECK("simworld 1299");

	// Check whether downstream substations have become engulfed by
	// an expanding city.
	FOR(slist_tpl<senke_t *>, & senke_iter, senke_t::senke_list)
	{
		// This will add a city if the city has engulfed the substation, and remove a city if
		// the city has been deleted or become smaller. 
		senke_t* const substation = senke_iter;
		stadt_t* const city = get_city(substation->get_pos().get_2d());
		substation->set_city(city);
		if(city)
		{
			city->add_substation(substation);
		}
		else
		{		
			// Check whether an industry has placed itself near the substation.
			substation->check_industry_connexion();
		}
	}

	recalc_average_speed();
	INT_CHECK("simworld 1921");

	// update toolbars (i.e. new waytypes
	werkzeug_t::update_toolbars();


	if( !env_t::networkmode  &&  env_t::autosave>0  &&  last_month%env_t::autosave==0 ) {
		char buf[128];
		sprintf( buf, "save/autosave%02i.sve", last_month+1 );
		save( buf, loadsave_t::autosave_mode, env_t::savegame_version_str, env_t::savegame_ex_version_str, true );
	}

	settings.update_max_alternative_destinations_commuting(commuter_targets.get_sum_weight());
	settings.update_max_alternative_destinations_visiting(visitor_targets.get_sum_weight());

	set_citycar_speed_average();
	calc_generic_road_time_per_tile_city();
	calc_generic_road_time_per_tile_intercity();
	calc_max_road_check_depth();

	// Added by : Knightly
	// Note		: This should be done after all lines and convoys have rolled their statistics
	path_explorer_t::refresh_all_categories(true);
}


void karte_t::new_year()
{
	last_year = current_month/12;

	// advance history ...
	for(  int hist=0;  hist<karte_t::MAX_WORLD_COST;  hist++  ) {
		for( int y=MAX_WORLD_HISTORY_YEARS-1; y>0;  y--  ) {
			finance_history_year[y][hist] = finance_history_year[y-1][hist];
		}
	}

DBG_MESSAGE("karte_t::new_year()","speedbonus for %d %i, %i, %i, %i, %i, %i, %i, %i", last_year,
			average_speed[0], average_speed[1], average_speed[2], average_speed[3], average_speed[4], average_speed[5], average_speed[6], average_speed[7] );

	cbuffer_t buf;
	buf.printf( translator::translate("Year %i has started."), last_year );
	msg->add_message(buf,koord::invalid,message_t::general,COL_BLACK,skinverwaltung_t::neujahrsymbol->get_bild_nr(0));

	FOR(vector_tpl<convoihandle_t>, const cnv, convoi_array) {
		cnv->neues_jahr();
	}

	for(int i=0; i<MAX_PLAYER_COUNT; i++) {
		if(  spieler[i] != NULL  ) {
			spieler[i]->neues_jahr();
		}
	}

	for(weighted_vector_tpl<gebaeude_t *>::const_iterator a = ausflugsziele.begin(), end = ausflugsziele.end(); a != end; ++a)
	{
		(*a)->new_year();
	}

	finance_history_year[0][WORLD_CITYCARS] = 0;

	scenario->new_year();
}


// recalculated speed boni for different vehicles
// and takes care of all timeline stuff
void karte_t::recalc_average_speed()
{
	// retire/allocate vehicles
	stadtauto_t::built_timeline_liste(this);

	const uint32 speed_bonus_percent = get_settings().get_speed_bonus_multiplier_percent();
	for(int i=road_wt; i<=narrowgauge_wt; i++) {
		const int typ = i==4 ? 3 : (i-1)&7;
		const uint32 base_speed_bonus = vehikelbauer_t::get_speedbonus( this->get_timeline_year_month(), i==4 ? air_wt : (waytype_t)i );
		average_speed[typ] = (base_speed_bonus * speed_bonus_percent) / 100;
	}

	//	DBG_MESSAGE("karte_t::recalc_average_speed()","");
	if(use_timeline()) {
		for(int i=road_wt; i<=air_wt; i++) {
			const char *vehicle_type=NULL;
			switch(i) {
				case road_wt:
					vehicle_type = "road vehicle";
					break;
				case track_wt:
					vehicle_type = "rail car";
					break;
				case water_wt:
					vehicle_type = "water vehicle";
					break;
				case monorail_wt:
					vehicle_type = "monorail vehicle";
					break;
				case tram_wt:
					vehicle_type = "street car";
					break;
				case air_wt:
					vehicle_type = "airplane";
					break;
				case maglev_wt:
					vehicle_type = "maglev vehicle";
					break;
				case narrowgauge_wt:
					vehicle_type = "narrowgauge vehicle";
					break;
				default:
					// this is not a valid waytype
					continue;
			}
			vehicle_type = translator::translate( vehicle_type );

			FOR(slist_tpl<vehikel_besch_t *>, const info, vehikelbauer_t::get_info((waytype_t)i)) 
			{
				const uint16 intro_month = info->get_intro_year_month();
				if(intro_month == current_month) 
				{
					if(info->is_available_only_as_upgrade())
					{
						cbuffer_t buf;
						buf.printf(translator::translate("Upgrade to %s now available:\n%s\n"), vehicle_type, translator::translate(info->get_name()));
						msg->add_message(buf,koord::invalid,message_t::new_vehicle,NEW_VEHICLE,info->get_basis_bild());
					}
					else
					{
						cbuffer_t buf;
						buf.printf( translator::translate("New %s now available:\n%s\n"), vehicle_type, translator::translate(info->get_name()) );
						msg->add_message(buf,koord::invalid,message_t::new_vehicle,NEW_VEHICLE,info->get_basis_bild());
					}
				}

				const uint16 retire_month = info->get_retire_year_month();
				if(retire_month == current_month) 
				{
					cbuffer_t buf;
					buf.printf( translator::translate("Production of %s has been stopped:\n%s\n"), vehicle_type, translator::translate(info->get_name()) );
					msg->add_message(buf,koord::invalid,message_t::new_vehicle,NEW_VEHICLE,info->get_basis_bild());
				}

				const uint16 obsolete_month = info->get_obsolete_year_month(this);
				if(obsolete_month == current_month) 
				{
					cbuffer_t buf;
					buf.printf(translator::translate("The following %s has become obsolete:\n%s\n"), vehicle_type, translator::translate(info->get_name()));
					msg->add_message(buf,koord::invalid,message_t::new_vehicle,COL_DARK_BLUE,info->get_basis_bild());
				}
			}
		}

		// city road (try to use always a timeline)
		if (weg_besch_t const* city_road_test = settings.get_city_road_type(current_month) ) {
			city_road = city_road_test;
		}
		else {
			DBG_MESSAGE("karte_t::new_month()","Month %d has started", last_month);
			city_road = wegbauer_t::weg_search(road_wt,50,get_timeline_year_month(),weg_t::type_flat);
		}

	}
	else {
		// defaults
		city_road = settings.get_city_road_type(0);
		if(city_road==NULL) {
			city_road = wegbauer_t::weg_search(road_wt,50,0,weg_t::type_flat);
		}
	}
}


// returns the current speed record
sint32 karte_t::get_record_speed( waytype_t w ) const
{
	return records->get_record_speed(w);
}


// sets the new speed record
void karte_t::notify_record( convoihandle_t cnv, sint32 max_speed, koord k )
{
	records->notify_record(cnv, max_speed, k, current_month);
}


void karte_t::set_schedule_counter()
{
	// do not call this from gui when playing in network mode!
	assert( (get_random_mode() & INTERACTIVE_RANDOM) == 0  );

	schedule_counter++;
}


void karte_t::step()
{
rands[8] = get_random_seed();
	DBG_DEBUG4("karte_t::step", "start step");
	unsigned long time = dr_time();

	// calculate delta_t before handling overflow in ticks
	const long delta_t = (long)(ticks-last_step_ticks);

	// first: check for new month
	if(ticks > next_month_ticks) {

		// avoid overflow here ...
		// Should not overflow: now using 64-bit values.
		// @author: jamespetts
/*
		if(  next_month_ticks > next_month_ticks+karte_t::ticks_per_world_month  ) {
			// avoid overflow here ...
			dbg->warning( "karte_t::step()", "Ticks were overflowing => resetted" );
			ticks %= karte_t::ticks_per_world_month;
			next_month_ticks %= karte_t::ticks_per_world_month;
		}
*/
		next_month_ticks += karte_t::ticks_per_world_month;

		DBG_DEBUG4("karte_t::step", "calling neuer_monat");
		new_month();
	}
rands[9] = get_random_seed();

	DBG_DEBUG4("karte_t::step", "time calculations");
	if(  step_mode==NORMAL  ) {
		/* Try to maintain a decent pause, with a step every 170-250 ms (~5,5 simloops/s)
		 * Also avoid too large or negative steps
		 */

		// needs plausibility check?!?
		if(delta_t>10000  || delta_t<0) {
			dbg->error( "karte_t::step()", "delta_t (%li) out of bounds!", delta_t );
			last_step_ticks = ticks;
			next_step_time = time+10;
			return;
		}
		idle_time = 0;
		last_step_nr[steps%32] = ticks;
		next_step_time = time+(3200/get_time_multiplier());
	}
	else if(  step_mode==FAST_FORWARD  ) {
		// fast forward first: get average simloops (i.e. calculate acceleration)
		last_step_nr[steps%32] = dr_time();
		int last_5_simloops = simloops;
		if(  last_step_nr[(steps+32-5)%32] < last_step_nr[steps%32]  ) {
			// since 5 steps=1s
			last_5_simloops = (1000) / (last_step_nr[steps%32]-last_step_nr[(steps+32-5)%32]);
		}
		if(  last_step_nr[(steps+1)%32] < last_step_nr[steps%32]  ) {
			simloops = (10000*32) / (last_step_nr[steps%32]-last_step_nr[(steps+1)%32]);
		}
		// now try to approach the target speed
		if(last_5_simloops<env_t::max_acceleration) {
			if(idle_time>0) {
				idle_time --;
			}
		}
		else if(simloops>8u*env_t::max_acceleration) {
			if((long)idle_time<get_frame_time()-10) {
				idle_time ++;
			}
		}
		// cap it ...
		if( (long)idle_time>=get_frame_time()-10) {
			idle_time = get_frame_time()-10;
		}
		next_step_time = time+idle_time;
	}
	else {
		// network mode
	}
	// now do the step ...
	last_step_ticks = ticks;
	steps ++;

	// to make sure the tick counter will be updated
	INT_CHECK("karte_t::step");

	// check for pending seasons change
	if(pending_season_change>0) {
		// process
		const uint32 end_count = min( cached_grid_size.x*cached_grid_size.y,  tile_counter + max( 16384, cached_grid_size.x*cached_grid_size.y/16 ) );
		DBG_DEBUG4("karte_t::step", "pending_season_change. %u tiles.", end_count);
		while(  tile_counter < end_count  ) {
			plan[tile_counter].check_season(current_month);
			tile_counter ++;
			if((tile_counter&0x3FF)==0) {
				INT_CHECK("karte_t::step");
			}
		}

		if(  tile_counter >= (uint32)cached_grid_size.x*(uint32)cached_grid_size.y  ) {
			pending_season_change --;
			tile_counter = 0;
		}
	}
rands[10] = get_random_seed();

	// to make sure the tick counter will be updated
	INT_CHECK("karte_t::step 1");

	// Knightly : calling global path explorer
	path_explorer_t::step();
rands[11] = get_random_seed();
	INT_CHECK("karte_t::step 2");
	
	DBG_DEBUG4("karte_t::step 4", "step %d convois", convoi_array.get_count());
	// since convois will be deleted during stepping, we need to step backwards
	for (size_t i = convoi_array.get_count(); i-- != 0;) {
		convoihandle_t cnv = convoi_array[i];
		cnv->step();
		if((i&7)==0) {
			INT_CHECK("karte_t::step 5");
		}
	}
rands[12] = get_random_seed();

	if(cities_awaiting_private_car_route_check.get_count() > 0 && (steps % 12) == 0)
	{
		stadt_t* city = cities_awaiting_private_car_route_check.remove_first();
		city->check_all_private_car_routes();
		city->set_check_road_connexions(false);
	}
rands[13] = get_random_seed();


	// now step all towns 
	DBG_DEBUG4("karte_t::step 6", "step cities");
	FOR(weighted_vector_tpl<stadt_t*>, const i, stadt) {
		i->step(delta_t);
		rands[21] += i->get_einwohner();
		rands[22] += i->get_buildings();
	}
rands[14] = get_random_seed();
rands[23] = 0;

rands[24] = 0;
rands[25] = 0;
rands[26] = 0;
rands[27] = 0;
rands[28] = 0;
rands[29] = 0;
rands[30] = 0;
rands[31] = 0;
rands[23] = 0;

	step_passengers_and_mail(delta_t);

	// the inhabitants stuff
	finance_history_year[0][WORLD_CITICENS] = finance_history_month[0][WORLD_CITICENS] = 0;
	finance_history_year[0][WORLD_JOBS] = finance_history_month[0][WORLD_JOBS] = 0;
	finance_history_year[0][WORLD_VISITOR_DEMAND] = finance_history_month[0][WORLD_VISITOR_DEMAND] = 0;

	FOR(weighted_vector_tpl<stadt_t*>, const city, stadt) 
	{
		finance_history_year[0][WORLD_CITICENS] += city->get_finance_history_month(0, HIST_CITICENS);
		finance_history_month[0][WORLD_CITICENS] += city->get_finance_history_year(0, HIST_CITICENS);

		finance_history_month[0][WORLD_JOBS] += city->get_finance_history_month(0, HIST_JOBS);
		finance_history_year[0][WORLD_JOBS] += city->get_finance_history_year(0, HIST_JOBS);

		finance_history_month[0][WORLD_VISITOR_DEMAND] += city->get_finance_history_month(0, HIST_VISITOR_DEMAND);
		finance_history_year[0][WORLD_VISITOR_DEMAND] += city->get_finance_history_year(0, HIST_VISITOR_DEMAND);
	}

	DBG_DEBUG4("karte_t::step", "step factories");
	FOR(vector_tpl<fabrik_t*>, const f, fab_list) {
		f->step(delta_t);
	}
rands[16] = get_random_seed();

	finance_history_year[0][WORLD_FACTORIES] = finance_history_month[0][WORLD_FACTORIES] = fab_list.get_count();

	// step powerlines - required order: pumpe, senke, then powernet
	DBG_DEBUG4("karte_t::step", "step poweline stuff");
	pumpe_t::step_all( delta_t );
	senke_t::step_all( delta_t );
	powernet_t::step_all( delta_t );
rands[17] = get_random_seed();

	DBG_DEBUG4("karte_t::step", "step players");
	// then step all players
	for(  int i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
		if(  spieler[i] != NULL  ) {
			spieler[i]->step();
		}
	}
rands[18] = get_random_seed();

	DBG_DEBUG4("karte_t::step", "step halts");
	haltestelle_t::step_all();
rands[19] = get_random_seed();

	// Re-check paths if the time has come. 
	// Long months means that it might be necessary to do
	// this more than once per month to get up to date
	// routings for goods/passengers.
	// Default: 8192 ~ 1h (game time) at 125m/tile.

	if((steps % get_settings().get_reroute_check_interval_steps()) == 0)
	{
		path_explorer_t::refresh_all_categories(true);
	}

	// ok, next step
	INT_CHECK("karte_t::step 6");

	if((steps%8)==0) {
		DBG_DEBUG4("karte_t::step", "checkmidi");
		check_midi();
	}

	// will also call all objects if needed ...
	if(  recalc_snowline()  ) {
		pending_season_change ++;
	}

	// number of playing clients changed
	if(  env_t::server  &&  last_clients!=socket_list_t::get_playing_clients()  ) {
		if(  env_t::server_announce  ) {
			// inform the master server
			announce_server( 1 );
		}

		// check if player has left and send message
		for(uint32 i=0; i < socket_list_t::get_count(); i++) {
			socket_info_t& info = socket_list_t::get_client(i);
			if (info.state == socket_info_t::has_left) {
				nwc_nick_t::server_tools(this, i, nwc_nick_t::FAREWELL, NULL);
				info.state = socket_info_t::inactive;
			}
		}
		last_clients = socket_list_t::get_playing_clients();
		// add message via tool
		cbuffer_t buf;
		buf.printf(translator::translate("Now %u clients connected.", settings.get_name_language_id()), last_clients);
		werkzeug_t *w = create_tool( WKZ_ADD_MESSAGE_TOOL | SIMPLE_TOOL );
		w->set_default_param( buf );
		set_werkzeug( w, NULL );
		// since init always returns false, it is safe to delete immediately
		delete w;
#ifdef DEBUG_SIMRAND_CALLS
		if(/*last_clients == 0*/ true)
		{
			ITERATE(karte_t::random_callers, n)
			{
				get_message()->add_message(random_callers.get_element(n), koord::invalid, message_t::ai);
			}
			print_randoms = false;
		}
		else
		{
			print_randoms = true;
		}
		printf("Number of connected clients changed to %u", last_clients);
#endif
	}

	if(  get_scenario()->is_scripted() ) {
		get_scenario()->step();
	}
	DBG_DEBUG4("karte_t::step", "end");
rands[20] = get_random_seed();
}

sint32 karte_t::calc_adjusted_step_interval(const unsigned long weight, uint32 trips_per_month_hundredths) const
{
	const uint32 median_packet_size = (uint32)(get_settings().get_passenger_routing_packet_size() + 1) / 2;	
	const uint64 trips_per_month = max((((uint64)weight * calc_adjusted_monthly_figure(trips_per_month_hundredths)) / 100u) / median_packet_size, 1);
		
	return (sint32)((uint64)ticks_per_world_month > trips_per_month ? (uint64) ticks_per_world_month / trips_per_month : 1);
}

void karte_t::step_passengers_and_mail(long delta_t)
{
	if(delta_t > ticks_per_world_month) 
	{
		delta_t = 1;
	}

	next_step_passenger += delta_t;
	next_step_mail += delta_t;

	while(passenger_step_interval <= next_step_passenger) 
	{
		if(passenger_origins.get_count() == 0)
		{
			return;
		}
		generate_passengers_or_mail(warenbauer_t::passagiere);	
		INT_CHECK("simworld 5093");
	} 

	while(mail_step_interval <= next_step_mail) 
	{
		if(mail_origins_and_targets.get_count() == 0)
		{
			return;
		}
		generate_passengers_or_mail(warenbauer_t::post);
		INT_CHECK("simworld 5103");
	} 
}

sint32 karte_t::get_tiles_of_gebaeude(gebaeude_t* const gb, vector_tpl<const planquadrat_t*> &tile_list) const
{
	const haus_tile_besch_t* tile = gb->get_tile();
	const haus_besch_t *hb = tile->get_besch();
	const koord size = hb->get_groesse(tile->get_layout());
	if(size == koord(1,1))
	{
		// A single tiled building - just add the single tile.
		tile_list.append(access_nocheck(gb->get_pos().get_2d()));
	}
	else
	{
		// A multi-tiled building: check all tiles. Any tile within the 
		// coverage radius of a building connects the whole building.
		koord3d k = gb->get_pos();
		const koord start_pos = k.get_2d() - tile->get_offset();
		const koord end_pos = k.get_2d() + size;
		
		for(k.y = start_pos.y; k.y < end_pos.y; k.y ++) 
		{
			for(k.x = start_pos.x; k.x < end_pos.x; k.x ++) 
			{
				grund_t *gr = lookup(k);
				if(gr) 
				{
					/* This would fail for depots, but those are 1x1 buildings */
					gebaeude_t *gb_part = gr->find<gebaeude_t>();
					// There may be buildings with holes.
					if(gb_part && gb_part->get_tile()->get_besch() == hb) 
					{
						tile_list.append(access_nocheck(k.get_2d()));
					}
				}
			}
		}
	}
	return size.x * size.y;
}

void karte_t::get_nearby_halts_of_tiles(const vector_tpl<const planquadrat_t*> &tile_list, const ware_besch_t * wtyp, vector_tpl<nearby_halt_t> &halts) const
{
	// Suitable start search (public transport)
	FOR(vector_tpl<const planquadrat_t*>, const& current_tile, tile_list)
	{
		const nearby_halt_t* halt_list = current_tile->get_haltlist();
		for(int h = current_tile->get_haltlist_count() - 1; h >= 0; h--) 
		{
			nearby_halt_t halt = halt_list[h];
			if (halt.halt->is_enabled(wtyp)) 
			{
				// Previous versions excluded overcrowded halts here, but we need to know which
				// overcrowded halt would have been the best start halt if it was not overcrowded,
				// so do that below.
				halts.append(halt);
			}
		}
	}
}

void karte_t::generate_passengers_or_mail(const ware_besch_t * wtyp)
{
	const city_cost history_type = (wtyp == warenbauer_t::passagiere) ? HIST_PAS_TRANSPORTED : HIST_MAIL_TRANSPORTED;
	const uint32 units_this_step = simrand((uint32)settings.get_passenger_routing_packet_size(), "void karte_t::step_passengers_and_mail(long delta_t) passenger/mail packet size") + 1;

	// Pick the building from which to generate passengers/mail
	gebaeude_t* gb;
	if(wtyp == warenbauer_t::passagiere)
	{
		// Pick a passenger building at random
		const uint32 weight = simrand(passenger_origins.get_sum_weight() - 1, "void karte_t::step_passengers_and_mail(long delta_t) pick origin building (passengers)");
		gb = passenger_origins.at_weight(weight);
	}
	else
	{
		// Pick a mail building at random
		const uint32 weight = simrand(mail_origins_and_targets.get_sum_weight() - 1, "void karte_t::step_passengers_and_mail(long delta_t) pick origin building (mail)");
		gb = mail_origins_and_targets.at_weight(weight);
	}

	stadt_t* city = gb->get_stadt();

	// We need this for recording statistics for onward journeys in the very original departure point.
	gebaeude_t* const first_origin = gb;

	if(city)
	{
		// Mail is generated in non-city buildings such as attractions.
		// That will be the only legitimate case in which this condition is not fulfilled.
		city->set_generated_passengers(units_this_step, history_type + 1);
	}
	
	
	koord3d origin_pos = gb->get_pos();
	vector_tpl<const planquadrat_t*> tile_list;
	sint32 size = get_tiles_of_gebaeude(first_origin, tile_list);

	// Suitable start search (public transport)
	vector_tpl<nearby_halt_t> start_halts(tile_list[0]->get_haltlist_count() * size);
	get_nearby_halts_of_tiles(tile_list, wtyp, start_halts);

	//INT_CHECK("simworld 4490");

	// Check whether this batch of passengers has access to a private car each.
		
	const sint16 private_car_percent = wtyp == warenbauer_t::passagiere ? get_private_car_ownership(get_timeline_year_month()) : 0; 
	// Only passengers have private cars
	// QUERY: Should people be taken to be able to deliver mail packets in their own cars?
	bool has_private_car = private_car_percent > 0 ? simrand(100, "karte_t::step_passengers_and_mail() (has private car?)") <= (uint16)private_car_percent : false;
	
	// Record the most useful set of information about why passengers cannot reach their chosen destination:
	// Too slow > overcrowded > no route. Tiebreaker: higher destination preference.
	koord best_bad_destination;
	uint8 best_bad_start_halt;
	bool too_slow_already_set;
	bool overcrowded_already_set;

	const uint16 min_commuting_tolerance = settings.get_min_commuting_tolerance();
	const uint16 range_commuting_tolerance = max(0, settings.get_range_commuting_tolerance() - min_commuting_tolerance);

	const uint16 min_visiting_tolerance = settings.get_min_visiting_tolerance();
	const uint16 range_visiting_tolerance = max(0, settings.get_range_visiting_tolerance() - min_visiting_tolerance);

	const uint16 max_onward_trips = settings.get_max_onward_trips();

	trip_type trip = (wtyp == warenbauer_t::passagiere) ?
			simrand(100, "karte_t::step_passengers_and_mail() (commuting or visiting trip?)") < settings.get_commuting_trip_chance_percent() ?
			commuting_trip : visiting_trip : mail_trip;
	// Add 1 because the simuconf.tab setting is for maximum *alternative* destinations, whereas we need maximum *actual* desintations 
	// Mail does not have alternative destinations: people do not send mail to one place because they cannot reach another. Mail has specific desinations.
	const uint32 max_destinations = trip == commuting_trip ? settings.get_max_alternative_destinations_commuting() + 1 : 
									trip == visiting_trip ? settings.get_max_alternative_destinations_visiting() + 1 : 1;
	koord destination_pos;
	route_status_type route_status;
	destination current_destination;
	destination first_destination;
	first_destination.location = koord::invalid;
	uint16 time_per_tile;
	uint16 tolerance;

	// Find passenger destination

	// Mail does not make onward journeys.
	const uint16 onward_trips = simrand(100, "void stadt_t::step_passagiere() (any onward trips?)") < settings.get_onward_trip_chance_percent() &&
		wtyp == warenbauer_t::passagiere ? simrand(max_onward_trips, "void stadt_t::step_passagiere() (how many onward trips?)") + 1 : 1;

	route_status = initialising;

	for(uint32 trip_count = 0; trip_count < onward_trips && route_status != no_route && route_status != too_slow && route_status != overcrowded && route_status != destination_unavailable; trip_count ++)
	{
		// Permit onward journeys - but only for successful journeys
		const uint32 destination_count = simrand(max_destinations, "void stadt_t::step_passagiere() (number of destinations?)") + 1;

		// Split passengers between commuting trips and other trips.
		if(trip_count == 0)
		{
			// Set here because we deduct the previous journey time from the tolerance for onward trips.
			if(trip == mail_trip)
			{
				tolerance = 65535;
			}
			else if(trip == commuting_trip)
			{
				tolerance = (uint16)simrand_normal(range_commuting_tolerance, settings.get_random_mode_commuting(), "karte_t::step_passengers_and_mail (commuting tolerance?)") + min_commuting_tolerance;
			}
			else
			{
				tolerance = (uint16)simrand_normal(range_visiting_tolerance, settings.get_random_mode_visiting(), "karte_t::step_passengers_and_mail (visiting tolerance?)") + min_visiting_tolerance;
			}
		}
		else
		{
			// The trip is already set. Only re-set this for a commuting trip, as people making onward journeys
			// from a commuting trip will not be doing so as another commuting trip. 
			if(trip == commuting_trip)
			{
				trip = visiting_trip;
			}

			// Onward journey - set the initial point to the previous end point.
			const grund_t* gr = lookup_kartenboden(destination_pos);
			if(!gr)
			{
				continue;
			}
			gb = gr->find<gebaeude_t>();
					
			if(!gb)
			{
				// This sometimes happens for unknown reasons. 
				continue;
			}
			city = get_city(destination_pos);

			// Added here as the original journey had its generated passengers set much earlier, outside the for loop.
			if(city)
			{
				city->set_generated_passengers(units_this_step, history_type + 1);
			}

			if(route_status != private_car)
			{
				// If passengers did not use a private car for the first leg, they cannot use one for subsequent legs.
				has_private_car = false;
			}

			// Regenerate the start halts information for this new onward trip.
			// We cannot reuse "destination_list" as this is a list of halthandles,
			// not nearby_halt_t objects.
			// TODO BG, 15.02.2014: first build a nearby_destination_list and then a destination_list from it.
			//  Should be faster than finding all nearby halts again.

			tile_list.clear();
			get_tiles_of_gebaeude(gb, tile_list);

			// Suitable start search (public transport)
			start_halts.clear();
			get_nearby_halts_of_tiles(tile_list, wtyp, start_halts);
		}
			
		first_destination = find_destination(trip);
		current_destination = first_destination;

		if(trip == commuting_trip)
		{
			first_origin->add_passengers_generated_commuting(units_this_step);
		}
			
		else if(trip == visiting_trip)
		{
			first_origin->add_passengers_generated_visiting(units_this_step);
		}

		// Do nothing if trip == mail_trip

		//INT_CHECK("simworld 4557");

		/**
			* Quasi tolerance is necessary because mail can be delivered by hand. If it is delivered
			* by hand, the deliverer has a tolerance, but if it is sent through the postal system,
			* the mail packet itself does not have a tolerance.
			*
			* In addition, walking tolerance is divided by two because passengers prefer not to
			* walk for long distances, as it is tiring, especially with luggage.
			* (Neroden suggests that this be reconsidered)
			*/
		uint16 quasi_tolerance = tolerance;
		if(wtyp == warenbauer_t::post)
		{
			// People will walk long distances with mail: it is not heavy.
			quasi_tolerance = simrand_normal(range_visiting_tolerance, settings.get_random_mode_visiting(), "karte_t::step_passengers_and_mail (quasi tolerance)") + min_visiting_tolerance;
		}
		else
		{
			// Passengers. 
			quasi_tolerance = max(quasi_tolerance / 2, min(tolerance, 300));
		}

		uint16 car_minutes = 65535;

		best_bad_destination = first_destination.location;
		best_bad_start_halt = 0;
		too_slow_already_set = false;
		overcrowded_already_set = false;
		ware_t pax(wtyp);
		pax.is_commuting_trip = trip == commuting_trip;
		halthandle_t start_halt;
		uint16 best_journey_time;
		uint32 walking_time;
		route_status = initialising;

		for(int n = 0; n < destination_count && route_status != public_transport && route_status != private_car && route_status != on_foot; n++)
		{
			destination_pos = current_destination.location;
			if(trip == commuting_trip)
			{
				gebaeude_t* gb = current_destination.building;
				if(!gb || !gb->jobs_available())
				{
					if(route_status == initialising)
					{
						// This is the lowest priority route status.
						route_status = destination_unavailable;
					}
					/**
						* As there are no jobs, this is not a destination for commuting
						*/
					if(n < destination_count - 1)
					{
						current_destination = find_destination(trip);
					}
					continue;
				}
			}

			if(route_status == initialising)
			{
				route_status = no_route;
			}

			const uint32 straight_line_distance = shortest_distance(origin_pos.get_2d(), destination_pos);
			// Careful -- use uint32 here to avoid overflow cutoff errors.
			// This number may be very long.
			walking_time = walking_time_tenths_from_distance(straight_line_distance);
			car_minutes = 65535;

			// If can_walk is true, it also guarantees that walking_time will fit in a uint16.
			const bool can_walk = walking_time <= quasi_tolerance;

			if(!has_private_car && !can_walk && start_halts.empty())
			{
				/**
					* If the passengers have no private car, are not in reach of any public transport
					* facilities and the journey is too long on foot, do not continue to check other things.
					*/
				if(n < destination_count - 1)
				{
					current_destination = find_destination(trip);
				}
				continue;
			}

			// Check for a suitable stop within range of the destination.

			// Note that, although factories are only *connected* now if they are within the smaller factory radius
			// (default: 1), they can take passengers within the wider square of the passenger radius. This is intended,
			// and is as a result of using the below method for all destination types.

			tile_list.clear();
			sint32 size = get_tiles_of_gebaeude(current_destination.building, tile_list);

			if(tile_list.empty())
			{
				tile_list.append(access(current_destination.location));
			}

			vector_tpl<halthandle_t> destination_list(tile_list[0]->get_haltlist_count() * size);
				
			FOR(vector_tpl<const planquadrat_t*>, const& current_tile, tile_list)
			{
				const nearby_halt_t* halt_list = current_tile->get_haltlist();
				for(int h = current_tile->get_haltlist_count() - 1; h >= 0; h--) 
				{
					halthandle_t halt = halt_list[h].halt;
					if(halt->is_enabled(wtyp)) 
					{
						// Previous versions excluded overcrowded halts here, but we need to know which
						// overcrowded halt would have been the best start halt if it was not overcrowded,
						// so do that below.
						destination_list.append(halt);
					}
				}
			}

			best_journey_time = 65535;
			if(start_halts.get_count() == 1 && destination_list.get_count() == 1 && start_halts[0].halt == destination_list.get_element(0))
			{
				/** There is no public transport route, as the only stop
				* for the origin is also the only stop for the destintation.
				*/
				start_halt = start_halts[0].halt;
			}
			else
			{
				// Check whether public transport can be used.
				// Journey start information needs to be added later.
				pax.reset();
				pax.set_zielpos(destination_pos);
				pax.menge = units_this_step;
				//"Menge" = volume (Google)

				// Search for a route using public transport. 

				uint32 best_start_halt = 0;
				uint32 best_non_crowded_start_halt = 0;
				uint32 best_journey_time_including_crowded_halts = 65535;

				ITERATE(start_halts, i)
				{
					halthandle_t current_halt = start_halts[i].halt;
				
					uint32 current_journey_time = current_halt->find_route(destination_list, pax, best_journey_time, destination_pos);
					
					// Add walking time from the origin to the origin stop. 
					// Note that the walking time to the destination stop is already added by find_route.
					current_journey_time += walking_time_tenths_from_distance(start_halts[i].distance);
					if(current_journey_time > 65535)
					{
						current_journey_time = 65535;
					}
					// TODO: Add facility to check whether station/stop has car parking facilities, and add the possibility of a (faster) private car journey.
					// Use the private car journey time per tile from the passengers' origin to the city in which the stop is located.

					if(current_journey_time < best_journey_time)
					{
						if(!current_halt->is_overcrowded(wtyp->get_catg_index()))
						{
							best_journey_time = current_journey_time;
							best_non_crowded_start_halt = i;
							if(pax.get_ziel().is_bound())
							{
								route_status = public_transport;
							}
						}
						best_journey_time_including_crowded_halts = current_journey_time;
						best_start_halt = i;
					}
				}

				if(best_journey_time == 0)
				{
					best_journey_time = 1;
				}

				if(can_walk && walking_time < best_journey_time)
				{
					// If walking is faster than public transport, passengers will walk.
					const grund_t* destination_gr = lookup_kartenboden(current_destination.location);
					if(destination_gr && !destination_gr->ist_wasser())
					{
						// People cannot walk on water. This is relevant for fisheries and oil rigs in particular.
						route_status = on_foot;
					}
				}

				// Check first whether the best route is outside
				// the passengers' tolerance.

				if(best_journey_time_including_crowded_halts < tolerance && route_status != public_transport && walking_time > best_journey_time)
				{ 
					route_status = overcrowded;
					if(!overcrowded_already_set)
					{
						best_bad_destination = destination_pos;
						best_bad_start_halt = best_start_halt;
						overcrowded_already_set = true;
					}
				}
				else if((route_status == public_transport || route_status == no_route) && best_journey_time_including_crowded_halts >= tolerance)
				{
					route_status = too_slow;
				
					if(!too_slow_already_set && !overcrowded_already_set)
					{
						best_bad_destination = destination_pos;
						best_bad_start_halt = best_start_halt;
						too_slow_already_set = true;
					}
				}
				else
				{
					// All passengers will use the quickest route.
					if(start_halts.get_count() > 0)
					{
						start_halt = start_halts[best_start_halt].halt;
					}
				}
			}

			//INT_CHECK("simworld.cc 4774");
			
			if(has_private_car) 
			{
				// time_per_tile here is in 100ths of minutes per tile.
				// 1/100th of a minute per tile = km/h * 6.
				time_per_tile = 65535;
				switch(current_destination.type)
				{
				case town:
					//Town
					if(city)
					{
						time_per_tile = city->check_road_connexion_to(current_destination.building->get_stadt());
					}
					else
					{
						// Going onward from an out of town attraction or industry to a city building - get route backwards.
						if(current_destination.type == attraction)
						{
							time_per_tile = current_destination.building->get_stadt()->check_road_connexion_to(current_destination.building);
						}
						else if(current_destination.type == factory)		
						{
							time_per_tile = current_destination.building->get_stadt()->check_road_connexion_to(current_destination.building->get_fabrik());
						}						
					}
					break;
				case factory:
					if(city) // Previous time per tile value used as default if the city is not available.
					{
						time_per_tile = city->check_road_connexion_to(current_destination.building->get_fabrik());
					}
					break;
				case attraction:
					if(city) // Previous time per tile value used as default if the city is not available.
					{
						time_per_tile = city->check_road_connexion_to(current_destination.building);
					}							
					break;
				default:
					//Some error - this should not be reached.
					dbg->error("simworld.cc", "Incorrect destination type detected");
				};

				if(time_per_tile < 65535)
				{
					// *Hundredths* of minutes used here for per tile times for accuracy.
					// Convert to tenths, but only after multiplying to preserve accuracy.
					// Use a uint32 intermediary to avoid overflow.
					const uint32 car_mins = (time_per_tile * straight_line_distance) / 10;
					car_minutes = car_mins + 30; // Add three minutes to represent 1:30m parking time at each end.

					// Now, adjust the timings for congestion (this is already taken into account if the route was
					// calculated using the route finder; note that journeys inside cities are not calculated using
					// the route finder). 

					if(settings.get_assume_everywhere_connected_by_road() || (current_destination.type == town && current_destination.building->get_stadt() == city))
					{
						// Congestion here is assumed to be on the percentage basis: i.e. the percentage of extra time that
						// a journey takes owing to congestion. This is the measure used by the TomTom congestion index,
						// compiled by the satellite navigation company of that name, which provides useful research data.
						// See: http://www.tomtom.com/lib/doc/trafficindex/2013-0129-TomTom%20Congestion-Index-2012Q3europe-km.pdf
							
						//Average congestion of origin and destination towns.
						uint16 congestion_total;
						if(current_destination.building->get_stadt() != NULL && current_destination.building->get_stadt() != city)
						{
							// Destination type is town and the destination town object can be found.
							congestion_total = (city->get_congestion() + current_destination.building->get_stadt()->get_congestion()) / 2;
						}
						else
						{
							congestion_total = city->get_congestion();
						}
					
						const uint32 congestion_extra_minutes = (car_minutes * congestion_total) / 100;

						car_minutes += congestion_extra_minutes;
					}
				}
			}

			// Cannot be <=, as mail has a tolerance of 65535, which is used as the car_minutes when
			// a private car journey is not possible.
			if(car_minutes < tolerance)
			{
				const uint16 private_car_chance = (uint16)simrand(100, "void stadt_t::step_passagiere() (private car chance?)");

				if(route_status != public_transport)
				{
					// The passengers can get to their destination by car but not by public transport.
					// Therefore, they will always use their car unless it is faster to walk and they 
					// are not people who always prefer to use the car.
					if(car_minutes > walking_time && can_walk && private_car_chance > settings.get_always_prefer_car_percent())
					{
						// If walking is faster than taking the car, passengers will walk.
						route_status = on_foot;
					}
					else
					{
						route_status = private_car;
					}
				}
				else if(private_car_chance <= settings.get_always_prefer_car_percent() || car_minutes <= best_journey_time)
				{
					route_status = private_car;
				}
			}
			else if(car_minutes != 65535)
			{
				route_status = too_slow;

				if(!too_slow_already_set && !overcrowded_already_set)
				{
					best_bad_destination = destination_pos;
 					// too_slow_already_set = true;
					// Do not set too_slow_already_set here, as will
					// prevent the passengers showing up in a "too slow" 
					// graph on a subsequent station/stop.
				}
			}
				
			//INT_CHECK("simworld 4897");
			if((route_status == no_route || route_status == too_slow || route_status == overcrowded || route_status == destination_unavailable) && n < destination_count - 1)
			{
				// Do not get a new destination if there is a good status,
				// or if this is the last destination to be assigned,
				// or else entirely the wrong information will be recorded
				// below!
				current_destination = find_destination(trip);
			}

		} // For loop (route_status)

		bool set_return_trip = false;
		stadt_t* destination_town;

		switch(route_status)
		{
		case public_transport:

			if(tolerance < 65535)
			{
				tolerance -= best_journey_time;
				quasi_tolerance -= best_journey_time;
			}
			pax.arrival_time = get_zeit_ms();
			pax.set_origin(start_halt);
			start_halt->starte_mit_route(pax);
			if(city && wtyp == warenbauer_t::passagiere)
			{
				city->merke_passagier_ziel(destination_pos, COL_YELLOW);
			}
			set_return_trip = true;
			// create pedestrians in the near area?
			if(settings.get_random_pedestrians() && wtyp == warenbauer_t::passagiere) 
			{
				haltestelle_t::erzeuge_fussgaenger(origin_pos, units_this_step);
			}
			// We cannot do this on arrival, as the ware packets do not remember their origin building.
			// However, as for the destination, this can be set when the passengers arrive.
					
			if(trip == commuting_trip && first_origin)
			{
				first_origin->add_passengers_succeeded_commuting(units_this_step);
			}
			else if(trip == visiting_trip && first_origin)
			{
				first_origin->add_passengers_succeeded_visiting(units_this_step);
			}
			// Do nothing if trip == mail: mail statistics are added on arrival.
			break;

		case private_car:
					
			if(tolerance < 65535)
			{
				tolerance -= car_minutes;
				quasi_tolerance -= car_minutes;
			}
					
			destination_town = current_destination.type == town ? current_destination.building->get_stadt() : NULL;
			if(city)
			{
#ifdef DESTINATION_CITYCARS
				city->erzeuge_verkehrsteilnehmer(origin_pos.get_2d(), car_minutes, destination_pos, units_this_step);
#endif
				if(wtyp == warenbauer_t::passagiere)
				{
					city->set_private_car_trip(units_this_step, destination_town);
					city->merke_passagier_ziel(destination_pos, COL_TURQUOISE);
				}
				else
				{
					// Mail
					city->add_transported_mail(units_this_step);
				}
			}

			set_return_trip = true;
			// We cannot do this on arrival, as the ware packets do not remember their origin building.
			if(trip == commuting_trip)
			{
				first_origin->add_passengers_succeeded_commuting(units_this_step);
				if(current_destination.type == factory)
				{
					// Only add commuting passengers at a factory.
					current_destination.building->get_fabrik()->liefere_an(wtyp, units_this_step);
				}
				else if(current_destination.building->get_tile()->get_besch()->get_typ() != gebaeude_t::wohnung)
				{
					// Houses do not record received passengers.
					current_destination.building->set_commute_trip(units_this_step);
				}
			}
			else if(trip == visiting_trip)
			{
				first_origin->add_passengers_succeeded_visiting(units_this_step);
				if(current_destination.building->get_tile()->get_besch()->get_typ() != gebaeude_t::wohnung)
				{
					// Houses do not record received passengers.
					current_destination.building->add_passengers_succeeded_visiting(units_this_step);
				}
			}
			break;

		case on_foot:
					
			if(tolerance < 65535)
			{
				tolerance -= walking_time;
				quasi_tolerance -= walking_time;
			}	

			// Walking passengers are not marked as "happy", as the player has not made them happy.

			if(settings.get_random_pedestrians() && wtyp == warenbauer_t::passagiere) 
			{
				haltestelle_t::erzeuge_fussgaenger(origin_pos, units_this_step);
			}
				
			if(city)
			{
				if(wtyp == warenbauer_t::passagiere)
				{
					city->merke_passagier_ziel(destination_pos, COL_DARK_YELLOW);
					city->add_walking_passengers(units_this_step);
				}
				else
				{
					// Mail
					city->add_transported_mail(units_this_step);
				}
			}
			set_return_trip = true;

			// We cannot do this on arrival, as the ware packets do not remember their origin building.
			if(trip == commuting_trip)
			{
				first_origin->add_passengers_succeeded_commuting(units_this_step);
				if(current_destination.type == factory)
				{
					// Only add commuting passengers at a factory.
					current_destination.building->get_fabrik()->liefere_an(wtyp, units_this_step);
				}
				else if(current_destination.building->get_tile()->get_besch()->get_typ() != gebaeude_t::wohnung)
				{
					// Houses do not record received passengers.
					current_destination.building->set_commute_trip(units_this_step);
				}
			}
			else if(trip == visiting_trip)
			{
				first_origin->add_passengers_succeeded_visiting(units_this_step);
				if(current_destination.building->get_tile()->get_besch()->get_typ() != gebaeude_t::wohnung)
				{
					// Houses do not record received passengers.
					current_destination.building->add_passengers_succeeded_visiting(units_this_step);
				}
			}
			// Do nothing if trip == mail.
			break;

		case overcrowded:

			if(city && wtyp == warenbauer_t::passagiere)
			{
				city->merke_passagier_ziel(best_bad_destination, COL_RED);
			}					
					
			if(start_halts.get_count() > 0)
			{
				start_halt = start_halts[best_bad_start_halt].halt; 					
				if(start_halt.is_bound())
				{
					start_halt->add_pax_unhappy(units_this_step);
				}
			}

			break;

		case too_slow:
		
			if(city && wtyp == warenbauer_t::passagiere)
			{
				if(car_minutes >= best_journey_time)
				{
					city->merke_passagier_ziel(best_bad_destination, COL_LIGHT_PURPLE);
				}
				else if(car_minutes < 65535)
				{
					city->merke_passagier_ziel(best_bad_destination, COL_PURPLE);
				}
				else
				{
					// This should not occur but occasionally does.
					goto no_route;
				}
			}

			if(too_slow_already_set && !start_halts.empty())
			{
				// This will be dud for a private car trip.
				start_halt = start_halts[best_bad_start_halt].halt; 					
			}
			if(start_halt.is_bound())
			{
				start_halt->add_pax_too_slow(units_this_step);
			}

			break;

		case no_route:
		case destination_unavailable:
no_route:
			if(city && wtyp == warenbauer_t::passagiere)
			{
				if(route_status == destination_unavailable)
				{
					city->merke_passagier_ziel(first_destination.location, COL_DARK_RED);
				}
				else
				{
					city->merke_passagier_ziel(first_destination.location, COL_DARK_ORANGE);
				}
			}
					
			if(route_status != destination_unavailable && start_halts.get_count() > 0)
			{
				start_halt = start_halts[best_bad_start_halt].halt; 					
				if(start_halt.is_bound())
				{
					start_halt->add_pax_no_route(units_this_step);
				}
			}
		};

		if(set_return_trip)
		{
			// Calculate a return journey
			// This comes most of the time for free and also balances the flows of passengers to and from any given place.

			// NOTE: This currently does not re-do the whole start/end stop search done on the way out. This saves time, but might
			// cause anomalies with substantially asymmetric routes. Reconsider this some time.

			// Because passengers/mail now register as transported on delivery, these are needed 
			// here to keep an accurate record of the proportion transported.
			stadt_t* const destination_town = get_city(current_destination.location);
			if(destination_town)
			{
				destination_town->set_generated_passengers(units_this_step, history_type + 1);
			}
			else if(city)
			{
				city->set_generated_passengers(units_this_step, history_type + 1);
				// Cannot add success figures for buildings here as cannot get a building from a koord. 
				// However, this should not matter much, as equally not recording generated passengers
				// for all return journeys should still show accurate percentages overall. 
			}
			if(current_destination.type == factory && (trip == commuting_trip || trip == mail_trip))
			{
				// The only passengers generated by a factory are returning passengers who have already reached the factory somehow or another
				// from home (etc.).
				current_destination.building->get_fabrik()->book_stat(units_this_step, (wtyp == warenbauer_t::passagiere ? FAB_PAX_GENERATED : FAB_MAIL_GENERATED));
			}
		
			halthandle_t ret_halt = pax.get_ziel();
			// Those who have driven out have to take thier cars back regardless of whether public transport is better - do not check again.
			bool return_in_private_car = route_status == private_car;
			bool return_on_foot = route_status == on_foot;

			if(!return_in_private_car && !return_on_foot)
			{
				if(!ret_halt.is_bound())
				{
					if(walking_time <= tolerance)
					{
						return_on_foot = true;
						goto return_on_foot;
					}
					else
					{
						// If the passengers cannot return, do not record them as having returned.
						continue;
					}
				}
				
				bool found = false;
				ITERATE(start_halts, i)
				{
					halthandle_t test_halt = start_halts[i].halt;
				
					if(test_halt->is_enabled(wtyp) && (start_halt == test_halt || test_halt->get_connexions(wtyp->get_catg_index())->access(start_halt) != NULL))
					{
						found = true;
						start_halt = test_halt;
						break;
					}
				}

				// Now try to add them to the target halt
				ware_t test_passengers;
				test_passengers.set_ziel(start_halts[best_bad_start_halt].halt);
				const bool overcrowded_route = ret_halt->find_route(test_passengers) < 65535;
				if(!ret_halt->is_overcrowded(wtyp->get_catg_index()) || !overcrowded_route)
				{
					// prissi: not overcrowded and can recieve => add them
					// Only mark the passengers as unable to get to their destination
					// due to overcrowding if they could get to their destination
					// if the stop was not overcroweded.
					if(found) 
					{
						ware_t return_pax(wtyp, ret_halt);
						return_pax.menge = units_this_step;

						return_pax.set_zielpos(origin_pos.get_2d());
						return_pax.set_ziel(start_halt);
						return_pax.is_commuting_trip = trip == commuting_trip;
						if(ret_halt->find_route(return_pax) != 65535)
						{
							return_pax.arrival_time = get_zeit_ms();
							ret_halt->starte_mit_route(return_pax);
						}
						if(current_destination.type == factory && (trip == commuting_trip || trip == mail_trip))
						{
							// This is somewhat anomalous, as we are recording that the passengers have departed, not arrived, whereas for cities, we record
							// that they have successfully arrived. However, this is not easy to implement for factories, as passengers do not store their ultimate
							// origin, so the origin factory is not known by the time that the passengers reach the end of their journey.
							current_destination.building->get_fabrik()->book_stat(units_this_step, (wtyp == warenbauer_t::passagiere ? FAB_PAX_DEPARTED : FAB_MAIL_DEPARTED));
						}
					}
					else 
					{
						// no route back
						if(walking_time <= tolerance)
						{
							return_on_foot = true;
						}
						else
						{	
							ret_halt->add_pax_no_route(units_this_step);
						}
					}
				}
				else
				{
					// Return halt crowded. Either return on foot or mark unhappy.
					if(walking_time <= tolerance)
					{
						return_on_foot = true;
					}
					else if(overcrowded_route)
					{
						ret_halt->add_pax_unhappy(units_this_step);
					}
				}
			}
					
			if(return_in_private_car)
			{
				if(car_minutes < 65535)
				{
					// Do not check tolerance, as they must come back!
					if(wtyp == warenbauer_t::passagiere)
					{
						if(destination_town)
						{
							destination_town->set_private_car_trip(units_this_step, city);
						}
						else
						{
							// Industry, attraction or local
							city->set_private_car_trip(units_this_step, NULL);
						}
					}
					else
					{
						// Mail
						if(destination_town)
						{
							destination_town->add_transported_mail(units_this_step);
						}
						else if(city)
						{
							city->add_transported_mail(units_this_step);
						}
					}

#ifdef DESTINATION_CITYCARS
					//citycars with destination
					city->erzeuge_verkehrsteilnehmer(current_destination.location, car_minutes, origin_pos.get_2d(), units_this_step);
#endif

					if(current_destination.type == factory && (trip == commuting_trip || trip == mail_trip))
					{
						current_destination.building->get_fabrik()->book_stat(units_this_step, (wtyp == warenbauer_t::passagiere ? FAB_PAX_DEPARTED : FAB_MAIL_DEPARTED));
					}
				}
				else
				{
					if(ret_halt.is_bound())
					{
						ret_halt->add_pax_no_route(units_this_step);
					}
					if(city)
					{
						city->merke_passagier_ziel(origin_pos.get_2d(), COL_DARK_ORANGE);
					}
				}
			}
return_on_foot:
			if(return_on_foot)
			{
				if(wtyp == warenbauer_t::passagiere)
				{
					if(destination_town)
					{
						destination_town->add_walking_passengers(units_this_step);
					}
					else if(city)
					{
						// Local, attraction or industry.
						city->merke_passagier_ziel(origin_pos.get_2d(), COL_DARK_YELLOW);
						city->add_walking_passengers(units_this_step);
					}
				}
				else
				{
					// Mail
					if(destination_town)
					{
						destination_town->add_transported_mail(units_this_step);
					}
					else if(city)
					{
						city->add_transported_mail(units_this_step);
					}
				}
				if(current_destination.type == factory && (trip == commuting_trip || trip == mail_trip))
				{
					current_destination.building->get_fabrik()->book_stat(units_this_step, (wtyp==warenbauer_t::passagiere ? FAB_PAX_DEPARTED : FAB_MAIL_DEPARTED));
				}
			}
		} // Set return trip
	} // Onward journeys (for loop)

	if(wtyp == warenbauer_t::passagiere)
	{
		next_step_passenger -= (passenger_step_interval * units_this_step);
	}
	else
	{
		next_step_mail -= (mail_step_interval * units_this_step);
	}
}

karte_t::destination karte_t::find_destination(trip_type trip)
{
	destination current_destination;
	current_destination.type = karte_t::invalid;
	gebaeude_t* gb;

	switch(trip)
	{
	
	case commuting_trip: 
		gb = pick_any_weighted(commuter_targets);
		break;

	case visiting_trip:
		gb = pick_any_weighted(visitor_targets);
		break;

	default:
	case mail_trip:
		gb = pick_any_weighted(mail_origins_and_targets);
	};

	if(!gb)
	{
		// Might happen if the relevant collection object is empty.		
		current_destination.location = koord::invalid;
		return current_destination;
	}

	current_destination.location = gb->get_pos().get_2d();
	current_destination.building = gb;

	// Add the correct object type.
	fabrik_t* const fab = gb->get_fabrik();
	stadt_t* const city = gb->get_stadt();
	if(fab)
	{
		current_destination.type = karte_t::factory;
	}
	else if(city)
	{
		current_destination.type = karte_t::town;
	}
	else // Attraction (out of town)
	{
		current_destination.type = karte_t::attraction;
	}

	return current_destination;
}


// recalculates world statistics for older versions
void karte_t::restore_history()
{
	last_month_bev = -1;
	for(  int m=12-1;  m>0;  m--  ) {
		// now step all towns
		sint64 bev=0;
		sint64 total_pas = 1, trans_pas = 0;
		sint64 total_mail = 1, trans_mail = 0;
		sint64 total_goods = 1, supplied_goods = 0;
		FOR(weighted_vector_tpl<stadt_t*>, const i, stadt) {
			bev            += i->get_finance_history_month(m, HIST_CITICENS);
			trans_pas      += i->get_finance_history_month(m, HIST_PAS_TRANSPORTED);
			trans_pas      += i->get_finance_history_month(m, HIST_PAS_WALKED);
			trans_pas      += i->get_finance_history_month(m, HIST_CITYCARS);
			total_pas      += i->get_finance_history_month(m, HIST_PAS_GENERATED);
			trans_mail     += i->get_finance_history_month(m, HIST_MAIL_TRANSPORTED);
			total_mail     += i->get_finance_history_month(m, HIST_MAIL_GENERATED);
			supplied_goods += i->get_finance_history_month(m, HIST_GOODS_RECIEVED);
			total_goods    += i->get_finance_history_month(m, HIST_GOODS_NEEDED);
		}

		// the inhabitants stuff
		if(last_month_bev == -1) {
			last_month_bev = bev;
		}
		finance_history_month[m][WORLD_GROWTH] = bev-last_month_bev;
		finance_history_month[m][WORLD_CITICENS] = bev;
		last_month_bev = bev;

		// transportation ratio and total number
		finance_history_month[m][WORLD_PAS_RATIO] = (10000*trans_pas)/total_pas;
		finance_history_month[m][WORLD_PAS_GENERATED] = total_pas-1;
		finance_history_month[m][WORLD_MAIL_RATIO] = (10000*trans_mail)/total_mail;
		finance_history_month[m][WORLD_MAIL_GENERATED] = total_mail-1;
		finance_history_month[m][WORLD_GOODS_RATIO] = (10000*supplied_goods)/total_goods;
	}

	// update total transported, including passenger and mail
	for(  int m=min(MAX_WORLD_HISTORY_MONTHS,MAX_PLAYER_HISTORY_MONTHS)-1;  m>0;  m--  ) {
		sint64 transported = 0;
		for(  uint i=0;  i<MAX_PLAYER_COUNT;  i++ ) {
			if(  spieler[i]!=NULL  ) {
				transported += spieler[i]->get_finance()->get_history_veh_month( TT_ALL, m, ATV_TRANSPORTED );
			}
		}
		finance_history_month[m][WORLD_TRANSPORTED_GOODS] = transported;
	}

	sint64 bev_last_year = -1;
	for(  int y=min(MAX_WORLD_HISTORY_YEARS,MAX_CITY_HISTORY_YEARS)-1;  y>0;  y--  ) {
		// now step all towns (to generate passengers)
		sint64 bev=0;
		sint64 total_pas_year = 1, trans_pas_year = 0;
		sint64 total_mail_year = 1, trans_mail_year = 0;
		sint64 total_goods_year = 1, supplied_goods_year = 0;
		FOR(weighted_vector_tpl<stadt_t*>, const i, stadt) {
			bev                 += i->get_finance_history_year(y, HIST_CITICENS);
			trans_pas_year      += i->get_finance_history_year(y, HIST_PAS_TRANSPORTED);
			trans_pas_year      += i->get_finance_history_year(y, HIST_PAS_WALKED);
			trans_pas_year      += i->get_finance_history_year(y, HIST_CITYCARS);
			total_pas_year      += i->get_finance_history_year(y, HIST_PAS_GENERATED);
			trans_mail_year     += i->get_finance_history_year(y, HIST_MAIL_TRANSPORTED);
			total_mail_year     += i->get_finance_history_year(y, HIST_MAIL_GENERATED);
			supplied_goods_year += i->get_finance_history_year(y, HIST_GOODS_RECIEVED);
			total_goods_year    += i->get_finance_history_year(y, HIST_GOODS_NEEDED);
		}

		// the inhabitants stuff
		if(bev_last_year == -1) {
			bev_last_year = bev;
		}
		finance_history_year[y][WORLD_GROWTH] = bev-bev_last_year;
		finance_history_year[y][WORLD_CITICENS] = bev;
		bev_last_year = bev;

		// transportation ratio and total number
		finance_history_year[y][WORLD_PAS_RATIO] = (10000*trans_pas_year)/total_pas_year;
		finance_history_year[y][WORLD_PAS_GENERATED] = total_pas_year-1;
		finance_history_year[y][WORLD_MAIL_RATIO] = (10000*trans_mail_year)/total_mail_year;
		finance_history_year[y][WORLD_MAIL_GENERATED] = total_mail_year-1;
		finance_history_year[y][WORLD_GOODS_RATIO] = (10000*supplied_goods_year)/total_goods_year;
	}

	for(  int y=min(MAX_WORLD_HISTORY_YEARS,MAX_CITY_HISTORY_YEARS)-1;  y>0;  y--  ) {
		sint64 transported_year = 0;
		for(  uint i=0;  i<MAX_PLAYER_COUNT;  i++ ) {
			if(  spieler[i]  ) {
				transported_year += spieler[i]->get_finance()->get_history_veh_year( TT_ALL, y, ATV_TRANSPORTED );
			}
		}
		finance_history_year[y][WORLD_TRANSPORTED_GOODS] = transported_year;
	}
	// fix current month/year
	update_history();
}


void karte_t::update_history()
{
	finance_history_year[0][WORLD_CONVOIS] = finance_history_month[0][WORLD_CONVOIS] = convoi_array.get_count();
	finance_history_year[0][WORLD_FACTORIES] = finance_history_month[0][WORLD_FACTORIES] = fab_list.get_count();

	// now step all towns (to generate passengers)
	sint64 bev = 0;
	sint64 jobs = 0;
	sint64 visitor_demand = 0;
	sint64 total_pas = 1, trans_pas = 0;
	sint64 total_mail = 1, trans_mail = 0;
	sint64 total_goods = 1, supplied_goods = 0;
	sint64 total_pas_year = 1, trans_pas_year = 0;
	sint64 total_mail_year = 1, trans_mail_year = 0;
	sint64 total_goods_year = 1, supplied_goods_year = 0;
	FOR(weighted_vector_tpl<stadt_t*>, const i, stadt) {
		bev							+= i->get_finance_history_month(0, HIST_CITICENS);
		jobs						+= i->get_finance_history_month(0, HIST_JOBS);
		visitor_demand				+= i->get_finance_history_month(0, HIST_VISITOR_DEMAND);
		trans_pas					+= i->get_finance_history_month(0, HIST_PAS_TRANSPORTED);
		trans_pas					+= i->get_finance_history_month(0, HIST_PAS_WALKED);
		trans_pas					+= i->get_finance_history_month(0, HIST_CITYCARS);
		total_pas					+= i->get_finance_history_month(0, HIST_PAS_GENERATED);
		trans_mail					+= i->get_finance_history_month(0, HIST_MAIL_TRANSPORTED);
		total_mail					+= i->get_finance_history_month(0, HIST_MAIL_GENERATED);
		supplied_goods				+= i->get_finance_history_month(0, HIST_GOODS_RECIEVED);
		total_goods					+= i->get_finance_history_month(0, HIST_GOODS_NEEDED);
		trans_pas_year				+= i->get_finance_history_year( 0, HIST_PAS_TRANSPORTED);
		trans_pas_year				+= i->get_finance_history_year( 0, HIST_PAS_WALKED);
		trans_pas_year				+= i->get_finance_history_year( 0, HIST_CITYCARS);
		total_pas_year				+= i->get_finance_history_year( 0, HIST_PAS_GENERATED);
		trans_mail_year				+= i->get_finance_history_year( 0, HIST_MAIL_TRANSPORTED);
		total_mail_year				+= i->get_finance_history_year( 0, HIST_MAIL_GENERATED);
		supplied_goods_year			+= i->get_finance_history_year( 0, HIST_GOODS_RECIEVED);
		total_goods_year			+= i->get_finance_history_year( 0, HIST_GOODS_NEEDED);
	}

	finance_history_month[0][WORLD_GROWTH] = bev - last_month_bev;
	finance_history_year[0][WORLD_GROWTH] = bev - (finance_history_year[1][WORLD_CITICENS]==0 ? finance_history_month[0][WORLD_CITICENS] : finance_history_year[1][WORLD_CITICENS]);

	// the inhabitants stuff
	finance_history_year[0][WORLD_TOWNS] = finance_history_month[0][WORLD_TOWNS] = stadt.get_count();
	finance_history_year[0][WORLD_CITICENS] = finance_history_month[0][WORLD_CITICENS] = bev;
	finance_history_year[0][WORLD_JOBS] = finance_history_month[0][WORLD_JOBS] = jobs;
	finance_history_year[0][WORLD_VISITOR_DEMAND] = finance_history_month[0][WORLD_VISITOR_DEMAND] = visitor_demand;
	finance_history_month[0][WORLD_GROWTH] = bev - last_month_bev;
	finance_history_year[0][WORLD_GROWTH] = bev - (finance_history_year[1][WORLD_CITICENS] == 0 ? finance_history_month[0][WORLD_CITICENS] : finance_history_year[1][WORLD_CITICENS]);

	// transportation ratio and total number
	finance_history_month[0][WORLD_PAS_RATIO] = (10000*trans_pas)/total_pas;
	finance_history_month[0][WORLD_PAS_GENERATED] = total_pas-1;
	finance_history_month[0][WORLD_MAIL_RATIO] = (10000*trans_mail)/total_mail;
	finance_history_month[0][WORLD_MAIL_GENERATED] = total_mail-1;
	finance_history_month[0][WORLD_GOODS_RATIO] = (10000*supplied_goods)/total_goods;

	finance_history_year[0][WORLD_PAS_RATIO] = (10000*trans_pas_year)/total_pas_year;
	finance_history_year[0][WORLD_PAS_GENERATED] = total_pas_year-1;
	finance_history_year[0][WORLD_MAIL_RATIO] = (10000*trans_mail_year)/total_mail_year;
	finance_history_year[0][WORLD_MAIL_GENERATED] = total_mail_year-1;
	finance_history_year[0][WORLD_GOODS_RATIO] = (10000*supplied_goods_year)/total_goods_year;

	// update total transported, including passenger and mail
	sint64 transported = 0;
	sint64 transported_year = 0;
	for(  uint i=0;  i<MAX_PLAYER_COUNT;  i++ ) {
		if(  spieler[i]!=NULL  ) {
			transported += spieler[i]->get_finance()->get_history_veh_month( TT_ALL, 0, ATV_TRANSPORTED_GOOD );
			transported_year += spieler[i]->get_finance()->get_history_veh_year( TT_ALL, 0, ATV_TRANSPORTED_GOOD );
		}
	}
	finance_history_month[0][WORLD_TRANSPORTED_GOODS] = transported;
	finance_history_year[0][WORLD_TRANSPORTED_GOODS] = transported_year;

	finance_history_month[0][WORLD_CAR_OWNERSHIP] = get_private_car_ownership(get_timeline_year_month());

	// Average the annual figure
	sint64 car_ownership_sum = 0;
	for(uint8 months = 0; months < MAX_WORLD_HISTORY_MONTHS; months ++)
	{
		car_ownership_sum += finance_history_month[months][WORLD_CAR_OWNERSHIP];
	}
	finance_history_year[0][WORLD_CAR_OWNERSHIP] = car_ownership_sum / MAX_WORLD_HISTORY_MONTHS;
}


static sint8 median( sint8 a, sint8 b, sint8 c )
{
#if 0
	if(  a==b  ||  a==c  ) {
		return a;
	}
	else if(  b==c  ) {
		return b;
	}
	else {
		// noting matches
//		return (3*128+1 + a+b+c)/3-128;
		return -128;
	}
#elif 0
	if(  a<=b  ) {
		return b<=c ? b : max(a,c);
	}
	else {
		return b>c ? b : min(a,c);
	}
#else
		return (6*128+3 + a+a+b+b+c+c)/6-128;
#endif
}


uint8 karte_t::recalc_natural_slope( const koord k, sint8 &new_height ) const
{
	grund_t *gr = lookup_kartenboden(k);
	if(!gr) {
		return hang_t::flach;
	}
	else {
		const sint8 max_hdiff = grund_besch_t::double_grounds ? 2 : 1;

		sint8 corner_height[4];

		// get neighbour corner heights
		sint8 neighbour_height[8][4];
		get_neighbour_heights( k, neighbour_height );

		//check whether neighbours are foundations
		bool neighbour_fundament[8];
		for(  int i = 0;  i < 8;  i++  ) {
			grund_t *gr2 = lookup_kartenboden( k + koord::neighbours[i] );
			neighbour_fundament[i] = (gr2  &&  gr2->get_typ() == grund_t::fundament);
		}

		for(  uint8 i = 0;  i < 4;  i++  ) { // 0 = sw, 1 = se etc.
			// corner1 (i=0): tests vs neighbour 1:w (corner 2 j=1),2:sw (corner 3) and 3:s (corner 4)
			// corner2 (i=1): tests vs neighbour 3:s (corner 3 j=2),4:se (corner 4) and 5:e (corner 1)
			// corner3 (i=2): tests vs neighbour 5:e (corner 4 j=3),6:ne (corner 1) and 7:n (corner 2)
			// corner4 (i=3): tests vs neighbour 7:n (corner 1 j=0),0:nw (corner 2) and 1:w (corner 3)

			sint16 median_height = 0;
			uint8 natural_corners = 0;
			for(  int j = 1;  j < 4;  j++  ) {
				if(  !neighbour_fundament[(i * 2 + j) & 7]  ) {
					natural_corners++;
					median_height += neighbour_height[(i * 2 + j) & 7][(i + j) & 3];
				}
			}
			switch(  natural_corners  ) {
				case 1: {
					corner_height[i] = (sint8)median_height;
					break;
				}
				case 2: {
					corner_height[i] = median_height >> 1;
					break;
				}
				default: {
					// take the average of all 3 corners (if no natural corners just use the artificial ones anyway)
					corner_height[i] = median( neighbour_height[(i * 2 + 1) & 7][(i + 1) & 3], neighbour_height[(i * 2 + 2) & 7][(i + 2) & 3], neighbour_height[(i * 2 + 3) & 7][(i + 3) & 3] );
					break;
				}
			}
		}

		// new height of that tile ...
		sint8 min_height = min( min( corner_height[0], corner_height[1] ), min( corner_height[2], corner_height[3] ) );
		sint8 max_height = max( max( corner_height[0], corner_height[1] ), max( corner_height[2], corner_height[3] ) );
		/* check for an artificial slope on a steep sidewall */
		bool not_ok = abs( max_height - min_height ) > max_hdiff  ||  min_height == -128;

		sint8 old_height = gr->get_hoehe();
		new_height = min_height;

		// now we must make clear, that there is no ground above/below the slope
		if(  old_height!=new_height  ) {
			not_ok |= lookup(koord3d(k,new_height))!=NULL;
			if(  old_height > new_height  ) {
				not_ok |= lookup(koord3d(k,old_height-1))!=NULL;
			}
			if(  old_height < new_height  ) {
				not_ok |= lookup(koord3d(k,old_height+1))!=NULL;
			}
		}

		if(  not_ok  ) {
			/* difference too high or ground above/below
			 * we just keep it as it was ...
			 */
			new_height = old_height;
			return gr->get_grund_hang();
		}

		const sint16 d1 = min( corner_height[0] - new_height, max_hdiff );
		const sint16 d2 = min( corner_height[1] - new_height, max_hdiff );
		const sint16 d3 = min( corner_height[2] - new_height, max_hdiff );
		const sint16 d4 = min( corner_height[3] - new_height, max_hdiff );
		return d4 * 27 + d3 * 9 + d2 * 3 + d1;
	}
	return 0;
}


uint8 karte_t::calc_natural_slope( const koord k ) const
{
	if(is_within_grid_limits(k.x, k.y)) {

		const sint8 * p = &grid_hgts[k.x + k.y*(sint32)(get_size().x+1)];

		const int h1 = *p;
		const int h2 = *(p+1);
		const int h3 = *(p+get_size().x+2);
		const int h4 = *(p+get_size().x+1);

		const int mini = min(min(h1,h2), min(h3,h4));

		const int d1=h1-mini;
		const int d2=h2-mini;
		const int d3=h3-mini;
		const int d4=h4-mini;

		return d1 * 27 + d2 * 9 + d3 * 3 + d4;
	}
	return 0;
}


bool karte_t::ist_wasser(koord k, koord dim) const
{
	koord k_check;
	for(  k_check.x = k.x;  k_check.x < k.x + dim.x;  k_check.x++  ) {
		for(  k_check.y = k.y;  k_check.y < k.y + dim.y;  k_check.y++  ) {
			if(  !is_within_grid_limits( k_check + koord(1, 1) )  ||  max_hgt(k_check) > get_water_hgt(k_check)  ) {
				return false;
			}
		}
	}
	return true;
}


bool karte_t::square_is_free(koord k, sint16 w, sint16 h, int *last_y, climate_bits cl) const
{
	if(k.x < 0  ||  k.y < 0  ||  k.x+w > get_size().x || k.y+h > get_size().y) {
		return false;
	}

	grund_t *gr = lookup_kartenboden(k);
	const sint16 platz_h = gr->get_grund_hang() ? max_hgt(k) : gr->get_hoehe();	// remember the max height of the first tile

	koord k_check;
	for(k_check.y=k.y+h-1; k_check.y>=k.y; k_check.y--) {
		for(k_check.x=k.x; k_check.x<k.x+w; k_check.x++) {
			const grund_t *gr = lookup_kartenboden(k_check);

			// we can built, if: max height all the same, everything removable and no buildings there
			hang_t::typ slope = gr->get_grund_hang();
			sint8 max_height = gr->get_hoehe() + hang_t::max_diff(slope);
			climate test_climate = get_climate(k_check);
			if(  cl & (1 << water_climate)  &&  test_climate != water_climate  ) {
				bool neighbour_water = false;
				for(int i=0; i<8  &&  !neighbour_water; i++) {
					if(  is_within_limits(k_check + koord::neighbours[i])  &&  get_climate( k_check + koord::neighbours[i] ) == water_climate  ) {
						neighbour_water = true;
					}
				}
				if(  neighbour_water  ) {
					test_climate = water_climate;
				}
			}
			if(  platz_h != max_height  ||  !gr->ist_natur()  ||  gr->kann_alle_obj_entfernen(NULL) != NULL  ||
			     (cl & (1 << test_climate)) == 0  ||  ( slope && (lookup( gr->get_pos()+koord3d(0,0,1) ) ||
			     (hang_t::max_diff(slope)==2 && lookup( gr->get_pos()+koord3d(0,0,2) )) ))  ) {
				if(  last_y  ) {
					*last_y = k_check.y;
				}
				return false;
			}
		}
	}
	return true;
}


slist_tpl<koord> *karte_t::find_squares(sint16 w, sint16 h, climate_bits cl, sint16 old_x, sint16 old_y) const
{
	slist_tpl<koord> * list = new slist_tpl<koord>();
	koord start;
	int last_y;

DBG_DEBUG("karte_t::finde_plaetze()","for size (%i,%i) in map (%i,%i)",w,h,get_size().x,get_size().y );
	for(start.x=0; start.x<get_size().x-w; start.x++) {
		for(start.y=start.x<old_x?old_y:0; start.y<get_size().y-h; start.y++) {
			if(square_is_free(start, w, h, &last_y, cl)) {
				list->insert(start);
			}
			else {
				// Optimiert fuer groessere Felder, hehe!
				// Die Idee: wenn bei 2x2 die untere Reihe nicht geht, koennen
				// wir gleich 2 tiefer weitermachen! V. Meyer
				start.y = last_y;
			}
		}
	}
	return list;
}


/**
 * Play a sound, but only if near enoungh.
 * Sounds are muted by distance and clipped completely if too far away.
 *
 * @author Hj. Malthaner
 */
bool karte_t::play_sound_area_clipped(koord const k, uint16 const idx) const
{
	if(is_sound  &&  zeiger) {
		const int dist = koord_distance( k, zeiger->get_pos() );

		if(dist < 100) {
			int xw = (2*display_get_width())/get_tile_raster_width();
			int yw = (4*display_get_height())/get_tile_raster_width();

			uint8 const volume = (uint8)(255U * (xw + yw) / (xw + yw + 64 * dist));
			if (volume > 8) {
				sound_play(idx, volume);
			}
		}
		return dist < 25;
	}
	return false;
}


void karte_t::save(const char *filename, loadsave_t::mode_t savemode, const char *version_str, const char *ex_version_str, bool silent )
{
DBG_MESSAGE("karte_t::speichern()", "saving game to '%s'", filename);
	loadsave_t  file;
	bool save_temp = strstart( filename, "save/" );
	const char *savename = save_temp ? "save/_temp.sve" : filename;

	display_show_load_pointer( true );
	if(env_t::networkmode && !env_t::server && savemode == loadsave_t::bzip2)
	{
		// Make local saving/loading faster in network mode.
		savemode = loadsave_t::zipped;
	}
	if(!file.wr_open( savename, savemode, env_t::objfilename.c_str(), version_str, ex_version_str )) {
		create_win(new news_img("Kann Spielstand\nnicht speichern.\n"), w_info, magic_none);
		dbg->error("karte_t::speichern()","cannot open file for writing! check permissions!");
	}
	else {
		save(&file,silent);
		const char *success = file.close();
		if(success) {
			static char err_str[512];
			sprintf( err_str, translator::translate("Error during saving:\n%s"), success );
			create_win( new news_img(err_str), w_time_delete, magic_none);
		}
		else {
			if(  save_temp  ) {
				remove( filename );
				rename( savename, filename );
			}
			if(!silent) {
				create_win( new news_img("Spielstand wurde\ngespeichert!\n"), w_time_delete, magic_none);
				// update the filename, if no autosave
				settings.set_filename(filename);
			}
		}
		reset_interaction();
	}
	display_show_load_pointer( false );
}


void karte_t::save(loadsave_t *file,bool silent)
{
	bool needs_redraw = false;

	loadingscreen_t *ls = NULL;
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "start");
	if(!silent) {
		ls = new loadingscreen_t( translator::translate("Saving map ..."), get_size().y );
	}

	// rotate the map until it can be saved completely
	for( int i=0;  i<4  &&  nosave_warning;  i++  ) {
		rotate90();
		needs_redraw = true;
	}
	// seems not successful
	if(nosave_warning) {
		// but then we try to rotate until only warnings => some buildings may be broken, but factories should be fine
		for( int i=0;  i<4  &&  nosave;  i++  ) {
			rotate90();
			needs_redraw = true;
		}
		if(  nosave  ) {
			dbg->error( "karte_t::speichern()","Map cannot be saved in any rotation!" );
			create_win( new news_img("Map may be not saveable in any rotation!"), w_info, magic_none);
			// still broken, but we try anyway to save it ...
		}
	}
	// only broken buildings => just warn
	if(nosave_warning) {
		dbg->error( "karte_t::speichern()","Some buildings may be broken by saving!" );
	}

	/* If the current tool is a two_click_werkzeug, call cleanup() in order to delete dummy grounds (tunnel + monorail preview)
	 * THIS MUST NOT BE DONE IN NETWORK MODE!
	 */
	for(  uint8 sp_nr=0;  sp_nr<MAX_PLAYER_COUNT;  sp_nr++  ) {
		if(  two_click_werkzeug_t* tool = dynamic_cast<two_click_werkzeug_t*>(werkzeug[sp_nr]) ) {
			tool->cleanup( false );
		}
	}

	file->set_buffered(true);

	// do not set value for empyt player
	uint8 old_sp[MAX_PLAYER_COUNT];
	for(  int i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
		old_sp[i] = settings.get_player_type(i);
		if(  spieler[i]==NULL  ) {
			settings.set_player_type(i, spieler_t::EMPTY);
		}
	}
	settings.rdwr(file);
	for(  int i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
		settings.set_player_type(i, old_sp[i]);
	}

	if(file->get_experimental_version() <= 1)
	{
		uint32 old_ticks = (uint32)ticks;
		file->rdwr_long(old_ticks);
		ticks = old_ticks;
	}
	else
	{
		file->rdwr_longlong(ticks);
	}
	file->rdwr_long(last_month);
	file->rdwr_long(last_year);

	// rdwr cityrules (and associated settings) for networkgames
	if(file->get_version()>102002 && (file->get_experimental_version() == 0 || file->get_experimental_version() >= 9))
	{
		bool do_rdwr = env_t::networkmode;
		file->rdwr_bool(do_rdwr);
		if (do_rdwr) 
		{
			if(file->get_experimental_version() >= 9)
			{
				stadt_t::cityrules_rdwr(file);
				privatecar_rdwr(file);
			}
			stadt_t::electricity_consumption_rdwr(file);
			if(file->get_version()>102003 && (file->get_experimental_version() == 0 || file->get_experimental_version() >= 9)) 
			{
				vehikelbauer_t::rdwr_speedbonus(file);
			}
		}
	}

	FOR(weighted_vector_tpl<stadt_t*>, const i, stadt) {
		i->rdwr(file);
		if(silent) {
			INT_CHECK("saving");
		}
	}
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "saved cities ok");

	for(int j=0; j<get_size().y; j++) {
		for(int i=0; i<get_size().x; i++) {
			plan[i+j*cached_grid_size.x].rdwr(file, koord(i,j) );
		}
		if(silent) {
			INT_CHECK("saving");
		}
		else {
			ls->set_progress(j);
		}
	}
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "saved tiles");

	if(  file->get_version()<=102001  ) {
		// not needed any more
		for(int j=0; j<(get_size().y+1)*(sint32)(get_size().x+1); j++) {
			file->rdwr_byte(grid_hgts[j]);
		}
	DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "saved hgt");
	}

	sint32 fabs = fab_list.get_count();
	file->rdwr_long(fabs);
	FOR(vector_tpl<fabrik_t*>, const f, fab_list) {
		f->rdwr(file);
		if(silent) {
			INT_CHECK("saving");
		}
	}
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "saved fabs");

	sint32 haltcount=haltestelle_t::get_alle_haltestellen().get_count();
	file->rdwr_long(haltcount);
	FOR(vector_tpl<halthandle_t>, const s, haltestelle_t::get_alle_haltestellen()) {
		s->rdwr(file);
	}
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "saved stops");

	// svae number of convois
	if(  file->get_version()>=101000  ) {
		uint16 i=convoi_array.get_count();
		file->rdwr_short(i);
	}
	FOR(vector_tpl<convoihandle_t>, const cnv, convoi_array) {
		// one MUST NOT call INT_CHECK here or else the convoi will be broken during reloading!
		cnv->rdwr(file);
	}
	if(  file->get_version()<101000  ) {
		file->wr_obj_id("Ende Convois");
	}
	if(silent) {
		INT_CHECK("saving");
	}
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "saved %i convois",convoi_array.get_count());

	for(int i=0; i<MAX_PLAYER_COUNT; i++) {
// **** REMOVE IF SOON! *********
		if(file->get_version()<101000) {
			if(  i<8  ) {
				if(  spieler[i]  ) {
					spieler[i]->rdwr(file);
				}
				else {
					// simulate old ones ...
					spieler_t *sp = new spieler_t( this, i );
					sp->rdwr(file);
					delete sp;
				}
			}
		}
		else {
			if(  spieler[i]  ) {
				spieler[i]->rdwr(file);
			}
		}
	}
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "saved players");

	// saving messages
	if(  file->get_version()>=102005  ) {
		msg->rdwr(file);
	}
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "saved messages");

	// centered on what?
	sint32 dummy = viewport->get_world_position().x;
	file->rdwr_long(dummy);
	dummy = viewport->get_world_position().y;
	file->rdwr_long(dummy);

	if(file->get_version() >= 99018)
	{
		// Most recent Standard version is 99018
		
		for (int year = 0; year < /*MAX_WORLD_HISTORY_YEARS*/ 12; year++)
		{
			for (int cost_type = 0; cost_type < MAX_WORLD_COST; cost_type++)
			{
				if(file->get_experimental_version() < 12 && (cost_type == WORLD_JOBS || cost_type == WORLD_VISITOR_DEMAND || cost_type == WORLD_CAR_OWNERSHIP))
				{
					finance_history_year[year][cost_type] = 0;
				}
				else
				{
					file->rdwr_longlong(finance_history_year[year][cost_type]);
				}
			}
		}
		for (int month = 0; month < /*MAX_WORLD_HISTORY_MONTHS*/ 12; month++)
		{
			for (int cost_type = 0; cost_type < MAX_WORLD_COST; cost_type++)
			{
				if(file->get_experimental_version() < 12 && (cost_type == WORLD_JOBS || cost_type == WORLD_VISITOR_DEMAND || cost_type == WORLD_CAR_OWNERSHIP))
				{
					finance_history_month[month][cost_type] = 0;
				}
				else
				{
					file->rdwr_longlong(finance_history_month[month][cost_type]);
				}
			}
		}
	}

	// finally a possible scenario
	scenario->rdwr( file );

	if(file->get_experimental_version() >= 2)
	{
		file->rdwr_short(base_pathing_counter);
	}
	if(file->get_experimental_version() >= 7 && file->get_experimental_version() < 9 && file->get_version() < 110006)
	{
		double old_proportion = (double)industry_density_proportion / 10000.0;
		file->rdwr_double(old_proportion);
		industry_density_proportion = old_proportion * 10000.0;
	}
	else if(file->get_experimental_version() >= 9 && file->get_version() >= 110006 && file->get_experimental_version() < 11)
	{
		// Versions before 10.16 used an excessively low (and therefore inaccurate) integer for the industry density proportion. 
		// Detect this by checking whether the highest bit is set (it will not be naturally, so will only be set if this is 
		// 10.16 or higher, and not 11.0 and later, where we can assume that the numbers are correct and can be dealt with simply).
		uint32 idp = industry_density_proportion;

		idp |= 0x8000;
		file->rdwr_long(idp);
	}
	else if(file->get_experimental_version() >= 11)
	{
		file->rdwr_long(industry_density_proportion);
	}

	if(file->get_experimental_version() >=9 && file->get_version() >= 110000)
	{
		if(file->get_experimental_version() < 11)
		{
			// Was next_private_car_update_month
			uint8 dummy;
			file->rdwr_byte(dummy);
		}
		
		// Existing values now saved in order to prevent network desyncs
		file->rdwr_long(citycar_speed_average);
		file->rdwr_bool(recheck_road_connexions);
		file->rdwr_short(generic_road_time_per_tile_city);
		file->rdwr_short(generic_road_time_per_tile_intercity);
		file->rdwr_long(max_road_check_depth);
		if(file->get_experimental_version() < 10)
		{
			double old_density = actual_industry_density / 100.0;
			file->rdwr_double(old_density);
			actual_industry_density = old_density * 100;
		}
		else
		{
			file->rdwr_long(actual_industry_density);
		}
	}

	if(file->get_experimental_version() >= 12)
	{
		file->rdwr_long(next_step_passenger);
		file->rdwr_long(next_step_mail);
	}

	if(  file->get_version() >= 112008  ) {
		xml_tag_t t( file, "motd_t" );

		chdir( env_t::user_dir );
		// maybe show message about server
DBG_MESSAGE("karte_t::speichern(loadsave_t *file)", "motd filename %s", env_t::server_motd_filename.c_str() );
		if(  FILE *fmotd = fopen( env_t::server_motd_filename.c_str(), "r" )  ) {
			struct stat st;
			stat( env_t::server_motd_filename.c_str(), &st );
			sint32 len = min( 32760, st.st_size+1 );
			char *motd = (char *)malloc( len );
			fread( motd, len-1, 1, fmotd );
			fclose( fmotd );
			motd[len] = 0;
			file->rdwr_str( motd, len );
			free( motd );
		}
		else {
			// no message
			char *motd = "";
			file->rdwr_str( motd, 1 );
		}
	}

	// MUST be at the end of the load/save routine.
	// save all open windows (upon request)
	file->rdwr_byte( active_player_nr );
	rdwr_all_win(file);

	file->set_buffered(false);

	if(needs_redraw) {
		update_map();
	}
	if(!silent) {
		delete ls;
	}
}


// store missing obj during load and their severity
void karte_t::add_missing_paks( const char *name, missing_level_t level )
{
	if(  missing_pak_names.get( name )==NOT_MISSING  ) {
		missing_pak_names.put( strdup(name), level );
	}
}


// LOAD, not save
// just the preliminaries, opens the file, checks the versions ...
bool karte_t::load(const char *filename)
{
	cbuffer_t name;
	bool ok = false;
	bool restore_player_nr = false;
	bool server_reload_pwd_hashes = false;
	mute_sound(true);
	display_show_load_pointer(true);
	loadsave_t file;
	cities_awaiting_private_car_route_check.clear();

	// clear hash table with missing paks (may cause some small memory loss though)
	missing_pak_names.clear();

	DBG_MESSAGE("karte_t::load", "loading game from '%s'", filename);

	// reloading same game? Remeber pos
	const koord oldpos = settings.get_filename()[0]>0  &&  strncmp(filename,settings.get_filename(),strlen(settings.get_filename()))==0 ? viewport->get_world_position() : koord::invalid;

	if(  strstart(filename, "net:")  ) {
		// probably finish network mode?
		if(  env_t::networkmode  ) {
			network_core_shutdown();
		}
		chdir( env_t::user_dir );
		const char *err = network_connect(filename+4, this);
		if(err) {
			create_win( new news_img(err), w_info, magic_none );
			display_show_load_pointer(false);
			step_mode = NORMAL;
			return false;
		}
		else {
			env_t::networkmode = true;
			name.printf( "client%i-network.sve", network_get_client_id() );
			restore_player_nr = strcmp( last_network_game.c_str(), filename )==0;
			if(  !restore_player_nr  ) {
				last_network_game = filename;
			}
		}
	}
	else {
		// probably finish network mode first?
		if(  env_t::networkmode  ) {
			if (  env_t::server  ) {
				char fn[256];
				sprintf( fn, "server%d-network.sve", env_t::server );
				if(  strcmp(filename, fn) != 0  ) {
					// stay in networkmode, but disconnect clients
					dbg->warning("karte_t::load","disconnecting all clients");
					network_reset_server();
				}
				else {
					// read password hashes from separate file
					// as they are not in the savegame to avoid sending them over network
					server_reload_pwd_hashes = true;
				}
			}
			else {
				// check, if reload during sync
				char fn[256];
				sprintf( fn, "client%i-network.sve", network_get_client_id() );
				if(  strcmp(filename,fn)!=0  ) {
					// no sync => finish network mode
					dbg->warning("karte_t::load","finished network mode");
					network_disconnect();
					finish_loop = false; // do not trigger intro screen
					// closing the socket will tell the server, I am away too
				}
			}
		}
		name.append(filename);
	}

	if(!file.rd_open(name)) {

		if(  (sint32)file.get_version()==-1  ||  file.get_version()>loadsave_t::int_version(SAVEGAME_VER_NR, NULL, NULL).version  ) {
			dbg->warning("karte_t::load()", translator::translate("WRONGSAVE") );
			create_win( new news_img("WRONGSAVE"), w_info, magic_none );
		}
		else {
			dbg->warning("karte_t::load()", translator::translate("Kann Spielstand\nnicht laden.\n") );
			create_win(new news_img("Kann Spielstand\nnicht laden.\n"), w_info, magic_none);
		}
	}
	else if(file.get_version() < 84006) {
		// too old
		dbg->warning("karte_t::load()", translator::translate("WRONGSAVE") );
		create_win(new news_img("WRONGSAVE"), w_info, magic_none);
	}
	else {
DBG_MESSAGE("karte_t::load()","Savegame version is %d", file.get_version());

		load(&file);

		if(  env_t::networkmode  ) {
			clear_command_queue();
		}

		if(  env_t::server  ) {
			step_mode = FIX_RATIO;
			if(  env_t::server  ) {
				// meaningless to use a locked map; there are passwords now
				settings.set_allow_player_change(true);
				// language of map becomes server language
				settings.set_name_language_iso(translator::get_lang()->iso_base);
			}

			if(  server_reload_pwd_hashes  ) {
				char fn[256];
				sprintf( fn, "server%d-pwdhash.sve", env_t::server );
				loadsave_t pwdfile;
				if (pwdfile.rd_open(fn)) {
					rdwr_player_password_hashes( &pwdfile );
					// correct locking info
					nwc_auth_player_t::init_player_lock_server(this);
					pwdfile.close();
				}
				server_reload_pwd_hashes = false;
			}
		}
		else if(  env_t::networkmode  ) {
			step_mode = PAUSE_FLAG|FIX_RATIO;
			switch_active_player( last_active_player_nr, true );
			if(  is_within_limits(oldpos)  ) {
				// go to position when last disconnected
				viewport->change_world_position( oldpos );
			}
		}
		else {
			step_mode = NORMAL;
		}

		ok = true;
		file.close();

		if(  !scenario->rdwr_ok()  ) {
			// error during loading of savegame of scenario
			const char* err = scenario->get_error_text();
			if (err == NULL) {
				err = "Loading scenario failed.";
			}
			create_win( new news_img( err ), w_info, magic_none);
			delete scenario;
			scenario = new scenario_t(this);
		}
		else if(  !env_t::networkmode  ||  !env_t::restore_UI  ) {
			// warning message about missing paks
			if(  !missing_pak_names.empty()  ) {

				cbuffer_t msg;
				msg.append("<title>");
				msg.append(translator::translate("Missing pakfiles"));
				msg.append("</title>\n");

				cbuffer_t error_paks;
				cbuffer_t warning_paks;

				cbuffer_t paklog;
				paklog.append( "\n" );
				FOR(stringhashtable_tpl<missing_level_t>, const& i, missing_pak_names) {
					if (i.value <= MISSING_ERROR) {
						error_paks.append(translator::translate(i.key));
						error_paks.append("<br>\n");
						paklog.append( i.key );
						paklog.append("\n" );
					}
					else {
						warning_paks.append(translator::translate(i.key));
						warning_paks.append("<br>\n");
					}
				}

				if(  error_paks.len()>0  ) {
					msg.append("<h1>");
					msg.append(translator::translate("Pak which may cause severe errors:"));
					msg.append("</h1><br>\n");
					msg.append("<br>\n");
					msg.append( error_paks );
					msg.append("<br>\n");
					dbg->warning( "The following paks are missing and may cause errors", paklog );
				}

				if(  warning_paks.len()>0  ) {
					msg.append("<h1>");
					msg.append(translator::translate("Pak which may cause visual errors:"));
					msg.append("</h1><br>\n");
					msg.append("<br>\n");
					msg.append( warning_paks );
					msg.append("<br>\n");
				}

				help_frame_t *win = new help_frame_t();
				win->set_text( msg );
				create_win(win, w_info, magic_pakset_info_t);
			}
			// do not notify if we restore everything
			create_win( new news_img("Spielstand wurde\ngeladen!\n"), w_time_delete, magic_none);
		}
		set_dirty();

		reset_timer();
		recalc_average_speed();
		mute_sound(false);

		werkzeug_t::update_toolbars();
		set_werkzeug( werkzeug_t::general_tool[WKZ_ABFRAGE], get_active_player() );
	}

	settings.set_filename(filename);
	display_show_load_pointer(false);

	calc_generic_road_time_per_tile_city();
	calc_generic_road_time_per_tile_intercity();
	calc_max_road_check_depth();

	return ok;
}


void karte_t::plans_laden_abschliessen( sint16 x_min, sint16 x_max, sint16 y_min, sint16 y_max )
{
	for(  int y = y_min;  y < y_max;  y++  ) {
		for(  int x = x_min; x < x_max;  x++  ) {
			const planquadrat_t *plan = access_nocheck(x,y);
			const int boden_count = plan->get_boden_count();
			for(  int schicht = 0;  schicht < boden_count;  schicht++  ) {
				grund_t *gr = plan->get_boden_bei(schicht);
				for(  int n = 0;  n < gr->get_top();  n++  ) {
					obj_t *obj = gr->obj_bei(n);
					if(obj) {
						obj->laden_abschliessen();
					}
				}
				if(  load_version.version <= 111000  &&  gr->ist_natur()  ) {
					gr->sort_trees();
				}
				gr->calc_bild();
			}
		}
	}
}


void karte_t::load(loadsave_t *file)
{
	char buf[80];

	intr_disable();
	dbg->message("karte_t::load()", "Prepare for loading" );
	for(  uint i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
		werkzeug[i] = werkzeug_t::general_tool[WKZ_ABFRAGE];
	}
	destroy_all_win(true);

	clear_random_mode(~LOAD_RANDOM);
	set_random_mode(LOAD_RANDOM);
	destroy();

	loadingscreen_t ls(translator::translate("Loading map ..."), 1, true, true );

	clear_random_mode(~LOAD_RANDOM);
	set_random_mode(LOAD_RANDOM);

	// Added by : Knightly
	path_explorer_t::initialise(this);

	//fast_forward = false;

	tile_counter = 0;

	simloops = 60;

	// zum laden vorbereiten -> tabelle loeschen
	powernet_t::neue_karte();
	pumpe_t::neue_karte();
	senke_t::neue_karte();

	const uint16 old_scale_factor = get_settings().get_meters_per_tile();
	file->set_buffered(true);

	// jetzt geht das laden los
	dbg->warning("karte_t::load", "Fileversion: %d", file->get_version());
	// makes a copy:
	settings = env_t::default_settings;
	settings.rdwr(file);

	// We may wish to override the settings saved in the file.
	// But not if we are a network client.
	if (  !env_t::networkmode || env_t::server  ) {
		bool read_progdir_simuconf = env_t::default_settings.get_progdir_overrides_savegame_settings();
		bool read_pak_simuconf = env_t::default_settings.get_pak_overrides_savegame_settings();
		bool read_userdir_simuconf = env_t::default_settings.get_userdir_overrides_savegame_settings();
		tabfile_t simuconf;
		sint16 idummy;
		string dummy;

		if (read_progdir_simuconf) {
			chdir( env_t::program_dir );
			if(simuconf.open("config/simuconf.tab")) {
				printf("parse_simuconf() in program dir (%s) for override of save file: ", "config/simuconf.tab");
				settings.parse_simuconf( simuconf, idummy, idummy, idummy, dummy );
				simuconf.close();
			}
			chdir( env_t::user_dir );
		}
		if (read_pak_simuconf) {
			chdir( env_t::program_dir );
			std::string pak_simuconf = env_t::objfilename + "config/simuconf.tab";
			if(simuconf.open(pak_simuconf.c_str())) {
				printf("parse_simuconf() in pak dir (%s) for override of save file: ", pak_simuconf.c_str() );
				settings.parse_simuconf( simuconf, idummy, idummy, idummy, dummy );
				simuconf.close();
			}
			chdir( env_t::user_dir );
		}
		if (read_userdir_simuconf) {
			chdir( env_t::user_dir );
			std::string userdir_simuconf = "simuconf.tab";
			if(simuconf.open("simuconf.tab")) {
				printf("parse_simuconf() in user dir (%s) for override of save file: ", userdir_simuconf.c_str() );
				settings.parse_simuconf( simuconf, idummy, idummy, idummy, dummy );
				simuconf.close();
			}
		}

	}

	loaded_rotation = settings.get_rotation();


	// some functions (laden_abschliessen) need to know what version was loaded
	load_version.version = file->get_version();
	load_version.experimental_version = file->get_experimental_version();

	if(  env_t::networkmode  ) {
		// clear the checklist history
		for(  int i=0;  i<LAST_CHECKLISTS_COUNT;  ++i  ) {
			last_checklists[i] = checklist_t();
		}
		for(  int i = 0;  i < CHK_RANDS  ;  i++  ) {
			rands[i] = 0;
		}
	}


#ifndef DEBUG_SIMRAND_CALLS
	if(  env_t::networkmode  ) {
		// to have games synchronized, transfer random counter too
		setsimrand(settings.get_random_counter(), 0xFFFFFFFFu );
//		setsimrand(0, 0xFFFFFFFFu );
		translator::init_custom_names(settings.get_name_language_id());
	}
#endif

	if(  !env_t::networkmode  ||  (env_t::server  &&  socket_list_t::get_playing_clients()==0)  ) {
		if (settings.get_allow_player_change() && env_t::default_settings.get_use_timeline() < 2) {
			// not locked => eventually switch off timeline settings, if explicitly stated
			settings.set_use_timeline(env_t::default_settings.get_use_timeline());
			DBG_DEBUG("karte_t::load", "timeline: reset to %i", env_t::default_settings.get_use_timeline() );
		}
	}
	if (settings.get_beginner_mode()) {
		warenbauer_t::set_multiplier(settings.get_beginner_price_factor(), settings.get_meters_per_tile());
	}
	else {
		warenbauer_t::set_multiplier( 1000, settings.get_meters_per_tile() );
	}
	// Must do this just after set_multiplier, since it depends on warenbauer_t having registered all wares:
	settings.cache_speedbonuses();

	if(old_scale_factor != get_settings().get_meters_per_tile())
	{
		set_scale();
	}

	grundwasser = (sint8)(settings.get_grundwasser());

//
//	DBG_DEBUG("karte_t::load()","grundwasser %i",grundwasser);
//	grund_besch_t::calc_water_level( this, height_to_climate );

	DBG_DEBUG("karte_t::load()","grundwasser %i",grundwasser);

	init_height_to_climate();


	// just an initialisation for the loading
	season = (2+last_month/3)&3; // summer always zero
	snowline = settings.get_winter_snowline() + grundwasser;

	DBG_DEBUG("karte_t::load", "settings loaded (groesse %i,%i) timeline=%i beginner=%i", settings.get_groesse_x(), settings.get_groesse_y(), settings.get_use_timeline(), settings.get_beginner_mode());

	// wird gecached, um den Pointerzugriff zu sparen, da
	// die groesse _sehr_ oft referenziert wird
	cached_grid_size.x = settings.get_groesse_x();
	cached_grid_size.y = settings.get_groesse_y();
	cached_size_max = max(cached_grid_size.x,cached_grid_size.y);
	cached_size.x = cached_grid_size.x-1;
	cached_size.y = cached_grid_size.y-1;
	viewport->set_x_off(0);
	viewport->set_y_off(0);

	// Reliefkarte an neue welt anpassen
	reliefkarte_t::get_karte()->init();

	ls.set_max( get_size().y*2+256 );
	init_felder();


	// reinit pointer with new pointer object and old values
	zeiger = new zeiger_t(koord3d::invalid, NULL );

	hausbauer_t::neue_karte();
	fabrikbauer_t::neue_karte();

	DBG_DEBUG("karte_t::load", "init felder ok");

	if(file->get_experimental_version() <= 1)
	{
		uint32 old_ticks = (uint32)ticks;
		file->rdwr_long(old_ticks);
		ticks = (sint64)old_ticks;
	}
	else
	{
		file->rdwr_longlong(ticks);
	}
	file->rdwr_long(last_month);
	file->rdwr_long(last_year);
	if(file->get_version()<86006) {
		last_year += env_t::default_settings.get_starting_year();
	}
	// old game might have wrong month
	last_month %= 12;
	// set the current month count
	set_ticks_per_world_month_shift(settings.get_bits_per_month());
	current_month = last_month + (last_year*12);
	season = (2+last_month/3)&3; // summer always zero
	next_month_ticks = ( (ticks >> karte_t::ticks_per_world_month_shift) + 1 ) << karte_t::ticks_per_world_month_shift;
	last_step_ticks = ticks;
	network_frame_count = 0;
	sync_steps = 0;
	steps = 0;
	network_frame_count = 0;
	sync_steps = 0;
	step_mode = PAUSE_FLAG;

DBG_MESSAGE("karte_t::load()","savegame loading at tick count %i",ticks);
	recalc_average_speed();	// resets timeline
	// recalc_average_speed may have opened message windows
	destroy_all_win(true);

DBG_MESSAGE("karte_t::load()", "init player");
	for(int i=0; i<MAX_PLAYER_COUNT; i++) {
		if(  file->get_version()>=101000  ) {
			// since we have different kind of AIs
			delete spieler[i];
			spieler[i] = NULL;
			new_spieler(i, settings.spieler_type[i]);
		}
		else if(i<8) {
			// get the old player ...
			if(  spieler[i]==NULL  ) {
				new_spieler( i, (i==3) ? spieler_t::AI_PASSENGER : spieler_t::AI_GOODS );
			}
			settings.spieler_type[i] = spieler[i]->get_ai_id();
		}
	}
	// so far, player 1 will be active (may change in future)
	active_player = spieler[0];
	active_player_nr = 0;

	// rdwr cityrules for networkgames
	if(file->get_version() > 102002 && (file->get_experimental_version() == 0 || file->get_experimental_version() >= 9)) {
		bool do_rdwr = env_t::networkmode;
		file->rdwr_bool(do_rdwr);
		if(do_rdwr) 
		{
			// This stuff should not be in a saved game.  Unfortunately, due to the vagaries
			// of the poorly-designed network interface, it is.  Because it is, we need to override
			// it on demand.
			bool pak_overrides = env_t::default_settings.get_pak_overrides_savegame_settings();

			// First cityrules
			stadt_t::cityrules_rdwr(file);
			if (  !env_t::networkmode || env_t::server  ) {
				if (pak_overrides) {
					chdir( env_t::program_dir );
					printf("stadt_t::cityrules_init in pak dir (%s) for override of save file: ", env_t::objfilename.c_str() );
					stadt_t::cityrules_init( env_t::objfilename );
					chdir( env_t::user_dir );
				}
			}

			// Next privatecar and electricity
			if(file->get_experimental_version() >= 9)
			{
				privatecar_rdwr(file);
				stadt_t::electricity_consumption_rdwr(file);
				if(!env_t::networkmode || env_t::server) 
				{
					if(pak_overrides) 
					{
						chdir(env_t::program_dir);
						printf("stadt_t::privatecar_init in pak dir (%s) for override of save file: ", env_t::objfilename.c_str());
						privatecar_init(env_t::objfilename);
						printf("stadt_t::electricity_consumption_init in pak dir (%s) for override of save file: ", env_t::objfilename.c_str());
						stadt_t::electricity_consumption_init(env_t::objfilename);
						chdir(env_t::user_dir);
					}
				}
			}

			// Finally speedbonus
			if(file->get_version()>102003 && (file->get_experimental_version() == 0 || file->get_experimental_version() >= 9)) 
			{
				vehikelbauer_t::rdwr_speedbonus(file);
				if (  !env_t::networkmode || env_t::server  ) {
					if (pak_overrides) {
						chdir( env_t::program_dir );
						printf("stadt_t::speedbonus_init in pak dir (%s) for override of save file: ", env_t::objfilename.c_str() );
						vehikelbauer_t::speedbonus_init( env_t::objfilename );
						chdir( env_t::user_dir );
					}
				}
			}
		}
	}
	DBG_DEBUG("karte_t::load", "init %i cities", settings.get_anzahl_staedte());
	stadt.clear();
	stadt.resize(settings.get_anzahl_staedte());
	for (int i = 0; i < settings.get_anzahl_staedte(); ++i) {
		stadt_t *s = new stadt_t(file);
		const sint32 population = s->get_einwohner();
		stadt.append(s, population > 0 ? population : 1); // This has to be at least 1, or else the weighted vector will not add it. TODO: Remove this check once the population checking method is improved.
	}

	DBG_MESSAGE("karte_t::load()","loading blocks");
	old_blockmanager_t::rdwr(this, file);

	DBG_MESSAGE("karte_t::load()","loading tiles");
	for (int y = 0; y < get_size().y; y++) {
		for (int x = 0; x < get_size().x; x++) {
			plan[x+y*cached_grid_size.x].rdwr(file, koord(x,y) );
		}
		if(file->is_eof()) {
			dbg->fatal("karte_t::load()","Savegame file mangled (too short)!");
		}
		ls.set_progress( y/2 );
	}

	if(file->get_version()<99005) {
		DBG_MESSAGE("karte_t::load()","loading grid for older versions");
		for (int y = 0; y <= get_size().y; y++) {
			for (int x = 0; x <= get_size().x; x++) {
				sint32 hgt;
				file->rdwr_long(hgt);
				// old height step was 16!
				set_grid_hgt(x, y, hgt/16 );
			}
		}
	}
	else if(  file->get_version()<=102001  )  {
		// hgt now bytes
		DBG_MESSAGE("karte_t::load()","loading grid for older versions");
		for( sint32 i=0;  i<(get_size().y+1)*(sint32)(get_size().x+1);  i++  ) {
			file->rdwr_byte(grid_hgts[i]);
		}
	}

	if(file->get_version()<88009) {
		DBG_MESSAGE("karte_t::load()","loading slopes from older version");
		// Hajo: load slopes for older versions
		// now part of the grund_t structure
		for (int y = 0; y < get_size().y; y++) {
			for (int x = 0; x < get_size().x; x++) {
				sint8 slope;
				file->rdwr_byte(slope);
				// convert slopes from old single height saved game
				slope = (scorner1(slope) + scorner2(slope) * 3 + scorner3(slope) * 9 + scorner4(slope) * 27) * env_t::pak_height_conversion_factor;
				access_nocheck(x, y)->get_kartenboden()->set_grund_hang(slope);
			}
		}
	}

	if(file->get_version()<=88000) {
		// because from 88.01.4 on the foundations are handled differently
		for (int y = 0; y < get_size().y; y++) {
			for (int x = 0; x < get_size().x; x++) {
				koord k(x,y);
				grund_t *gr = access_nocheck(x, y)->get_kartenboden();
				if(  gr->get_typ()==grund_t::fundament  ) {
					gr->set_hoehe( max_hgt_nocheck(k) );
					gr->set_grund_hang( hang_t::flach );
					// transfer object to on new grund
					for(  int i=0;  i<gr->get_top();  i++  ) {
						gr->obj_bei(i)->set_pos( gr->get_pos() );
					}
				}
			}
		}
	}

	if(  file->get_version() < 112007  ) {
		// set climates
		for(  sint16 y = 0;  y < get_size().y;  y++  ) {
			for(  sint16 x = 0;  x < get_size().x;  x++  ) {
				calc_climate( koord( x, y ), false );
			}
		}
	}

	// Reliefkarte an neue welt anpassen
	DBG_MESSAGE("karte_t::load()", "init relief");
	win_set_world( this );
	reliefkarte_t::get_karte()->init();

	sint32 fabs;
	file->rdwr_long(fabs);
	DBG_MESSAGE("karte_t::load()", "prepare for %i factories", fabs);

	for(sint32 i = 0; i < fabs; i++) {
		// liste in gleicher reihenfolge wie vor dem speichern wieder aufbauen
		fabrik_t *fab = new fabrik_t(file);
		if(fab->get_besch()) {
			fab_list.append(fab);
		}
		else {
			dbg->error("karte_t::load()","Unknown fabrik skipped!");
			delete fab;
		}
		if(i&7) {
			ls.set_progress( get_size().y/2+(128*i)/fabs );
		}
	}

	// load linemanagement status (and lines)
	// @author hsiegeln
	if (file->get_version() > 82003  &&  file->get_version()<88003) {
		DBG_MESSAGE("karte_t::load()", "load linemanagement");
		get_spieler(0)->simlinemgmt.rdwr(file, get_spieler(0));
	}
	// end load linemanagement

	DBG_MESSAGE("karte_t::load()", "load stops");
	// now load the stops
	// (the players will be load later and overwrite some values,
	//  like the total number of stops build (for the numbered station feature)
	haltestelle_t::start_load_game();
	if(file->get_version()>=99008) {
		sint32 halt_count;
		file->rdwr_long(halt_count);
		DBG_MESSAGE("karte_t::load()","%d halts loaded",halt_count);
		for(int i=0; i<halt_count; i++) {
			halthandle_t halt = haltestelle_t::create( file );
			if(!halt->existiert_in_welt()) {
				dbg->warning("karte_t::load()", "could not restore stop near %i,%i", halt->get_init_pos().x, halt->get_init_pos().y );
			}
			ls.set_progress( get_size().y/2+128+(get_size().y*i)/(2*halt_count) );
		}
		DBG_MESSAGE("karte_t::load()","%d halts loaded",halt_count);
	}

	DBG_MESSAGE("karte_t::load()", "load convois");
	uint16 convoi_nr = 65535;
	uint16 max_convoi = 65535;
	if(  file->get_version()>=101000  ) {
		file->rdwr_short(convoi_nr);
		max_convoi = convoi_nr;
	}
	while(  convoi_nr-->0  ) {

		if(  file->get_version()<101000  ) {
			file->rd_obj_id(buf, 79);
			if (strcmp(buf, "Ende Convois") == 0) {
				break;
			}
		}
		convoi_t *cnv = new convoi_t(file);
		convoi_array.append(cnv->self);

		if(cnv->in_depot()) {
			grund_t * gr = lookup(cnv->get_pos());
			depot_t *dep = gr ? gr->get_depot() : 0;
			if(dep) {
				//cnv->enter_depot(dep);
				dep->convoi_arrived(cnv->self, false);
			}
			else {
				dbg->error("karte_t::load()", "no depot for convoi, blocks may now be wrongly reserved!");
				cnv->destroy();
			}
		}
		else {
			sync_add( cnv );
		}
		if(  (convoi_array.get_count()&7) == 0  ) {
			ls.set_progress( get_size().y+(get_size().y*convoi_array.get_count())/(2*max_convoi)+128 );
		}
	}
DBG_MESSAGE("karte_t::load()", "%d convois/trains loaded", convoi_array.get_count());

	// jetzt koennen die spieler geladen werden
	for(int i=0; i<MAX_PLAYER_COUNT; i++) {
		if(  spieler[i]  ) {
			spieler[i]->rdwr(file);
			settings.automaten[i] = spieler[i]->is_active();
		}
		else {
			settings.automaten[i] = false;
		}
		ls.set_progress( (get_size().y*3)/2+128+8*i );
	}
DBG_MESSAGE("karte_t::load()", "players loaded");

	// loading messages
	if(  file->get_version()>=102005  ) {
		msg->rdwr(file);
	}
	else if(  !env_t::networkmode  ) {
		msg->clear();
	}
DBG_MESSAGE("karte_t::load()", "messages loaded");

	// nachdem die welt jetzt geladen ist koennen die Blockstrecken neu
	// angelegt werden
	old_blockmanager_t::laden_abschliessen(this);
	DBG_MESSAGE("karte_t::load()", "blocks loaded");

	sint32 mi,mj;
	file->rdwr_long(mi);
	file->rdwr_long(mj);
	DBG_MESSAGE("karte_t::load()", "Setting view to %d,%d", mi,mj);
	viewport->change_world_position( koord3d(mi,mj,0) );

	// right season for recalculations
	recalc_snowline();

DBG_MESSAGE("karte_t::load()", "%d ways loaded",weg_t::get_alle_wege().get_count());

	ls.set_progress( (get_size().y*3)/2+256 );

	world_xy_loop(&karte_t::plans_laden_abschliessen, SYNCX_FLAG);

	if(  file->get_version() < 112007  ) {
		// set transitions - has to be done after plans_laden_abschliessen
		world_xy_loop(&karte_t::recalc_transitions_loop, 0);
	}

	ls.set_progress( (get_size().y*3)/2+256+get_size().y/8 );

DBG_MESSAGE("karte_t::load()", "laden_abschliesen for tiles finished" );

	// must finish loading cities first before cleaning up factories
	weighted_vector_tpl<stadt_t*> new_weighted_stadt(stadt.get_count() + 1);
	FOR(weighted_vector_tpl<stadt_t*>, const s, stadt) {
		s->laden_abschliessen();
		// Must add city buildings to the world list here in any network game
		// to ensure that they are added in identical order.
		if(env_t::networkmode)
		{
			s->add_all_buildings_to_world_list();
		}
		new_weighted_stadt.append(s, s->get_einwohner());
		INT_CHECK("simworld 1278");
	}
	swap(stadt, new_weighted_stadt);
	DBG_MESSAGE("karte_t::load()", "cities initialized");

	

	ls.set_progress( (get_size().y*3)/2+256+get_size().y/4 );

	DBG_MESSAGE("karte_t::load()", "clean up factories");
	FOR(vector_tpl<fabrik_t*>, const f, fab_list) {
		f->laden_abschliessen();
	}

DBG_MESSAGE("karte_t::load()", "%d factories loaded", fab_list.get_count());

	ls.set_progress( (get_size().y*3)/2+256+get_size().y/3 );

	// resolve dummy stops into real stops first ...
	FOR(vector_tpl<halthandle_t>, const i, haltestelle_t::get_alle_haltestellen()) {
		if (i->get_besitzer() && i->existiert_in_welt()) {
			i->laden_abschliessen(file->get_experimental_version() < 10);
		}
	}

	// ... before removing dummy stops
	for(  vector_tpl<halthandle_t>::const_iterator i=haltestelle_t::get_alle_haltestellen().begin(); i!=haltestelle_t::get_alle_haltestellen().end();  ) {
		halthandle_t const h = *i;
		++i;
		if (!h->get_besitzer() || !h->existiert_in_welt()) {
			// this stop was only needed for loading goods ...
			haltestelle_t::destroy(h);	// remove from list
		}
	}

	ls.set_progress( (get_size().y*3)/2+256+(get_size().y*3)/8 );

	// adding lines and other stuff for convois
	for(unsigned i=0;  i<convoi_array.get_count();  i++ ) {
		convoihandle_t cnv = convoi_array[i];
		cnv->laden_abschliessen();
		// was deleted during loading => use same position again
		if(!cnv.is_bound()) {
			i--;
		}
	}
	haltestelle_t::end_load_game();

	// register all line stops and change line types, if needed
	for(int i=0; i<MAX_PLAYER_COUNT ; i++) {
		if(  spieler[i]  ) {
			spieler[i]->laden_abschliessen();
		}
	}


#if 0
	// reroute goods for benchmarking
	dt = dr_time();
	FOR(vector_tpl<halthandle_t>, const i, haltestelle_t::get_alle_haltestellen()) {
		sint16 dummy = 0x7FFF;
		i->reroute_goods(dummy);
	}
	DBG_MESSAGE("reroute_goods()","for all haltstellen_t took %ld ms", dr_time()-dt );
#endif

	// load history/create world history
	if(file->get_version()<99018) {
		restore_history();
	}
	else 
	{
		for(int year = 0; year < MAX_WORLD_HISTORY_YEARS; year++) 
		{
			for(int cost_type = 0; cost_type < MAX_WORLD_COST; cost_type++) 
			{
				if(file->get_experimental_version() < 12 && (cost_type == WORLD_JOBS || cost_type == WORLD_VISITOR_DEMAND || cost_type == WORLD_CAR_OWNERSHIP))
				{
					finance_history_year[year][cost_type] = 0;
				}
				else
				{
					file->rdwr_longlong(finance_history_year[year][cost_type]);
				}
			}
		}
		for(int month = 0; month < MAX_WORLD_HISTORY_MONTHS; month++) 
		{
			for(int cost_type = 0; cost_type < MAX_WORLD_COST; cost_type++) 
			{
				if(file->get_experimental_version() < 12 && (cost_type == WORLD_JOBS || cost_type == WORLD_VISITOR_DEMAND || cost_type == WORLD_CAR_OWNERSHIP))
				{
					finance_history_year[month][cost_type] = 0;
				}
				else
				{
					file->rdwr_longlong(finance_history_month[month][cost_type]);
				}
			}
		}
		last_month_bev = finance_history_month[1][WORLD_CITICENS];
	}

	// finally: do we run a scenario?
	if(file->get_version()>=99018) {
		scenario->rdwr(file);
	}

	// restore locked state
	// network game this will be done in nwc_sync_t::do_command
	if(  !env_t::networkmode  ) {
		for(  uint8 i=0;  i<PLAYER_UNOWNED;  i++  ) {
			if(  spieler[i]  ) {
				spieler[i]->check_unlock( player_password_hash[i] );
			}
		}
	}

	// initialize lock info for local server player
	// if call from sync command, lock info will be corrected there
	if(  env_t::server) {
		nwc_auth_player_t::init_player_lock_server(this);
	}

	if(file->get_experimental_version() >= 2)
	{
		file->rdwr_short(base_pathing_counter);
	}
	
	if((file->get_experimental_version() >= 7 && file->get_experimental_version() < 9 && file->get_version() < 110006))
	{
		double old_proportion = industry_density_proportion / 10000.0;
		file->rdwr_double(old_proportion);
		industry_density_proportion = old_proportion * 10000.0;
	}
	else if(file->get_experimental_version() >= 9 && file->get_version() >= 110006)
	{
		if(file->get_experimental_version() >= 11)
		{
			file->rdwr_long(industry_density_proportion);
		}
		else
		{
			uint32 idp = 0;
			file->rdwr_long(idp);
			idp = (idp & 0x8000) != 0 ? idp & 0x7FFF : idp * 150;
			industry_density_proportion = idp;
		}
	}
	else if(file->is_loading())
	{
		// Reconstruct the actual industry density.
		// @author: jamespetts			
		// Loading a game - must set this to zero here and recalculate.
		actual_industry_density = 0;
		uint32 weight;
		ITERATE(fab_list, i)
		{
			const fabrik_besch_t* factory_type = fab_list[i]->get_besch();
			if(!factory_type->is_electricity_producer())
			{
				// Power stations are excluded from the target weight:
				// a different system is used for them.
				weight = factory_type->get_gewichtung();
				actual_industry_density += (100 / weight);
			}
		}
		industry_density_proportion = ((sint64)actual_industry_density * 10000ll) / finance_history_month[0][WORLD_CITICENS];
	}

	if(file->get_experimental_version() >=9 && file->get_version() >= 110000)
	{
		if(file->get_experimental_version() < 11)
		{
			// Was next_private_car_update_month
			uint8 dummy;
			file->rdwr_byte(dummy);
		}
		
		// Existing values now saved in order to prevent network desyncs
		file->rdwr_long(citycar_speed_average);
		file->rdwr_bool(recheck_road_connexions);
		file->rdwr_short(generic_road_time_per_tile_city);
		file->rdwr_short(generic_road_time_per_tile_intercity);
		file->rdwr_long(max_road_check_depth);
		if(file->get_experimental_version() < 10)
		{
			double old_density = actual_industry_density / 100.0;
			file->rdwr_double(old_density);
			actual_industry_density = old_density * 100.0;
		}
		else
		{
			file->rdwr_long(actual_industry_density);
		}
		if(fab_list.empty() && file->get_version() < 111100)
		{
			// Correct some older saved games where the actual industry density was over-stated.
			actual_industry_density = 0;
		}
	}

	if(file->get_experimental_version() >= 12)
	{
		file->rdwr_long(next_step_passenger);
		file->rdwr_long(next_step_mail);
	}

	// show message about server
	if(  file->get_version() >= 112008  ) {
		xml_tag_t t( file, "motd_t" );
		char msg[32766];
		file->rdwr_str( msg, 32766 );
		if(  *msg  &&  !env_t::server  ) {
			// if not empty ...
			help_frame_t *win = new help_frame_t();
			win->set_text( msg );
			create_win(win, w_info, magic_motd);
		}
	}

	// MUST be at the end of the load/save routine.
	if(  file->get_version()>=102004  ) {
		if(  env_t::restore_UI  ) {
			file->rdwr_byte( active_player_nr );
			active_player = spieler[active_player_nr];
			/* restore all open windows
			 * otherwise it will be ignored
			 * which is save, since it is the end of file
			 */
			rdwr_all_win( file );
		}
	}

	// Check attractions' road connexions
	FOR(weighted_vector_tpl<gebaeude_t*>, const &i, ausflugsziele)
	{
		i->check_road_tiles(false);
	}

	// Added by : Knightly
	path_explorer_t::full_instant_refresh();

	file->set_buffered(false);
	clear_random_mode(LOAD_RANDOM);

	// loading finished, reset savegame version to current
	load_version = loadsave_t::int_version( env_t::savegame_version_str, NULL, NULL );

	FOR(slist_tpl<depot_t *>, const dep, depot_t::get_depot_list())
	{
		// This must be done here, as the cities have not been initialised on loading.
		dep->add_to_world_list();
	}

	dbg->warning("karte_t::load()","loaded savegame from %i/%i, next month=%i, ticks=%i (per month=1<<%i)",last_month,last_year,next_month_ticks,ticks,karte_t::ticks_per_world_month_shift);
}


// recalcs all ground tiles on the map
void karte_t::update_map_intern(sint16 x_min, sint16 x_max, sint16 y_min, sint16 y_max)
{
	if(  (loaded_rotation + settings.get_rotation()) & 1  ) {  // 1 || 3  // ~14% faster loop blocking rotations 1 and 3
		const int LOOP_BLOCK = 128;
		for(  int xx = x_min;  xx < x_max;  xx += LOOP_BLOCK  ) {
			for(  int yy = y_min;  yy < y_max;  yy += LOOP_BLOCK  ) {
				for(  int y = yy;  y < min(yy + LOOP_BLOCK, y_max);  y++  ) {
					for(  int x = xx;  x < min(xx + LOOP_BLOCK, x_max);  x++  ) {
						const int nr = y * cached_grid_size.x + x;
						for(  uint i = 0;  i < plan[nr].get_boden_count();  i++  ) {
							plan[nr].get_boden_bei(i)->calc_bild();
						}
					}
				}
			}
		}
	}
	else {
		for(  int y = y_min;  y < y_max;  y++  ) {
			for(  int x = x_min;  x < x_max;  x++  ) {
				const int nr = y * cached_grid_size.x + x;
				for(  uint i = 0;  i < plan[nr].get_boden_count();  i++  ) {
					plan[nr].get_boden_bei(i)->calc_bild();
				}
			}
		}
	}
}


// recalcs all ground tiles on the map
void karte_t::update_map()
{
	DBG_MESSAGE( "karte_t::update_map()", "" );
	world_xy_loop(&karte_t::update_map_intern, SYNCX_FLAG);
	set_dirty();
}

/**
 * return an index to a halt
 * optionally limit to that owned by player sp
 * by default create a new halt if none found
 * -- create_halt==true is used during loading of *old* saved games
 */
halthandle_t karte_t::get_halt_koord_index(koord k, spieler_t *sp, bool create_halt)
{
	halthandle_t my_halt;

	// already there?
	// check through all the grounds
	const planquadrat_t* plan = access(k);
	if (plan)
	{
		//for(  uint8 i=0;  i < plan->get_boden_count();  i++  ) {
		//	halthandle_t my_halt = plan->get_boden_bei(i)->get_halt();
		//	if(  my_halt.is_bound()  ) {
		//		// Stop at first halt found (always prefer ground level)
		//		return my_halt;
		//	}
		//}
		// for compatibility with old code (see above) we do not pass sp to get_halt():
		halthandle_t my_halt = plan->get_halt(NULL);
	}
	if( create_halt && !my_halt.is_bound() ) {
		// No halts found => create one
		my_halt = haltestelle_t::create( k, NULL );
	}
	return my_halt;
}

void karte_t::calc_climate(koord k, bool recalc)
{
	planquadrat_t *pl = access(k);
	if(  !pl  ) {
		return;
	}

	grund_t *gr = pl->get_kartenboden();
	if(  gr  ) {
		if(  !gr->ist_wasser()  ) {
			bool beach = false;
			if(  gr->get_pos().z == grundwasser  ) {
				for(  int i = 0;  i < 8 && !beach;  i++  ) {
					grund_t *gr2 = lookup_kartenboden( k + koord::neighbours[i] );
					if(  gr2 && gr2->ist_wasser()  ) {
						beach = true;
					}
				}
			}
			pl->set_climate( beach ? desert_climate : get_climate_at_height( max( gr->get_pos().z, grundwasser + 1 ) ) );
		}
		else {
			pl->set_climate( water_climate );
		}
		pl->set_climate_transition_flag(false);
		pl->set_climate_corners(0);
	}

	if(  recalc  ) {
		recalc_transitions(k);
		for(  int i = 0;  i < 8;  i++  ) {
			recalc_transitions( k + koord::neighbours[i] );
		}
	}
}


// fills array with neighbour heights
void karte_t::get_neighbour_heights(const koord k, sint8 neighbour_height[8][4]) const
{
	for(  int i = 0;  i < 8;  i++  ) { // 0 = nw, 1 = w etc.
		planquadrat_t *pl2 = access( k + koord::neighbours[i] );
		if(  pl2  ) {
			grund_t *gr2 = pl2->get_kartenboden();
			hang_t::typ slope_corner = gr2->get_grund_hang();
			for(  int j = 0;  j < 4;  j++  ) {
				neighbour_height[i][j] = gr2->get_hoehe() + slope_corner % 3;
				slope_corner /= 3;
			}
		}
		else {
			switch(i) {
				case 0: // nw
					neighbour_height[i][0] = grundwasser;
					neighbour_height[i][1] = max( lookup_hgt( k+koord(0,0) ), get_water_hgt( k ) );
					neighbour_height[i][2] = grundwasser;
					neighbour_height[i][3] = grundwasser;
				break;
				case 1: // w
					neighbour_height[i][0] = grundwasser;
					neighbour_height[i][1] = max( lookup_hgt( k+koord(0,1) ), get_water_hgt( k ) );
					neighbour_height[i][2] = max( lookup_hgt( k+koord(0,0) ), get_water_hgt( k ) );
					neighbour_height[i][3] = grundwasser;
				break;
				case 2: // sw
					neighbour_height[i][0] = grundwasser;
					neighbour_height[i][1] = grundwasser;
					neighbour_height[i][2] = max( lookup_hgt( k+koord(0,1) ), get_water_hgt( k ) );
					neighbour_height[i][3] = grundwasser;
				break;
				case 3: // s
					neighbour_height[i][0] = grundwasser;
					neighbour_height[i][1] = grundwasser;
					neighbour_height[i][2] = max( lookup_hgt( k+koord(1,1) ), get_water_hgt( k ) );
					neighbour_height[i][3] = max( lookup_hgt( k+koord(0,1) ), get_water_hgt( k ) );
				break;
				case 4: // se
					neighbour_height[i][0] = grundwasser;
					neighbour_height[i][1] = grundwasser;
					neighbour_height[i][2] = grundwasser;
					neighbour_height[i][3] = max( lookup_hgt( k+koord(1,1) ), get_water_hgt( k ) );
				break;
				case 5: // e
					neighbour_height[i][0] = max( lookup_hgt( k+koord(1,1) ), get_water_hgt( k ) );
					neighbour_height[i][1] = grundwasser;
					neighbour_height[i][2] = grundwasser;
					neighbour_height[i][3] = max( lookup_hgt( k+koord(1,0) ), get_water_hgt( k ) );
				break;
				case 6: // ne
					neighbour_height[i][0] = max( lookup_hgt( k+koord(1,0) ), get_water_hgt( k ) );
					neighbour_height[i][1] = grundwasser;
					neighbour_height[i][2] = grundwasser;
					neighbour_height[i][3] = grundwasser;
				break;
				case 7: // n
					neighbour_height[i][0] = max( lookup_hgt( k+koord(0,0) ), get_water_hgt( k ) );
					neighbour_height[i][1] = max( lookup_hgt( k+koord(1,0) ), get_water_hgt( k ) );
					neighbour_height[i][2] = grundwasser;
					neighbour_height[i][3] = grundwasser;
				break;
			}

			/*neighbour_height[i][0] = grundwasser;
			neighbour_height[i][1] = grundwasser;
			neighbour_height[i][2] = grundwasser;
			neighbour_height[i][3] = grundwasser;*/
		}
	}
}


void karte_t::rotate_transitions(koord k)
{
	planquadrat_t *pl = access(k);
	if(  !pl  ) {
		return;
	}

	uint8 climate_corners = pl->get_climate_corners();
	if(  climate_corners != 0  ) {
		climate_corners = (climate_corners >> 1) | ((climate_corners & 1) << 3);
		pl->set_climate_corners( climate_corners );
	}
}


void karte_t::recalc_transitions_loop( sint16 x_min, sint16 x_max, sint16 y_min, sint16 y_max )
{
	for(  int y = y_min;  y < y_max;  y++  ) {
		for(  int x = x_min; x < x_max;  x++  ) {
			recalc_transitions( koord( x, y ) );
		}
	}
}


void karte_t::recalc_transitions(koord k)
{
	planquadrat_t *pl = access(k);
	if(  !pl  ) {
		return;
	}

	grund_t *gr = pl->get_kartenboden();
	if(  !gr->ist_wasser()  ) {
		// get neighbour corner heights
		sint8 neighbour_height[8][4];
		get_neighbour_heights( k, neighbour_height );

		// look up neighbouring climates
		climate neighbour_climate[8];
		for(  int i = 0;  i < 8;  i++  ) { // 0 = nw, 1 = w etc.
			koord k_neighbour = k + koord::neighbours[i];
			if(  !is_within_limits(k_neighbour)  ) {
				k_neighbour = get_closest_coordinate(k_neighbour);
			}
			neighbour_climate[i] = get_climate( k_neighbour );
		}

		uint8 climate_corners = 0;
		climate climate0 = get_climate(k);

		hang_t::typ slope_corner = gr->get_grund_hang();
		for(  uint8 i = 0;  i < 4;  i++  ) { // 0 = sw, 1 = se etc.
			// corner1 (i=0): tests vs neighbour 1:w (corner 2 j=1),2:sw (corner 3) and 3:s (corner 4)
			// corner2 (i=1): tests vs neighbour 3:s (corner 3 j=2),4:se (corner 4) and 5:e (corner 1)
			// corner3 (i=2): tests vs neighbour 5:e (corner 4 j=3),6:ne (corner 1) and 7:n (corner 2)
			// corner4 (i=3): tests vs neighbour 7:n (corner 1 j=0),0:nw (corner 2) and 1:w (corner 3)
			sint8 corner_height = gr->get_hoehe() + slope_corner % 3;

			climate transition_climate = water_climate;
			climate min_climate = arctic_climate;

			for(  int j = 1;  j < 4;  j++  ) {
				if(  corner_height == neighbour_height[(i * 2 + j) & 7][(i + j) & 3]) {
					climate climatej = neighbour_climate[(i * 2 + j) & 7];
					climatej > transition_climate ? transition_climate = climatej : 0;
					climatej < min_climate ? min_climate = climatej : 0;
				}
			}

			if(  min_climate == water_climate  ||  transition_climate > climate0  ) {
				climate_corners |= 1 << i;
			}
			slope_corner /= 3;
		}
		pl->set_climate_transition_flag( climate_corners != 0 );
		pl->set_climate_corners( climate_corners );
	}
	gr->calc_bild();
}


void karte_t::create_grounds_loop( sint16 x_min, sint16 x_max, sint16 y_min, sint16 y_max )
{
	for(  int y = y_min;  y < y_max;  y++  ) {
		for(  int x = x_min; x < x_max;  x++  ) {
			koord k(x,y);
			access_nocheck(k)->kartenboden_setzen( new boden_t( koord3d( x, y, max(min_hgt_nocheck(k),get_water_hgt_nocheck(k)) ), 0 ) );
		}
	}
}


uint8 karte_t::sp2num(spieler_t *sp)
{
	if(  sp==NULL  ) {
		return PLAYER_UNOWNED;
	}
	for(int i=0; i<MAX_PLAYER_COUNT; i++) {
		if(spieler[i] == sp) {
			return i;
		}
	}
	dbg->fatal( "karte_t::sp2num()", "called with an invalid player!" );
}


void karte_t::load_heightfield(settings_t* const sets)
{
	sint16 w, h;
	sint8 *h_field;
	if(karte_t::get_height_data_from_file(sets->heightfield.c_str(), (sint8)(sets->get_grundwasser()), h_field, w, h, false )) {
		sets->set_groesse(w,h);
		// create map
		init(sets,h_field);
		delete [] h_field;
	}
	else {
		dbg->error("karte_t::load_heightfield()","Cant open file '%s'", sets->heightfield.c_str());
		create_win( new news_img("\nCan't open heightfield file.\n"), w_info, magic_none );
	}
}


void karte_t::mark_area( const koord3d pos, const koord size, const bool mark ) const
{
	for( sint16 y=pos.y;  y<pos.y+size.y;  y++  ) {
		for( sint16 x=pos.x;  x<pos.x+size.x;  x++  ) {
			grund_t *gr = lookup( koord3d(x,y,pos.z));
			if (!gr) {
				gr = lookup_kartenboden( x,y );
			}
			if(gr) {
				if(mark) {
					gr->set_flag(grund_t::marked);
				}
				else {
					gr->clear_flag(grund_t::marked);
				}
				gr->set_flag(grund_t::dirty);
			}
		}
	}
}


void karte_t::reset_timer()
{
	// Reset timers
	long last_tick_sync = dr_time();
	mouse_rest_time = last_tick_sync;
	sound_wait_time = AMBIENT_SOUND_INTERVALL;
	intr_set_last_time(last_tick_sync);

	if(  env_t::networkmode  &&  (step_mode&PAUSE_FLAG)==0  ) {
		step_mode = FIX_RATIO;
	}

	last_step_time = last_interaction = last_tick_sync;
	last_step_ticks = ticks;

	// reinit simloop counter
	for(  int i=0;  i<32;  i++  ) {
		last_step_nr[i] = steps;
	}

	if(  step_mode&PAUSE_FLAG  ) {
		intr_disable();
	}
	else if(step_mode==FAST_FORWARD) {
		next_step_time = last_tick_sync+1;
		idle_time = 0;
		set_frame_time( 100 );
		time_multiplier = 16;
		intr_enable();
	}
	else if(step_mode==FIX_RATIO) {
		last_frame_idx = 0;
		fix_ratio_frame_time = 1000 / clamp(settings.get_frames_per_second(), 5, 100);
		next_step_time = last_tick_sync + fix_ratio_frame_time;
		set_frame_time( fix_ratio_frame_time );
		intr_disable();
		// other stuff needed to synchronize
		tile_counter = 0;
		pending_season_change = 1;
	}
	else {
		// make timer loop invalid
		for( int i=0;  i<32;  i++ ) {
			last_frame_ms[i] = 0x7FFFFFFFu;
			last_step_nr[i] = 0xFFFFFFFFu;
		}
		last_frame_idx = 0;
		simloops = 60;

		set_frame_time( 1000/env_t::fps );
		next_step_time = last_tick_sync+(3200/get_time_multiplier() );
		intr_enable();
	}
	DBG_MESSAGE("karte_t::reset_timer()","called, mode=$%X", step_mode);
}


void karte_t::reset_interaction()
{
	last_interaction = dr_time();
}


void karte_t::set_map_counter(uint32 new_map_counter)
{
	map_counter = new_map_counter;
	if(  env_t::server  ) {
		nwc_ready_t::append_map_counter(map_counter);
	}
}


uint32 karte_t::generate_new_map_counter() const
{
	return (uint32)dr_time();
}


// jump one year ahead
// (not updating history!)
void karte_t::step_year()
{
	DBG_MESSAGE("karte_t::step_year()","called");
//	ticks += 12*karte_t::ticks_per_world_month;
//	next_month_ticks += 12*karte_t::ticks_per_world_month;
	current_month += 12;
	last_year ++;
	reset_timer();
	recalc_average_speed();
}


// jump one or more months ahead
// (updating history!)
void karte_t::step_month( sint16 months )
{
	while(  months-->0  ) {
		new_month();
	}
	reset_timer();
}


void karte_t::change_time_multiplier(sint32 delta)
{
	time_multiplier += delta;
	if(time_multiplier<=0) {
		time_multiplier = 1;
	}
	if(step_mode!=NORMAL) {
		step_mode = NORMAL;
		reset_timer();
	}
}


void karte_t::set_pause(bool p)
{
	bool pause = step_mode&PAUSE_FLAG;
	if(p!=pause) {
		step_mode ^= PAUSE_FLAG;
		if(p) {
			intr_disable();
		}
		else {
			reset_timer();
		}
	}
}


void karte_t::set_fast_forward(bool ff)
{
	if(  !env_t::networkmode  ) {
		if(  ff  ) {
			if(  step_mode==NORMAL  ) {
				step_mode = FAST_FORWARD;
				reset_timer();
			}
		}
		else {
			if(  step_mode==FAST_FORWARD  ) {
				step_mode = NORMAL;
				reset_timer();
			}
		}
	}
}


koord karte_t::get_closest_coordinate(koord outside_pos)
{
	outside_pos.clip_min(koord(0,0));
	outside_pos.clip_max(koord(get_size().x-1,get_size().y-1));

	return outside_pos;
}


/* creates a new player with this type */
const char *karte_t::new_spieler(uint8 new_player, uint8 type)
{
	if(  new_player>=PLAYER_UNOWNED  ||  get_spieler(new_player)!=NULL  ) {
		return "Id invalid/already in use!";
	}
	switch( type ) {
		case spieler_t::EMPTY: break;
		case spieler_t::HUMAN: spieler[new_player] = new spieler_t(this,new_player); break;
		case spieler_t::AI_GOODS: spieler[new_player] = new ai_goods_t(this,new_player); break;
		case spieler_t::AI_PASSENGER: spieler[new_player] = new ai_passenger_t(this,new_player); break;
		default: return "Unknow AI type!";
	}
	settings.set_player_type(new_player, type);
	return NULL;
}


void karte_t::remove_player(uint8 player_nr)
{
	if ( player_nr!=1  &&  player_nr<PLAYER_UNOWNED  &&  spieler[player_nr]!=NULL) {
		spieler[player_nr]->ai_bankrupt();
		delete spieler[player_nr];
		spieler[player_nr] = 0;
		nwc_chg_player_t::company_removed(player_nr);
		// if default human, create new instace of it (to avoid crashes)
		if(  player_nr == 0  ) {
			spieler[0] = new spieler_t( this, 0 );
		}

		// Reset all access rights
		for(sint32 i = 0; i < MAX_PLAYER_COUNT; i++)
		{
			if(spieler[i] != NULL && i != player_nr)
			{
				spieler[i]->set_allow_access_to(player_nr, i == 1); // Public player (no. 1) allows access by default, others do not allow by default.
			}
		}

		// if currently still active => reset to default human
		if(  player_nr == active_player_nr  ) {
			active_player_nr = 0;
			active_player = spieler[0];
			if(  !env_t::server  ) {
				create_win( display_get_width()/2-128, 40, new news_img("Bankrott:\n\nDu bist bankrott.\n"), w_info, magic_none);
			}
		}
	}
}


/* goes to next active player */
void karte_t::switch_active_player(uint8 new_player, bool silent)
{
	for(  uint8 i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
		if(  spieler[(i+new_player)%MAX_PLAYER_COUNT] != NULL  ) {
			new_player = (i+new_player)%MAX_PLAYER_COUNT;
			break;
		}
	}
	koord3d old_zeiger_pos = zeiger->get_pos();

	// no cheating allowed?
	if (!settings.get_allow_player_change() && spieler[1]->is_locked()) {
		active_player_nr = 0;
		active_player = spieler[0];
		if(new_player!=0) {
			create_win( new news_img("On this map, you are not\nallowed to change player!\n"), w_time_delete, magic_none);
		}
	}
	else {
		zeiger->change_pos( koord3d::invalid ); // unmark area
		// exit active tool to remove pointers (for two_click_tool_t's, stop mover, factory linker)
		if(werkzeug[active_player_nr]) {
			werkzeug[active_player_nr]->exit(active_player);
		}
		active_player_nr = new_player;
		active_player = spieler[new_player];
		if(  !silent  ) {
			// tell the player
			cbuffer_t buf;
			buf.printf( translator::translate("Now active as %s.\n"), get_active_player()->get_name() );
			msg->add_message(buf, koord::invalid, message_t::ai | message_t::local_flag, PLAYER_FLAG|get_active_player()->get_player_nr(), IMG_LEER);
		}

		// update menue entries
		werkzeug_t::update_toolbars();
		set_dirty();
	}

	// update pointer image / area
	werkzeug[active_player_nr]->init_cursor(zeiger);
	// set position / mark area
	zeiger->change_pos( old_zeiger_pos );
}


void karte_t::stop(bool exit_game)
{
	finish_loop = true;
	env_t::quit_simutrans = exit_game;
}


void karte_t::network_game_set_pause(bool pause_, uint32 syncsteps_)
{
	if (env_t::networkmode) {
		time_multiplier = 16;	// reset to normal speed
		sync_steps = syncsteps_;
		steps = sync_steps / settings.get_frames_per_step();
		network_frame_count = sync_steps % settings.get_frames_per_step();
		dbg->warning("karte_t::network_game_set_pause", "steps=%d sync_steps=%d pause=%d", steps, sync_steps, pause_);
		if (pause_) {
			if (!env_t::server) {
				reset_timer();
				step_mode = PAUSE_FLAG|FIX_RATIO;
			}
			else {
				// TODO
			}
		}
		else {
			step_mode = FIX_RATIO;
			if (!env_t::server) {
				/* make sure, the server is really that far ahead
				 * Sleep() on windows often returns before!
				 */
				unsigned long const ms = dr_time() + (settings.get_server_frames_ahead() + (uint32)env_t::additional_client_frames_behind) * fix_ratio_frame_time;
				while(  dr_time()<ms  ) {
					dr_sleep ( 10 );
				}
			}
			reset_timer();
		}
	}
	else {
		set_pause(pause_);
	}
}


static slist_tpl<network_world_command_t*> command_queue;

void karte_t::command_queue_append(network_world_command_t* nwc) const
{
	slist_tpl<network_world_command_t*>::iterator i = command_queue.begin();
	slist_tpl<network_world_command_t*>::iterator end = command_queue.end();
	while(i != end  &&  network_world_command_t::cmp(*i, nwc)) {
		++i;
	}
	command_queue.insert(i, nwc);
}


void karte_t::clear_command_queue() const
{
	while (!command_queue.empty()) {
		delete command_queue.remove_first();
	}
}


static void encode_URI(cbuffer_t& buf, char const* const text)
{
	for (char const* i = text; *i != '\0'; ++i) {
		char const c = *i;
		if (('A' <= c && c <= 'Z') ||
				('a' <= c && c <= 'z') ||
				('0' <= c && c <= '9') ||
				c == '-' || c == '.' || c == '_' || c == '~') {
			char const two[] = { c, '\0' };
			buf.append(two);
		} else {
			buf.printf("%%%02X", (unsigned char)c);
		}
	}
}


void karte_t::process_network_commands(sint32 *ms_difference)
{
	// did we receive a new command?
	unsigned long ms = dr_time();
	network_command_t *nwc = network_check_activity( this, next_step_time>ms ? min( next_step_time-ms, 5) : 0 );
	if(  nwc==NULL  &&  !network_check_server_connection()  ) {
		dbg->warning("karte_t::process_network_commands", "lost connection to server");
		network_disconnect();
		return;
	}

	// process the received command
	while (nwc) {
		// check timing
		if (nwc->get_id()==NWC_CHECK) {
			// checking for synchronisation
			nwc_check_t* nwcheck = (nwc_check_t*)nwc;
			// are we on time?
			*ms_difference = 0;
			sint64 const difftime = (sint64)next_step_time - dr_time() + ((sint64)nwcheck->server_sync_step - sync_steps - settings.get_server_frames_ahead() - env_t::additional_client_frames_behind) * fix_ratio_frame_time;
			if(  difftime<0  ) {
				// running ahead
				next_step_time += (uint32)(-difftime);
			}
			else {
				// more gentle catching up
				*ms_difference = (sint32)difftime;
			}
			dbg->message("NWC_CHECK","time difference to server %lli",difftime);
		}
		// check random number generator states
		if(  env_t::server  &&  nwc->get_id()==NWC_TOOL  ) {
			nwc_tool_t *nwt = dynamic_cast<nwc_tool_t *>(nwc);
			if(  nwt->is_from_initiator()  ) {
				if(  nwt->last_sync_step>sync_steps  ) {
					dbg->warning("karte_t::process_network_commands", "client was too fast (skipping command)" );
					delete nwc;
					nwc = NULL;
				}
				// out of sync => drop client (but we can only compare if nwt->last_sync_step is not too old)
				else if(  is_checklist_available(nwt->last_sync_step)  &&  LCHKLST(nwt->last_sync_step)!=nwt->last_checklist  ) {
					// lost synchronisation -> server kicks client out actively
					char buf[2048];
					const int offset = LCHKLST(nwt->last_sync_step).print(buf, "server");
					assert(offset < 2048);
					const int offset2 = offset + nwt->last_checklist.print(buf + offset, "initiator");
					assert(offset2 < 2048);
					dbg->warning("karte_t::process_network_commands", "kicking client due to checklist mismatch : sync_step=%u %s", nwt->last_sync_step, buf);
					socket_list_t::remove_client( nwc->get_sender() );
					delete nwc;
					nwc = NULL;
				}
			}
		}

		// execute command, append to command queue if necessary
		if(nwc  &&  nwc->execute(this)) {
			// network_world_command_t's will be appended to command queue in execute
			// all others have to be deleted here
			delete nwc;

		}
		// fetch the next command
		nwc = network_get_received_command();
	}
	uint32 next_command_step = get_next_command_step();

	// send data
	ms = dr_time();
	network_process_send_queues( next_step_time>ms ? min( next_step_time-ms, 5) : 0 );

	// process enqueued network world commands
	while(  !command_queue.empty()  &&  (next_command_step<=sync_steps/*  ||  step_mode&PAUSE_FLAG*/)  ) {
		network_world_command_t *nwc = command_queue.remove_first();
		if (nwc) {
			do_network_world_command(nwc);
			delete nwc;
		}
		next_command_step = get_next_command_step();
	}
}

void karte_t::do_network_world_command(network_world_command_t *nwc)
{
	// want to execute something in the past?
	if (nwc->get_sync_step() < sync_steps) {
		if (!nwc->ignore_old_events()) {
			dbg->warning("karte_t:::do_network_world_command", "wanted to do_command(%d) in the past", nwc->get_id());
			network_disconnect();
		}
	}
	// check map counter
	else if (nwc->get_map_counter() != map_counter) {
		dbg->warning("karte_t:::do_network_world_command", "wanted to do_command(%d) from another world", nwc->get_id());
	}
	// check random counter?
	else if(  nwc->get_id()==NWC_CHECK  ) {
		nwc_check_t* nwcheck = (nwc_check_t*)nwc;
		// this was the random number at the previous sync step on the server
		const checklist_t &server_checklist = nwcheck->server_checklist;
		const uint32 server_sync_step = nwcheck->server_sync_step;
		char buf[2048];
		const int offset = server_checklist.print(buf, "server");
		assert(offset < 2048);
		const int offset2 = offset + LCHKLST(server_sync_step).print(buf + offset, "client");
		assert(offset2 < 2048);
		dbg->warning("karte_t:::do_network_world_command", "sync_step=%u  %s", server_sync_step, buf);
		if(  LCHKLST(server_sync_step)!=server_checklist  ) {
			dbg->warning("karte_t:::do_network_world_command", "disconnecting due to checklist mismatch" );
			network_disconnect();
		}
	}
	else {
		if(  nwc->get_id()==NWC_TOOL  ) {
			nwc_tool_t *nwt = dynamic_cast<nwc_tool_t *>(nwc);
			if(  is_checklist_available(nwt->last_sync_step)  &&  LCHKLST(nwt->last_sync_step)!=nwt->last_checklist  ) {
				// lost synchronisation ...
				char buf[2048];
				const int offset = nwt->last_checklist.print(buf, "server");
				assert(offset < 2048);
				const int offset2 = offset + LCHKLST(nwt->last_sync_step).print(buf + offset, "executor");
				assert(offset2 < 2048);
				dbg->warning("karte_t:::do_network_world_command", "skipping command due to checklist mismatch : sync_step=%u %s", nwt->last_sync_step, buf);
				if(  !env_t::server  ) {
					network_disconnect();
				}
				delete nwc;
				return;
			}
		}
		nwc->do_command(this);
	}
}

uint32 karte_t::get_next_command_step()
{
	// when execute next command?
	if(  !command_queue.empty()  ) {
		return command_queue.front()->get_sync_step();
	}
	else {
		return 0xFFFFFFFFu;
	}
}

sint16 karte_t::get_sound_id(grund_t *gr)
{
	if(  gr->ist_natur()  ||  gr->ist_wasser()  ) {
		sint16 id = NO_SOUND;
		if(  gr->get_pos().z >= get_snowline()  ) {
			id = sound_besch_t::climate_sounds[ arctic_climate ];
		}
		else {
			id = sound_besch_t::climate_sounds[get_climate( zeiger->get_pos().get_2d() )];
		}
		if (id != NO_SOUND) {
			return id;
		}
		// try, if there is another sound ready
		if(  zeiger->get_pos().z==grundwasser  &&  !gr->ist_wasser()  ) {
			return sound_besch_t::beach_sound;
		}
		else if(  gr->get_top()>0  &&  gr->obj_bei(0)->get_typ()==obj_t::baum  ) {
			return sound_besch_t::forest_sound;
		}
	}
	return NO_SOUND;
}


bool karte_t::interactive(uint32 quit_month)
{

	finish_loop = false;
	sync_steps = 0;

	network_frame_count = 0;
	vector_tpl<uint16>hashes_ok;	// bit set: this client can do something with this player

	if(  !scenario->rdwr_ok()  ) {
		// error during loading of savegame of scenario
		create_win( new news_img( scenario->get_error_text() ), w_info, magic_none);
		scenario->stop();
	}
	// only needed for network
	if(  env_t::networkmode  ) {
		// clear the checklist history
		for(  int i=0;  i<LAST_CHECKLISTS_COUNT;  ++i  ) {
			last_checklists[i] = checklist_t();
		}
	}
	sint32 ms_difference = 0;
	reset_timer();
	DBG_DEBUG4("karte_t::interactive", "welcome in this routine");

	if(  env_t::server  ) {
		step_mode |= FIX_RATIO;

		reset_timer();
		// Announce server startup to the listing server
		if(  env_t::server_announce  ) {
			announce_server( 0 );
		}
	}

	DBG_DEBUG4("karte_t::interactive", "start the loop");
	do {
		// check for too much time eaten by frame updates ...
		if(  step_mode==NORMAL  ) {
			DBG_DEBUG4("karte_t::interactive", "decide to play a sound");
			last_interaction = dr_time();
			if(  mouse_rest_time+sound_wait_time < last_interaction  ) {
				// we play an ambient sound, if enabled
				grund_t *gr = lookup(zeiger->get_pos());
				if(  gr  ) {
					sint16 id = get_sound_id(gr);
					if(  id!=NO_SOUND  ) {
						sound_play(id);
					}
				}
				sound_wait_time *= 2;
			}
			DBG_DEBUG4("karte_t::interactive", "end of sound");
		}

		// check events queued since our last iteration
		eventmanager->check_events();

		if (env_t::quit_simutrans){
			break;
		}
#ifdef DEBUG_SIMRAND_CALLS
		station_check("karte_t::interactive after win_poll_event", this);
#endif

		if(  env_t::networkmode  ) {
			process_network_commands(&ms_difference);

		}
		else {
			// we wait here for maximum 9ms
			// average is 5 ms, so we usually
			// are quite responsive
			DBG_DEBUG4("karte_t::interactive", "can I get some sleep?");
			INT_CHECK( "karte_t::interactive()" );
			const sint32 wait_time = (sint32)(next_step_time-dr_time());
			if(wait_time>0) {
				if(wait_time<10  ) {
					dr_sleep( wait_time );
				}
				else {
					dr_sleep( 9 );
				}
				INT_CHECK( "karte_t::interactive()" );
			}
			DBG_DEBUG4("karte_t::interactive", "end of sleep");
		}

		// time for the next step?
		uint32 time = dr_time(); // - (env_t::server ? 0 : 5000);
		if(  next_step_time<=time  ) {
			if(  step_mode&PAUSE_FLAG  ) {
				// only update display
				sync_step( 0, false, true );
#ifdef DEBUG_SIMRAND_CALLS
				station_check("karte_t::interactive PAUSE after sync_step", this);
#endif
				idle_time = 100;
			}
			else {
				if(  step_mode==FAST_FORWARD  ) {
					sync_step( 100, true, false );
					set_random_mode( STEP_RANDOM );
					step();
					clear_random_mode( STEP_RANDOM );
#ifdef DEBUG_SIMRAND_CALLS
					station_check("karte_t::interactive FAST_FORWARD after step", this);
#endif
				}
				else if(  step_mode==FIX_RATIO  ) {
					next_step_time += fix_ratio_frame_time;
					if(  ms_difference>5  ) {
						next_step_time -= 5;
						ms_difference -= 5;
					}
					else if(  ms_difference<-5  ) {
						next_step_time += 5;
						ms_difference += 5;
					}
					sync_step( (fix_ratio_frame_time*time_multiplier)/16, true, true );
#ifdef DEBUG_SIMRAND_CALLS
					station_check("karte_t::interactive FIX_RATIO after sync_step", this);
#endif
					if (++network_frame_count == settings.get_frames_per_step()) {
						// ever fourth frame
						set_random_mode( STEP_RANDOM );
						step();
#ifdef DEBUG_SIMRAND_CALLS
					station_check("karte_t::interactive FIX_RATIO after step", this);
#endif
						clear_random_mode( STEP_RANDOM );
						network_frame_count = 0;
					}
					sync_steps = steps * settings.get_frames_per_step() + network_frame_count;
					LCHKLST(sync_steps) = checklist_t(sync_steps, (uint32)steps, network_frame_count, get_random_seed(), halthandle_t::get_next_check(), linehandle_t::get_next_check(), convoihandle_t::get_next_check(),
						rands
					);

#ifdef DEBUG_SIMRAND_CALLS
					char buf[2048];
					const int offset = LCHKLST(sync_steps).print(buf, "chklist");
					assert(offset<2048);
					dbg->warning("karte_t::interactive", "sync_step=%u  %s", sync_steps, buf);
#endif

					// some serverside tasks
					if(  env_t::networkmode  &&  env_t::server  ) {
						// broadcast sync info
						if (  (network_frame_count==0  &&  (sint64)dr_time()-(sint64)next_step_time>fix_ratio_frame_time*2)
								||  (sync_steps % env_t::server_sync_steps_between_checks)==0  ) {
							nwc_check_t* nwc = new nwc_check_t(sync_steps + 1, map_counter, LCHKLST(sync_steps), sync_steps);
							network_send_all(nwc, true);
						}
					}
#if DEBUG>4
					if(  env_t::networkmode  &&  (sync_steps & 7)==0  &&  env_t::verbose_debug>4  ) {
						dbg->message("karte_t::interactive", "time=%lu sync=%d"/*  rand=%d"*/, dr_time(), sync_steps/*, LRAND(sync_steps)*/);
					}
#endif

					// no clients -> pause game
					if (  env_t::networkmode  &&  env_t::pause_server_no_clients  &&  socket_list_t::get_playing_clients() == 0  &&  !nwc_join_t::is_pending()  ) {
						set_pause(true);
					}
				}
				else {
					INT_CHECK( "karte_t::interactive()" );
#ifdef DEBUG_SIMRAND_CALLS
					station_check("karte_t::interactive else after INT_CHECK 1", this);
#endif
					set_random_mode( STEP_RANDOM );
					step();
#ifdef DEBUG_SIMRAND_CALLS
					station_check("karte_t::interactive else after step", this);
#endif
					clear_random_mode( STEP_RANDOM );
					idle_time = ((idle_time*7) + next_step_time - dr_time())/8;
					INT_CHECK( "karte_t::interactive()" );
				}
			}
		}

		// Interval-based server announcements
		if (  env_t::server  &&  env_t::server_announce  &&  env_t::server_announce_interval > 0  &&
			dr_time() >= server_last_announce_time + (uint32)env_t::server_announce_interval * 1000  ) {
			announce_server( 1 );
		}

		DBG_DEBUG4("karte_t::interactive", "point of loop return");
	} while(!finish_loop  &&  get_current_month()<quit_month);

	if(  get_current_month() >= quit_month  ) {
		env_t::quit_simutrans = true;
	}

	// On quit announce server as being offline
	if(  env_t::server  &&  env_t::server_announce  ) {
		announce_server( 2 );
	}

	intr_enable();
	display_show_pointer(true);
	return finish_loop;
#undef LRAND
}


// Announce server to central listing server
// Status is one of:
// 0 - startup
// 1 - interval
// 2 - shutdown
void karte_t::announce_server(int status)
{
	assert(env_t::server  &&  env_t::server_announce);
	DBG_DEBUG( "announce_server()", "status: %i",  status );
	// Announce game info to server, format is:
	// st=on&dns=server.com&port=13353&rev=1234&pak=pak128&name=some+name&time=3,1923&size=256,256&active=[0-16]&locked=[0-16]&clients=[0-16]&towns=15&citizens=3245&factories=33&convoys=56&stops=17
	// (This is the data part of an HTTP POST)
	if(  env_t::server_announce  ) {
		cbuffer_t buf;
		// Always send dns and port as these are used as the unique identifier for the server
		buf.append( "&dns=" );
		encode_URI( buf, env_t::server_dns.c_str() );
		buf.printf( "&port=%u", env_t::server );
		// Always send announce interval to allow listing server to predict next announce
		buf.printf( "&aiv=%u", env_t::server_announce_interval );
		// Always send status, either online or offline
		if (  status == 0  ||  status == 1  ) {
			buf.append( "&st=1" );
		}
		else {
			buf.append( "&st=0" );
		}


		// Add fields sent only on server startup (cannot change during the course of a game)
		if (  status == 0  ) {
#ifndef REVISION
#	define REVISION 0
#endif
			// Simple revision used for matching (integer)
			buf.printf( "&rev=%d", atol( QUOTEME(REVISION) ) );
			// Complex version string used for display
			buf.printf( "&ver=Simutrans %s (r%s) built %s", QUOTEME(VERSION_NUMBER), QUOTEME(REVISION), QUOTEME(VERSION_DATE) );
			// Pakset version
			buf.append( "&pak=" );
			// Announce pak set, ideally get this from the copyright field of ground.Outside.pak
			char const* const copyright = grund_besch_t::ausserhalb->get_copyright();
			if (copyright && STRICMP("none", copyright) != 0) {
				// construct from outside object copyright string
				encode_URI( buf, copyright );
			}
			else {
				// construct from pak name
				std::string pak_name = env_t::objfilename;
				pak_name.erase( pak_name.length() - 1 );
				encode_URI( buf, pak_name.c_str() );
			}
			// TODO - change this to be the start date of the current map
			buf.printf( "&start=%u,%u", settings.get_starting_month() + 1, settings.get_starting_year() );
			// Add server name for listing
			buf.append( "&name=" );
			encode_URI( buf, env_t::server_name.c_str() );
			// Add server comments for listing
			buf.append( "&comments=" );
			encode_URI( buf, env_t::server_comments.c_str() );
			// Add server maintainer email for listing
			buf.append( "&email=" );
			encode_URI( buf, env_t::server_email.c_str() );
			// Add server pakset URL for listing
			buf.append( "&pakurl=" );
			encode_URI( buf, env_t::server_pakurl.c_str() );
			// Add server info URL for listing
			buf.append( "&infurl=" );
			encode_URI( buf, env_t::server_infurl.c_str() );

			// TODO send minimap data as well							// TODO
		}
		if (  status == 0  ||  status == 1  ) {
			// Now add the game data part
			uint8 active = 0, locked = 0;
			for(  uint8 i=0;  i<MAX_PLAYER_COUNT;  i++  ) {
				if(  spieler[i]  &&  spieler[i]->get_ai_id()!=spieler_t::EMPTY  ) {
					active ++;
					if(  spieler[i]->is_locked()  ) {
						locked ++;
					}
				}
			}
			buf.printf( "&time=%u,%u",   (get_current_month() % 12) + 1, get_current_month() / 12 );
			buf.printf( "&size=%u,%u",   get_size().x, get_size().y );
			buf.printf( "&active=%u",    active );
			buf.printf( "&locked=%u",    locked );
			buf.printf( "&clients=%u",   socket_list_t::get_playing_clients() );
			buf.printf( "&towns=%u",     stadt.get_count() );
			buf.printf( "&citizens=%u",  stadt.get_sum_weight() );
			buf.printf( "&factories=%u", fab_list.get_count() );
			buf.printf( "&convoys=%u",   convoys().get_count());
			buf.printf( "&stops=%u",     haltestelle_t::get_alle_haltestellen().get_count() );
		}

		network_http_post( ANNOUNCE_SERVER, ANNOUNCE_URL, buf, NULL );

		// Record time of this announce
		server_last_announce_time = dr_time();
	}
}


void karte_t::network_disconnect()
{
	// force disconnect
	dbg->warning("karte_t::network_disconnect()", "Lost synchronisation with server. Random flags: %d", get_random_mode());
	network_core_shutdown();
	destroy_all_win(true);

	clear_random_mode( INTERACTIVE_RANDOM );
	step_mode = NORMAL;
	reset_timer();
	clear_command_queue();
	create_win( display_get_width()/2-128, 40, new news_img("Lost synchronisation\nwith server."), w_info, magic_none);
	ticker::add_msg( translator::translate("Lost synchronisation\nwith server."), koord::invalid, COL_BLACK );
	last_active_player_nr = active_player_nr;

	stop(false);

#ifdef DEBUG_SIMRAND_CALLS
	print_randoms = false;
	printf("Lost synchronisation\nwith server.\n");
	ITERATE(karte_t::random_callers, n)
	{
		get_message()->add_message(random_callers.get_element(n), koord::invalid, message_t::ai);
	}
#endif
}

void karte_t::set_citycar_speed_average()
{
	if(stadtauto_t::table.empty())
	{
		// No city cars - use default speed.
		citycar_speed_average = 50;
		return;
	}
	sint32 vehicle_speed_sum = 0;
	sint32 count = 0;
	FOR(stringhashtable_tpl<const stadtauto_besch_t *>, const& iter, stadtauto_t::table)
	{
		// Take into account the *chance* of vehicles, too: fewer people have sports cars than Minis. 
		vehicle_speed_sum += (speed_to_kmh(iter.value->get_geschw())) * iter.value->get_gewichtung();
		count += iter.value->get_gewichtung();
	}
	citycar_speed_average = vehicle_speed_sum / count;
}

void karte_t::calc_generic_road_time_per_tile_intercity()
{
	// This method is used only when private car connexion
	// checking is turned off.
	
	// Adapted from the method used to build city roads in the first place, written by Hajo.
	const weg_besch_t* besch = settings.get_intercity_road_type(get_timeline_year_month());
	if(besch == NULL) 
	{
		// Hajo: try some default (might happen with timeline ... )
		besch = wegbauer_t::weg_search(road_wt, 80, get_timeline_year_month(),weg_t::type_flat);
	}
	generic_road_time_per_tile_intercity = (uint16)calc_generic_road_time_per_tile(besch);
}

sint32 karte_t::calc_generic_road_time_per_tile(const weg_besch_t* besch)
{
	sint32 speed_average = citycar_speed_average;
	if(besch)
	{
		const sint32 road_speed_limit = besch->get_topspeed();
		if (speed_average > road_speed_limit)
		{
			speed_average = road_speed_limit;
		}
	}
	else if(city_road)
	{
		const sint32 road_speed_limit = city_road->get_topspeed();
		if (speed_average > road_speed_limit)
		{
			speed_average = road_speed_limit;
		}
	}

	// Reduce by 1/3 to reflect the fact that vehicles will not always
	// be able to maintain maximum speed even in uncongested environs,
	// and the fact that we are converting route distances to straight
	// line distances.
	speed_average *= 2;
	speed_average /= 3; 

	if(speed_average == 0)
	{
		speed_average = 1;
	}
	
	return ((600 / speed_average) * settings.get_meters_per_tile()) / 100;
}

void karte_t::calc_max_road_check_depth()
{
	sint32 max_road_speed = 0;
	stringhashtable_tpl <weg_besch_t *> * ways = wegbauer_t::get_all_ways();

	if(ways != NULL)
	{
		FOR(stringhashtable_tpl <weg_besch_t *>, const& iter, *ways)
		{
			if(iter.value->get_wtyp() != road_wt || iter.value->get_intro_year_month() > current_month || iter.value->get_retire_year_month() > current_month)
			{
				continue;
			}
			if(iter.value->get_topspeed() > max_road_speed)
			{
				max_road_speed = iter.value->get_topspeed();
			}
		}
		if(max_road_speed == 0)
		{
			max_road_speed = citycar_speed_average;
		}
	}
	else
	{
		max_road_speed = citycar_speed_average;
	}

	// unit of max_road_check_depth: (min/10 * 100) / (m/tile * 6) * km/h  --> tile * 1000 / 36
	max_road_check_depth = ((uint32)settings.get_range_visiting_tolerance() * 100) / (settings.get_meters_per_tile() * 6) * min(citycar_speed_average, max_road_speed);
}

static bool sort_ware_by_name(const ware_besch_t* a, const ware_besch_t* b)
{
	int diff = strcmp(translator::translate(a->get_name()), translator::translate(b->get_name()));
	return diff < 0;
}


// Returns a list of goods produced by factories that exist in current game
const vector_tpl<const ware_besch_t*> &karte_t::get_goods_list()
{
	if (goods_in_game.empty()) {
		// Goods list needs to be rebuilt

		// Reset last vehicle filter, in case goods list has changed
		gui_convoy_assembler_t::selected_filter = VEHICLE_FILTER_RELEVANT;

		FOR(vector_tpl<fabrik_t*>, const factory, get_fab_list()) {
			slist_tpl<ware_besch_t const*>* const produced_goods = factory->get_produced_goods();
			FOR(slist_tpl<ware_besch_t const*>, const good, *produced_goods) {
				goods_in_game.insert_unique_ordered(good, sort_ware_by_name);
			}
			delete produced_goods;
		}

		goods_in_game.insert_at(0, warenbauer_t::passagiere);
		goods_in_game.insert_at(1, warenbauer_t::post);
	}

	return goods_in_game;
}

void karte_t::add_building_to_world_list(gebaeude_t *gb)
{
	assert(gb);
	if(gb != gb->get_first_tile())
	{
		return;
	}

	if(gb->get_adjusted_population() > 0)
	{
		passenger_origins.append(gb, gb->get_adjusted_population());
		passenger_step_interval = calc_adjusted_step_interval(passenger_origins.get_sum_weight(), get_settings().get_passenger_trips_per_month_hundredths());
	}	
	commuter_targets.append(gb, gb->get_adjusted_jobs());
	visitor_targets.append(gb, gb->get_adjusted_visitor_demand());
	if(gb->get_adjusted_mail_demand() > 0)
	{
		mail_origins_and_targets.append(gb, gb->get_adjusted_mail_demand());
		mail_step_interval = calc_adjusted_step_interval(mail_origins_and_targets.get_sum_weight(), get_settings().get_mail_packets_per_month_hundredths());
	}
}

void karte_t::remove_building_from_world_list(gebaeude_t *gb)
{
	// We do not need to specify the type here, as we can try removing from all lists.
	passenger_origins.remove_all(gb);
	commuter_targets.remove_all(gb);
	visitor_targets.remove_all(gb);
	mail_origins_and_targets.remove_all(gb);

	passenger_step_interval = calc_adjusted_step_interval(passenger_origins.get_sum_weight(), get_settings().get_passenger_trips_per_month_hundredths());
	mail_step_interval = calc_adjusted_step_interval(mail_origins_and_targets.get_sum_weight(), get_settings().get_mail_packets_per_month_hundredths());
}

void karte_t::update_weight_of_building_in_world_list(gebaeude_t *gb)
{
	gb = gb->get_first_tile();
	if(!gb || gb->get_is_factory() && gb->get_fabrik() == NULL)
	{
		// The tile will be set to "is_factory" but the factory pointer will be NULL when
		// this is called from a field of a factory that is closing down.
		return;
	}

	if(passenger_origins.is_contained(gb))
	{
		passenger_origins.update_at(passenger_origins.index_of(gb), gb->get_adjusted_population());
		passenger_step_interval = calc_adjusted_step_interval(passenger_origins.get_sum_weight(), get_settings().get_passenger_trips_per_month_hundredths());
	}

	if(commuter_targets.is_contained(gb))
	{
		commuter_targets.update_at(commuter_targets.index_of(gb), gb->get_adjusted_jobs());
	}

	if(visitor_targets.is_contained(gb))
	{
		visitor_targets.update_at(visitor_targets.index_of(gb), gb->get_adjusted_visitor_demand());
	}

	if(mail_origins_and_targets.is_contained(gb))
	{
		mail_origins_and_targets.update_at(mail_origins_and_targets.index_of(gb), gb->get_adjusted_mail_demand());
		mail_step_interval = calc_adjusted_step_interval(mail_origins_and_targets.get_sum_weight(), get_settings().get_mail_packets_per_month_hundredths());
	}
}

vector_tpl<car_ownership_record_t> karte_t::car_ownership;

sint16 karte_t::get_private_car_ownership(sint32 monthyear) const
{

	if(monthyear == 0) 
	{
		return default_car_ownership_percent;
	}

	// ok, now lets see if we have data for this
	if(car_ownership.get_count()) 
	{
		uint i=0;
		while(i < car_ownership.get_count() && monthyear >= car_ownership[i].year)
		{
			i++;
		}
		if(i == car_ownership.get_count()) 
		{
			// maxspeed already?
			return car_ownership[i-1].ownership_percent;
		}
		else if(i == 0) 
		{
			// minspeed below
			return car_ownership[0].ownership_percent;
		}
		else 
		{
			// interpolate linear
			const sint32 delta_ownership_percent = car_ownership[i].ownership_percent - car_ownership[i-1].ownership_percent;
			const sint64 delta_years = car_ownership[i].year - car_ownership[i-1].year;
			return ((delta_ownership_percent * (monthyear-car_ownership[i-1].year)) / delta_years ) + car_ownership[i-1].ownership_percent;
		}
	}
	else
	{
		return default_car_ownership_percent;
	}
}

void karte_t::privatecar_init(const std::string &objfilename)
{
	tabfile_t ownership_file;
	// first take user data, then user global data
	if(!ownership_file.open((objfilename+"config/privatecar.tab").c_str()))
	{
		dbg->message("stadt_t::privatecar_init()", "Error opening config/privatecar.tab.\nWill use default value." );
		return;
	}

	tabfileobj_t contents;
	ownership_file.read(contents);

	/* init the values from line with the form year, proportion, year, proportion
	 * must be increasing order!
	 */
	int *tracks = contents.get_ints("car_ownership");
	if((tracks[0]&1) == 1) 
	{
		dbg->message("stadt_t::privatecar_init()", "Ill formed line in config/privatecar.tab.\nWill use default value. Format is year,ownership percentage[ year,ownership percentage]!" );
		car_ownership.clear();
		return;
	}
	car_ownership.resize(tracks[0] / 2);
	for(int i = 1; i < tracks[0]; i += 2) 
	{
		car_ownership_record_t c(tracks[i], tracks[i+1]);
		car_ownership.append(c);
	}
	delete [] tracks;
}

/**
* Reads/writes private car ownership data from/to a savegame
* called from karte_t::speichern and karte_t::laden
* only written for networkgames
* @author jamespetts
*/
void karte_t::privatecar_rdwr(loadsave_t *file)
{
	if(file->get_experimental_version() < 9)
	{
		 return;
	}

	if(file->is_saving())
	{
		uint32 count = car_ownership.get_count();
		file->rdwr_long(count);
		ITERATE(car_ownership, i)
		{
			
			file->rdwr_longlong(car_ownership.get_element(i).year);
			file->rdwr_short(car_ownership.get_element(i).ownership_percent);
		}	
	}

	else
	{
		car_ownership.clear();
		uint32 counter;
		file->rdwr_long(counter);
		sint64 year = 0;
		uint16 ownership_percent = 0;
		for(uint32 c = 0; c < counter; c ++)
		{
			file->rdwr_longlong(year);
			file->rdwr_short(ownership_percent);
			car_ownership_record_t cow(year / 12, ownership_percent);
			car_ownership.append(cow);
		}
	}
}

sint64 karte_t::get_land_value (koord3d k)
{
	// TODO: Have this based on a much more sophisticated
	// formula derived from local desirability, based on 
	// transport success rates. 

	// NOTE: settings.cst_buy_land is a *negative* number.
	sint64 cost = settings.cst_buy_land;
	const stadt_t* city = get_city(k.get_2d());
	const grund_t* gr = lookup_kartenboden(k.get_2d());
	if(city)
	{
		if(city->get_city_population() >= settings.get_city_threshold_size())
		{
			cost *= 4;
		}
		else if(city->get_city_population() >= settings.get_capital_threshold_size())
		{
			cost *= 6;
		}
		else
		{
			cost *= 3;
		}
	}
	else
	{
		if(k.z > get_grundwasser() + 10)
		{
			// Mountainous areas are cheaper
			cost *= 70;
			cost /= 100;
		}
	}

	if(lookup_hgt(k.get_2d()) != k.z && gr)
	{
		// Elevated or underground way being built.
		// Check for building and pay wayleaves if necessary.
		const gebaeude_t* gb = obj_cast<gebaeude_t>(gr->first_obj());
		if(gb)
		{
			cost -= (gb->get_tile()->get_besch()->get_level() * settings.cst_buy_land) / 5;
		}
		// Building other than on the surface of the land is cheaper in any event.
		cost /= 2;
	}

	if(gr && gr->ist_wasser())
	{
		// Water is cheaper than land.
		cost /= 4;
	}

	return cost;
}
