#include "api.h"

/** @file api_halt.cc exports halt/station related functions. */

#include "get_next.h"

#include "api_obj_desc_base.h"
#include "../api_class.h"
#include "../api_function.h"
#include "../../simhalt.h"

using namespace script_api;

vector_tpl<sint64> const& get_halt_stat(halthandle_t halt, sint32 INDEX)
{
	static vector_tpl<sint64> v;
	v.clear();
	if (halt.is_bound()  &&  0<=INDEX  &&  INDEX<MAX_HALT_COST) {
		for(uint16 i = 0; i < MAX_MONTHS; i++) {
			v.append( halt->get_finance_history(i, INDEX) );
		}
	}
	return v;
}


SQInteger world_get_next_halt(HSQUIRRELVM vm)
{
	return generic_get_next(vm, haltestelle_t::get_alle_haltestellen().get_count());
}


SQInteger world_get_halt_by_index(HSQUIRRELVM vm)
{
	sint32 index = param<sint32>::get(vm, -1);
	const vector_tpl<halthandle_t>& list = haltestelle_t::get_alle_haltestellen();
	halthandle_t halt = (0<=index  &&  (uint32)index<list.get_count()) ?  list[index] : halthandle_t();
	return param<halthandle_t>::push(vm, halt);
}


// 0: not connected
// 1: connected
// -1: undecided
sint8 is_halt_connected(halthandle_t a, halthandle_t b, const ware_besch_t *besch)
{
	if (besch == 0  ||  !a.is_bound()  || !b.is_bound()) {
		return 0;
	}
	return a->is_connected(b, besch->get_catg_index());
}


void export_halt(HSQUIRRELVM vm)
{
	/**
	 * Implements iterator to iterate through the list of all halts on the map.
	 *
	 * Usage:
	 * @code
	 * local list = halt_list_x()
	 * foreach(halt in list) {
	 *     ... // halt is an instance of the halt_x class
	 * }
	 * @endcode
	 */
	create_class(vm, "halt_list_x");
	/**
	 * Meta-method to be used in foreach loops. Do not call them directly.
	 */
	register_function(vm, world_get_next_halt,     "_nexti",  2, "x o|i");
	/**
	 * Meta-method to be used in foreach loops. Do not call them directly.
	 * @typemask halt_x()
	 */
	register_function(vm, world_get_halt_by_index, "_get",    2, "xi");
	end_class(vm);

	/**
	 * Class to access halts.
	 */
	begin_class(vm, "halt_x", "extend_get");

	/**
	 * Station name.
	 * @returns name
	 */
	register_method(vm, &haltestelle_t::get_name, "get_name");

	/**
	 * Station owner.
	 * @returns owner
	 */
	register_method(vm, &haltestelle_t::get_besitzer, "get_owner");

	/**
	 * Quick check if there is connection for certain freight to the other halt.
	 * @param halt the other halt
	 * @param freight_type freight type
	 * @return 0 - not connected, 1 - connected, -1 - undecided (call this method again later)
	 */
	register_method(vm, &is_halt_connected, "is_connected", true);

	/**
	 * Does this station accept this type of good?
	 * @param freight_type freight type
	 * @returns the answer to this question
	 */
	register_method<bool (haltestelle_t::*)(const ware_besch_t*) const>(vm, &haltestelle_t::is_enabled, "accepts_good", false);

	/**
	 * Get monthly statistics of number of arrived goods.
	 * @returns array, index [0] corresponds to current month
	 */
	register_method_fv(vm, &get_halt_stat, "get_arrived", freevariable<sint32>(HALT_ARRIVED), true);
	/**
	 * Get monthly statistics of number of departed goods.
	 * @returns array, index [0] corresponds to current month
	 */
	register_method_fv(vm, &get_halt_stat, "get_departed", freevariable<sint32>(HALT_DEPARTED), true);
	/**
	 * Get monthly statistics of number of waiting goods.
	 * @returns array, index [0] corresponds to current month
	 */
	register_method_fv(vm, &get_halt_stat, "get_waiting", freevariable<sint32>(HALT_WAITING), true);
	/**
	 * Get monthly statistics of number of happy passengers.
	 * @returns array, index [0] corresponds to current month
	 */
	register_method_fv(vm, &get_halt_stat, "get_happy", freevariable<sint32>(HALT_HAPPY), true);
	/**
	 * Get monthly statistics of number of unhappy passengers.
	 *
	 * These passengers could not start their journey as station was crowded.
	 * @returns array, index [0] corresponds to current month
	 */
	register_method_fv(vm, &get_halt_stat, "get_unhappy", freevariable<sint32>(HALT_UNHAPPY), true);
	/**
	 * Get monthly statistics of number of passengers with no-route.
	 *
	 * These passengers could not start their journey as they find no route to their destination.
	 * @returns array, index [0] corresponds to current month
	 */
	register_method_fv(vm, &get_halt_stat, "get_noroute", freevariable<sint32>(HALT_NOROUTE), true);
	/**
	 * Get monthly statistics of number of convoys that stopped at this station.
	 * @returns array, index [0] corresponds to current month
	 */
	register_method_fv(vm, &get_halt_stat, "get_convoys", freevariable<sint32>(HALT_CONVOIS_ARRIVED), true);
	/**
	 * Get monthly statistics of number of passengers that could walk to their destination.
	 * @returns array, index [0] corresponds to current month
	 */
	register_method_fv(vm, &get_halt_stat, "get_too_slow", freevariable<sint32>(HALT_TOO_SLOW), true);

	end_class(vm);
}
