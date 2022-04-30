/*-
 * Copyright 2022 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "symcache_internal.hxx"
#include "symcache_item.hxx"
#include "symcache_runtime.hxx"
#include "libutil/cxx/util.hxx"
#include "libserver/task.h"
#include "libmime/scan_result.h"
#include <limits>

namespace rspamd::symcache {

/* At least once per minute */
constexpr static const auto PROFILE_MAX_TIME = 60.0;
/* For messages larger than 2Mb enable profiling */
constexpr static const auto PROFILE_MESSAGE_SIZE_THRESHOLD = 1024ul * 1024 * 2;
/* Enable profile at least once per this amount of messages processed */
constexpr static const auto PROFILE_PROBABILITY = 0.01;

auto
symcache_runtime::create(struct rspamd_task *task, symcache &cache) -> symcache_runtime *
{
	cache.maybe_resort();

	auto &&cur_order = cache.get_cache_order();
	auto *checkpoint = (symcache_runtime *) rspamd_mempool_alloc0 (task->task_pool,
			sizeof(symcache_runtime) +
			sizeof(struct cache_dynamic_item) * cur_order->size());

	checkpoint->order = cache.get_cache_order();
	rspamd_mempool_add_destructor(task->task_pool,
			symcache_runtime::savepoint_dtor, checkpoint);

	for (auto &pair: checkpoint->last_id_mappings) {
		pair.first = -1;
		pair.second = -1;
	}

	/* Calculate profile probability */
	ev_now_update_if_cheap(task->event_loop);
	ev_tstamp now = ev_now(task->event_loop);
	checkpoint->profile_start = now;

	if ((cache.get_last_profile() == 0.0 || now > cache.get_last_profile() + PROFILE_MAX_TIME) ||
		(task->msg.len >= PROFILE_MESSAGE_SIZE_THRESHOLD) ||
		(rspamd_random_double_fast() >= (1 - PROFILE_PROBABILITY))) {
		msg_debug_cache_task("enable profiling of symbols for task");
		checkpoint->profile = true;
		cache.set_last_profile(now);
	}

	task->symcache_runtime = (void *) checkpoint;

	return checkpoint;
}

auto
symcache_runtime::process_settings(struct rspamd_task *task, const symcache &cache) -> bool
{
	if (!task->settings) {
		msg_err_task("`process_settings` is called with no settings");
		return false;
	}

	const auto *wl = ucl_object_lookup(task->settings, "whitelist");

	if (wl != nullptr) {
		msg_info_task("task is whitelisted");
		task->flags |= RSPAMD_TASK_FLAG_SKIP;
		return true;
	}

	auto already_disabled = false;

	auto process_group = [&](const ucl_object_t *gr_obj, auto functor) -> void {
		ucl_object_iter_t it = nullptr;
		const ucl_object_t *cur;

		if (gr_obj) {
			while ((cur = ucl_iterate_object(gr_obj, &it, true)) != nullptr) {
				if (ucl_object_type(cur) == UCL_STRING) {
					auto *gr = (struct rspamd_symbols_group *)
							g_hash_table_lookup(task->cfg->groups,
									ucl_object_tostring(cur));

					if (gr) {
						GHashTableIter gr_it;
						void *k, *v;
						g_hash_table_iter_init(&gr_it, gr->symbols);

						while (g_hash_table_iter_next(&gr_it, &k, &v)) {
							functor((const char *) k);
						}
					}
				}
			}
		}
	};

	ucl_object_iter_t it = nullptr;
	const ucl_object_t *cur;

	const auto *enabled = ucl_object_lookup(task->settings, "symbols_enabled");

	if (enabled) {
		/* Disable all symbols but selected */
		disable_all_symbols(SYMBOL_TYPE_EXPLICIT_DISABLE);
		already_disabled = true;
		it = nullptr;

		while ((cur = ucl_iterate_object(enabled, &it, true)) != nullptr) {
			enable_symbol(task, cache, ucl_object_tostring(cur));
		}
	}

	/* Enable groups of symbols */
	enabled = ucl_object_lookup(task->settings, "groups_enabled");
	if (enabled && !already_disabled) {
		disable_all_symbols(SYMBOL_TYPE_EXPLICIT_DISABLE);
	}
	process_group(enabled, [&](const char *sym) {
		enable_symbol(task, cache, sym);
	});

	const auto *disabled = ucl_object_lookup(task->settings, "symbols_disabled");

	if (disabled) {
		it = nullptr;

		while ((cur = ucl_iterate_object (disabled, &it, true)) != nullptr) {
			disable_symbol(task, cache, ucl_object_tostring(cur));
		}
	}

	/* Disable groups of symbols */
	disabled = ucl_object_lookup(task->settings, "groups_disabled");
	process_group(disabled, [&](const char *sym) {
		disable_symbol(task, cache, sym);
	});

	return false;
}

