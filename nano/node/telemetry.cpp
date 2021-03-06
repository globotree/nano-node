#include <nano/lib/alarm.hpp>
#include <nano/lib/worker.hpp>
#include <nano/node/network.hpp>
#include <nano/node/telemetry.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/buffer.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <future>
#include <numeric>

std::chrono::milliseconds constexpr nano::telemetry_impl::cache_cutoff;

nano::telemetry::telemetry (nano::network & network_a, nano::alarm & alarm_a, nano::worker & worker_a) :
network (network_a),
alarm (alarm_a),
worker (worker_a),
batch_request (std::make_shared<nano::telemetry_impl> (network, alarm, worker))
{
}

void nano::telemetry::stop ()
{
	nano::lock_guard<std::mutex> guard (mutex);
	batch_request = nullptr;
	single_requests.clear ();
	stopped = true;
}

void nano::telemetry::add (nano::telemetry_data const & telemetry_data_a, nano::endpoint const & endpoint_a)
{
	nano::lock_guard<std::mutex> guard (mutex);
	if (!stopped)
	{
		batch_request->add (telemetry_data_a, endpoint_a);

		for (auto & request : single_requests)
		{
			request.second.impl->add (telemetry_data_a, endpoint_a);
		}
	}
}

void nano::telemetry::get_metrics_random_peers_async (std::function<void(telemetry_data_responses const &)> const & callback_a)
{
	// These peers will only be used if there isn't an already ongoing batch telemetry request round
	auto random_peers = network.random_set (network.size_sqrt (), network_params.protocol.telemetry_protocol_version_min);
	nano::lock_guard<std::mutex> guard (mutex);
	if (!stopped && !random_peers.empty ())
	{
		batch_request->get_metrics_async (random_peers, [callback_a](nano::telemetry_data_responses const & telemetry_data_responses) {
			callback_a (telemetry_data_responses);
		});
	}
	else
	{
		const auto all_received = false;
		callback_a (nano::telemetry_data_responses{ {}, false, all_received });
	}
}

nano::telemetry_data_responses nano::telemetry::get_metrics_random_peers ()
{
	std::promise<telemetry_data_responses> promise;
	get_metrics_random_peers_async ([&promise](telemetry_data_responses const & telemetry_data_responses_a) {
		promise.set_value (telemetry_data_responses_a);
	});

	return promise.get_future ().get ();
}

// After a request is made to a single peer we want to remove it from the container after the peer has not been requested for a while (cache_cutoff).
void nano::telemetry::ongoing_single_request_cleanup (nano::endpoint const & endpoint_a, nano::telemetry::single_request_data const & single_request_data_a)
{
	// This class is just
	class ongoing_func_wrapper
	{
	public:
		std::function<void()> ongoing_func;
	};

	auto wrapper = std::make_shared<ongoing_func_wrapper> ();
	// Keep calling ongoing_func while the peer is still being called
	const auto & last_updated = single_request_data_a.last_updated;
	wrapper->ongoing_func = [this, telemetry_impl_w = std::weak_ptr<nano::telemetry_impl> (single_request_data_a.impl), &last_updated, &endpoint_a, wrapper]() {
		if (auto telemetry_impl = telemetry_impl_w.lock ())
		{
			nano::lock_guard<std::mutex> guard (this->mutex);
			if (std::chrono::steady_clock::now () - telemetry_impl->cache_cutoff > last_updated && telemetry_impl->callbacks.empty ())
			{
				this->single_requests.erase (endpoint_a);
			}
			else
			{
				// Request is still active, so call again
				this->alarm.add (std::chrono::steady_clock::now () + telemetry_impl->cache_cutoff, wrapper->ongoing_func);
			}
		}
	};

	alarm.add (std::chrono::steady_clock::now () + single_request_data_a.impl->cache_cutoff, wrapper->ongoing_func);
}

void nano::telemetry::update_cleanup_data (nano::endpoint const & endpoint_a, nano::telemetry::single_request_data & single_request_data_a, bool is_new_a)
{
	auto telemetry_impl = single_request_data_a.impl;
	if (is_new_a)
	{
		// Clean this request up when it isn't being used anymore
		ongoing_single_request_cleanup (endpoint_a, single_request_data_a);
	}
	else
	{
		// Ensure that refreshed flag is reset so we don't delete it before processing
		single_request_data_a.last_updated = std::chrono::steady_clock::now ();
	}
}

