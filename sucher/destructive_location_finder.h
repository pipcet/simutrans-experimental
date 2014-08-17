class destructive_location_finder_t : public platzsucher_t {
	sint64 max_cost;

public:
	destructive_location_finder_t(karte_t *welt, sint64 max_cost, sint16 radius = -1) : platzsucher_t(welt, radius) { this->max_cost = max_cost; }

	virtual bool ist_platz_ok(koord pos, sint16 b, sint16 h, climate_bits cl) const {
		if (!platzsucher_t::ist_platz_ok(pos, b, h, cl)) {
			return false;
		}

		sint64 cost = 0;
		koord k(b, h);

		while(k.y-- > 0) {
			k.x = b;
			while(k.x-- > 0) {
				cost += tile_cost(pos, k, cl);
			}
		}

		return cost <= max_cost;
	}

	virtual sint64 tile_cost(koord pos, koord d, climate_bits cl) const {
		return 0;
	}
};