auto symcache_runtime::disable_all_symbols(int skip_mask) -> void
{
	for (auto i = 0; i < order->size(); i++) {
		auto *dyn_item = &dynamic_items[i];
		const auto &item = order->d[i];

		if (!(item->get_flags() & skip_mask)) {
			dyn_item->finished = true;
			dyn_item->started = true;
		}
	}
}

auto
symcache_runtime::disable_symbol(struct rspamd_task *task, const symcache &cache, std::string_view name) -> bool
{
	const auto *item = cache.get_item_by_name(name, true);

	if (item != nullptr) {

		auto *dyn_item = get_dynamic_item(item->id, false);

		if (dyn_item) {
			dyn_item->finished = true;
			dyn_item->started = true;
			msg_debug_cache_task("disable execution of %s", name.data());

			return true;
		}
		else {
			msg_debug_cache_task("cannot disable %s: id not found %d", name.data(), item->id);
		}
	}
	else {
		msg_debug_cache_task("cannot disable %s: symbol not found", name.data());
	}

	return false;
}

auto
symcache_runtime::enable_symbol(struct rspamd_task *task, const symcache &cache, std::string_view name) -> bool
{
	const auto *item = cache.get_item_by_name(name, true);

	if (item != nullptr) {

		auto *dyn_item = get_dynamic_item(item->id, false);

		if (dyn_item) {
			dyn_item->finished = false;
			dyn_item->started = false;
			msg_debug_cache_task("enable execution of %s", name.data());

			return true;
		}
		else {
			msg_debug_cache_task("cannot enable %s: id not found %d", name.data(), item->id);
		}
	}
	else {
		msg_debug_cache_task("cannot enable %s: symbol not found", name.data());
	}

	return false;
}

auto
symcache_runtime::is_symbol_checked(const symcache &cache, std::string_view name) -> bool
{
	const auto *item = cache.get_item_by_name(name, true);

	if (item != nullptr) {

		auto *dyn_item = get_dynamic_item(item->id, true);

		if (dyn_item) {
			return dyn_item->started;
		}
	}

	return false;
}

auto
symcache_runtime::is_symbol_enabled(struct rspamd_task *task, const symcache &cache, std::string_view name) -> bool
{

	const auto *item = cache.get_item_by_name(name, true);
	if (item) {

		if (!item->is_allowed(task, true)) {
			return false;
		}
		else {
			auto *dyn_item = get_dynamic_item(item->id, true);

			if (dyn_item) {
				if (dyn_item->started) {
					/* Already started */
					return false;
				}

				if (!item->is_virtual()) {
					return std::get<normal_item>(item->specific).check_conditions(item->symbol, task);
				}
			}
			else {
				/* Unknown item */
				msg_debug_cache_task("cannot enable %s: symbol not found", name.data());
			}
		}
	}

	return true;
}

auto symcache_runtime::get_dynamic_item(int id, bool save_in_cache) const -> cache_dynamic_item *
{
	/* Lookup in cache */
	if (save_in_cache) {
		for (const auto &cache_id: last_id_mappings) {
			if (cache_id.first == -1) {
				break;
			}
			if (cache_id.first == id) {
				auto *dyn_item = &dynamic_items[cache_id.second];

				return dyn_item;
			}
		}
	}

	/* Not found in the cache, do a hash lookup */
	auto our_id_maybe = rspamd::find_map(order->by_cache_id, id);

	if (our_id_maybe) {
		auto *dyn_item = &dynamic_items[our_id_maybe.value()];

		if (!save_in_cache) {
			return dyn_item;
		}

		/* Insert in the cache, swapping the first item with the last empty item */
		auto first_known = last_id_mappings[0];
		last_id_mappings[0].first = id;
		last_id_mappings[0].second = our_id_maybe.value();

		if (first_known.first != -1) {
			/* This loop is guaranteed to finish as we have just inserted one item */

			constexpr const auto cache_size = sizeof(last_id_mappings) / sizeof(last_id_mappings[0]);
			int i = cache_size - 1;
			for (;; --i) {
				if (last_id_mappings[i].first != -1) {
					if (i < cache_size - 1) {
						i++;
					}
					break;
				}
			}

			last_id_mappings[i] = first_known;
		}

		return dyn_item;
	}

	return nullptr;
}

auto symcache_runtime::process_symbols(struct rspamd_task *task, symcache &cache, int stage) -> bool
{
	msg_debug_cache_task("symbols processing stage at pass: %d", stage);

	if (RSPAMD_TASK_IS_SKIPPED(task)) {
		return true;
	}

	switch (stage) {
	case RSPAMD_TASK_STAGE_CONNFILTERS:
	case RSPAMD_TASK_STAGE_PRE_FILTERS:
	case RSPAMD_TASK_STAGE_POST_FILTERS:
	case RSPAMD_TASK_STAGE_IDEMPOTENT:
		return process_pre_postfilters(task, cache,
				rspamd_session_events_pending(task->s), stage);
		break;

	case RSPAMD_TASK_STAGE_FILTERS:
		return process_filters(task, cache,rspamd_session_events_pending(task->s));
		break;

	default:
		g_assert_not_reached ();
	}
}