void nano::telemetry::get_metrics_single_peer_async (std::shared_ptr<nano::transport::channel> const & channel_a, std::function<void(telemetry_data_response const &)> const & callback_a)
{
	auto invoke_callback_with_error = [&callback_a]() {
		auto const is_cached = false;
		auto const error = true;
		callback_a ({ nano::telemetry_data (), is_cached, error });
	};

	nano::lock_guard<std::mutex> guard (mutex);
	if (!stopped)
	{
		if (channel_a && (channel_a->get_network_version () >= network_params.protocol.telemetry_protocol_version_min))
		{
			auto pair = single_requests.emplace (channel_a->get_endpoint (), single_request_data{ std::make_shared<nano::telemetry_impl> (network, alarm, worker), std::chrono::steady_clock::now () });
			update_cleanup_data (pair.first->first, pair.first->second, pair.second);

			pair.first->second.impl->get_metrics_async ({ channel_a }, [callback_a](telemetry_data_responses const & telemetry_data_responses_a) {
				// There should only be 1 response, so if this hasn't been received then conclude it is an error.
				auto const error = !telemetry_data_responses_a.all_received;
				if (!error)
				{
					assert (telemetry_data_responses_a.data.size () == 1);
					callback_a ({ telemetry_data_responses_a.data.front (), telemetry_data_responses_a.is_cached, error });
				}
				else
				{
					callback_a ({ nano::telemetry_data (), telemetry_data_responses_a.is_cached, error });
				}
			});
		}
		else
		{
			invoke_callback_with_error ();
		}
	}
	else
	{
		invoke_callback_with_error ();
	}
}

nano::telemetry_data_response nano::telemetry::get_metrics_single_peer (std::shared_ptr<nano::transport::channel> const & channel_a)
{
	std::promise<telemetry_data_response> promise;
	get_metrics_single_peer_async (channel_a, [&promise](telemetry_data_response const & single_metric_data_a) {
		promise.set_value (single_metric_data_a);
	});

	return promise.get_future ().get ();
}

size_t nano::telemetry::telemetry_data_size ()
{
	nano::lock_guard<std::mutex> guard (mutex);
	auto total = std::accumulate (single_requests.begin (), single_requests.end (), static_cast<size_t> (0), [](size_t total, auto & single_request) {
		return total += single_request.second.impl->telemetry_data_size ();
	});

	if (batch_request)
	{
		total += batch_request->telemetry_data_size ();
	}
	return total;
}

nano::telemetry_impl::telemetry_impl (nano::network & network_a, nano::alarm & alarm_a, nano::worker & worker_a) :
network (network_a),
alarm (alarm_a),
worker (worker_a)
{
}

void nano::telemetry_impl::flush_callbacks (nano::unique_lock<std::mutex> & lk_a, bool cached_a)
{
	// Invoke all callbacks, it's possible that during the mutex unlock other callbacks were added,
	// so check again and invoke those too
	assert (lk_a.owns_lock ());
	invoking = true;
	while (!callbacks.empty ())
	{
		lk_a.unlock ();
		invoke_callbacks (cached_a);
		lk_a.lock ();
	}
	invoking = false;
}

void nano::telemetry_impl::get_metrics_async (std::unordered_set<std::shared_ptr<nano::transport::channel>> const & channels_a, std::function<void(telemetry_data_responses const &)> const & callback_a)
{
	{
		assert (!channels_a.empty ());
		nano::unique_lock<std::mutex> lk (mutex);
		callbacks.push_back (callback_a);
		if (callbacks.size () > 1 || invoking)
		{
			// This means we already have at least one pending result already, so it will handle calls this callback when it completes
			return;
		}

		// Check if we can just return cached results
		if (std::chrono::steady_clock::now () < (last_time + cache_cutoff))
		{
			// Post to worker so that it's truly async and not on the calling thread (same problem as std::async otherwise)
			worker.push_task ([this_w = std::weak_ptr<nano::telemetry_impl> (shared_from_this ())]() {
				if (auto this_l = this_w.lock ())
				{
					nano::unique_lock<std::mutex> lk (this_l->mutex);
					const auto is_cached = true;
					this_l->flush_callbacks (lk, is_cached);
				}
			});
			return;
		}

		all_received = true;
		assert (required_responses.empty ());
		std::transform (channels_a.begin (), channels_a.end (), std::inserter (required_responses, required_responses.end ()), [](auto const & channel) {
			return channel->get_endpoint ();
		});
	}

	fire_request_messages (channels_a);
}

void nano::telemetry_impl::add (nano::telemetry_data const & telemetry_data_a, nano::endpoint const & endpoint_a)
{
	nano::unique_lock<std::mutex> lk (mutex);
	if (required_responses.find (endpoint_a) == required_responses.cend ())
	{
		// Not requesting telemetry data from this channel so ignore it
		return;
	}

	current_telemetry_data_responses.push_back (telemetry_data_a);
	channel_processed (lk, endpoint_a);
}

void nano::telemetry_impl::invoke_callbacks (bool cached_a)
{
	decltype (callbacks) callbacks_l;

	{
		// Copy callbacks so that they can be called outside of holding the lock
		nano::lock_guard<std::mutex> guard (mutex);
		callbacks_l = callbacks;
		current_telemetry_data_responses.clear ();
		callbacks.clear ();
	}
	for (auto & callback : callbacks_l)
	{
		callback ({ cached_telemetry_data, cached_a, all_received });
	}
}

void nano::telemetry_impl::channel_processed (nano::unique_lock<std::mutex> & lk_a, nano::endpoint const & endpoint_a)
{
	assert (lk_a.owns_lock ());
	auto num_removed = required_responses.erase (endpoint_a);
	if (num_removed > 0 && required_responses.empty ())
	{
		assert (lk_a.owns_lock ());
		cached_telemetry_data = current_telemetry_data_responses;

		last_time = std::chrono::steady_clock::now ();
		auto const is_cached = false;
		flush_callbacks (lk_a, is_cached);
	}
}

void nano::telemetry_impl::fire_request_messages (std::unordered_set<std::shared_ptr<nano::transport::channel>> const & channels)
{
	uint64_t round_l;
	{
		nano::lock_guard<std::mutex> guard (mutex);
		++round;
		round_l = round;
	}

	// Fire off a telemetry request to all passed in channels
	nano::telemetry_req message;
	for (auto & channel : channels)
	{
		assert (channel->get_network_version () >= network_params.protocol.telemetry_protocol_version_min);

		std::weak_ptr<nano::telemetry_impl> this_w (shared_from_this ());
		channel->send (message, [this_w, endpoint = channel->get_endpoint ()](boost::system::error_code const & ec, size_t size_a) {
			if (auto this_l = this_w.lock ())
			{
				if (ec)
				{
					// Error sending the telemetry_req message
					nano::unique_lock<std::mutex> lk (this_l->mutex);
					this_l->all_received = false;
					this_l->channel_processed (lk, endpoint);
				}
			}
		});

		// If no response is seen after a certain period of time, remove it from the list of expected responses. However, only if it is part of the same round.
		alarm.add (std::chrono::steady_clock::now () + cache_cutoff, [this_w, endpoint = channel->get_endpoint (), round_l]() {
			if (auto this_l = this_w.lock ())
			{
				nano::unique_lock<std::mutex> lk (this_l->mutex);
				if (this_l->round == round_l && this_l->required_responses.find (endpoint) != this_l->required_responses.cend ())
				{
					this_l->all_received = false;
					this_l->channel_processed (lk, endpoint);
				}
			}
		});
	}
}

size_t nano::telemetry_impl::telemetry_data_size ()
{
	nano::lock_guard<std::mutex> guard (mutex);
	return current_telemetry_data_responses.size ();
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (telemetry & telemetry, const std::string & name)
{
	size_t single_requests_count;
	{
		nano::lock_guard<std::mutex> guard (telemetry.mutex);
		single_requests_count = telemetry.single_requests.size ();
	}

	auto composite = std::make_unique<container_info_composite> (name);
	if (telemetry.batch_request)
	{
		composite->add_component (collect_container_info (*telemetry.batch_request, "batch_request"));
	}
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "single_requests", single_requests_count, sizeof (decltype (telemetry.single_requests)::value_type) }));
	return composite;
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (telemetry_impl & telemetry_impl, const std::string & name)
{
	size_t callback_count;
	size_t all_telemetry_data_count;
	size_t cached_telemetry_data_count;
	size_t required_responses_count;
	{
		nano::lock_guard<std::mutex> guard (telemetry_impl.mutex);
		callback_count = telemetry_impl.callbacks.size ();
		all_telemetry_data_count = telemetry_impl.current_telemetry_data_responses.size ();
		cached_telemetry_data_count = telemetry_impl.cached_telemetry_data.size ();
		required_responses_count = telemetry_impl.required_responses.size ();
	}

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "callbacks", callback_count, sizeof (decltype (telemetry_impl.callbacks)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "current_telemetry_data_responses", all_telemetry_data_count, sizeof (decltype (telemetry_impl.current_telemetry_data_responses)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "cached_telemetry_data", cached_telemetry_data_count, sizeof (decltype (telemetry_impl.cached_telemetry_data)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "required_responses", required_responses_count, sizeof (decltype (telemetry_impl.required_responses)::value_type) }));
	return composite;
}