auto
symcache_runtime::process_pre_postfilters(struct rspamd_task *task,
										  symcache &cache,
										  int start_events,
										  int stage) -> bool
{
	auto saved_priority = std::numeric_limits<int>::min();
	auto all_done = true;
	auto compare_functor = +[](int a, int b) { return a < b; };

	auto proc_func = [&](cache_item *item) {
		auto dyn_item = get_dynamic_item(item->id, true);

		if (!dyn_item->started && !dyn_item->finished) {
			if (has_slow) {
				/* Delay */
				has_slow = false;

				return false;
			}

			if (saved_priority == std::numeric_limits<int>::min()) {
				saved_priority = item->priority;
			}
			else {
				if (compare_functor(item->priority, saved_priority) &&
					rspamd_session_events_pending(task->s) > start_events) {
					/*
					 * Delay further checks as we have higher
					 * priority filters to be processed
					 */
					return false;
				}
			}

			process_symbol(task, cache, item, dyn_item);
			all_done = false;
		}

		/* Continue processing */
		return true;
	};

	switch (stage) {
	case RSPAMD_TASK_STAGE_CONNFILTERS:
		all_done = cache.connfilters_foreach(proc_func);
		break;
	case RSPAMD_TASK_STAGE_PRE_FILTERS:
		all_done = cache.prefilters_foreach(proc_func);
		break;
	case RSPAMD_TASK_STAGE_POST_FILTERS:
		compare_functor = +[](int a, int b) { return a > b; };
		all_done = cache.postfilters_foreach(proc_func);
		break;
	case RSPAMD_TASK_STAGE_IDEMPOTENT:
		compare_functor = +[](int a, int b) { return a > b; };
		all_done = cache.idempotent_foreach(proc_func);
		break;
	default:
		g_error("invalid invocation");
		break;
	}

	return all_done;
}

auto
symcache_runtime::process_filters(struct rspamd_task *task, symcache &cache, int start_events) -> bool
{
	auto all_done = true;

	cache.filters_foreach([&](cache_item *item) -> bool {
		if (item->type == symcache_item_type::CLASSIFIER) {
			return true;
		}

		auto dyn_item = get_dynamic_item(item->id, true);

		if (!dyn_item->started && !dyn_item->finished) {
			all_done = false;

			if (!rspamd_symcache_check_deps(task, cache, item,
					checkpoint, 0, FALSE)) {
				msg_debug_cache_task ("blocked execution of %d(%s) unless deps are "
									  "resolved", item->id, item->symbol.c_str());

				return true;
			}

			process_symbol(task, cache, item, dyn_item);

			if (has_slow) {
				/* Delay */
				has_slow = false;

				return false;
			}
		}

		if (!(item->flags & SYMBOL_TYPE_FINE)) {
			if (rspamd_symcache_metric_limit(task, checkpoint)) {
				msg_info_task ("task has already scored more than %.2f, so do "
							   "not "
							   "plan more checks",
						rs->score);
				all_done = true;
				return false;
			}
		}
	});

	return all_done;
}

auto
symcache_runtime::process_symbol(struct rspamd_task *task, symcache &cache, cache_item *item,
								 cache_dynamic_item *dyn_item) -> bool
{
	if (item->type == symcache_item_type::CLASSIFIER || item->type == symcache_item_type::COMPOSITE) {
		/* Classifiers are special :( */
		return true;
	}

	if (rspamd_session_blocked(task->s)) {
		/*
		 * We cannot add new events as session is either destroyed or
		 * being cleaned up.
		 */
		return true;
	}

	g_assert (!item->is_virtual());
	if (dyn_item->started) {
		/*
		 * This can actually happen when deps span over different layers
		 */
		return dyn_item->finished;
	}

	/* Check has been started */
	dyn_item->started = true;
	auto check = true;

	if (!item->is_allowed(task, true) || !item->check_conditions(task)) {
		check = false;
	}

	if (check) {
		msg_debug_cache_task("execute %s, %d; symbol type = %s", item->symbol.data(),
				item->id);

		if (profile) {
			ev_now_update_if_cheap(task->event_loop);
			dyn_item->start_msec = (ev_now(task->event_loop) -
									profile_start) * 1e3;
		}

		dyn_item->async_events = 0;
		cur_item = item;
		items_inflight++;
		/* Callback now must finalize itself */
		item->call(task);
		cur_item = NULL;

		if (items_inflight == 0) {
			return true;
		}

		if (dyn_item->async_events == 0 && !dyn_item->finished) {
			msg_err_cache_task("critical error: item %s has no async events pending, "
							   "but it is not finalised", item->symbol.data());
			g_assert_not_reached ();
		}

		return false;
	}
	else {
		dyn_item->finished = true;
	}

	return true;
}

}

