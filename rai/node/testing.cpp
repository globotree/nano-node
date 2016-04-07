#include <rai/node/testing.hpp>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

rai::system::system (uint16_t port_a, size_t count_a) :
service (new boost::asio::io_service),
alarm (*service),
work (nullptr)
{
    nodes.reserve (count_a);
    for (size_t i (0); i < count_a; ++i)
    {
        rai::node_init init;
		rai::node_config config (port_a + i, logging);
        auto node (std::make_shared <rai::node> (init, *service, rai::unique_path (), alarm, config, work));
        assert (!init.error ());
        node->start ();
		rai::uint256_union wallet;
		rai::random_pool.GenerateBlock (wallet.bytes.data (), wallet.bytes.size ());
		node->wallets.create (wallet);
        nodes.push_back (node);
    }
    for (auto i (nodes.begin ()), j (nodes.begin () + 1), n (nodes.end ()); j != n; ++i, ++j)
    {
        auto starting1 ((*i)->peers.size ());
        auto new1 (starting1);
        auto starting2 ((*j)->peers.size ());
        auto new2 (starting2);
        (*j)->network.send_keepalive ((*i)->network.endpoint ());
        do {
            poll ();
            new1 = (*i)->peers.size ();
            new2 = (*j)->peers.size ();
        } while (new1 == starting1 || new2 == starting2);
    }
	auto iterations1 (0);
	while (std::any_of (nodes.begin (), nodes.end (), [] (std::shared_ptr <rai::node> const & node_a) {return node_a->bootstrap_initiator.in_progress ();}))
	{
		poll ();
		++iterations1;
		assert (iterations1 < 1000);
	}
}

rai::system::~system ()
{
    for (auto & i: nodes)
    {
        i->stop ();
    }
}

std::shared_ptr <rai::wallet> rai::system::wallet (size_t index_a)
{
    assert (nodes.size () > index_a);
	auto size (nodes [index_a]->wallets.items.size ());
    assert (size == 1);
    return nodes [index_a]->wallets.items.begin ()->second;
}

rai::account rai::system::account (MDB_txn * transaction_a, size_t index_a)
{
    auto wallet_l (wallet (index_a));
    auto keys (wallet_l->store.begin (transaction_a));
    assert (keys != wallet_l->store.end ());
    auto result (keys->first);
    assert (++keys == wallet_l->store.end ());
    return result;
}

void rai::system::poll ()
{
	auto polled1 (service->poll_one ());
	if (polled1 == 0)
	{
		std::this_thread::sleep_for (std::chrono::milliseconds (50));
	}
}


namespace
{
class traffic_generator : public std::enable_shared_from_this <traffic_generator>
{
public:
    traffic_generator (uint32_t count_a, uint32_t wait_a, std::shared_ptr <rai::node> node_a, rai::system & system_a) :
    count (count_a),
    wait (wait_a),
    node (node_a),
    system (system_a)
    {
    }
    void run ()
    {
        auto count_l (count - 1);
        count = count_l - 1;
        system.generate_activity (*node, accounts);
        if (count_l > 0)
        {
            auto this_l (shared_from_this ());
            node->alarm.add (std::chrono::system_clock::now () + std::chrono::milliseconds (wait), [this_l] () {this_l->run ();});
        }
    }
	std::vector <rai::account> accounts;
    uint32_t count;
    uint32_t wait;
    std::shared_ptr <rai::node> node;
    rai::system & system;
};
}

void rai::system::generate_usage_traffic (uint32_t count_a, uint32_t wait_a)
{
    for (size_t i (0), n (nodes.size ()); i != n; ++i)
    {
        generate_usage_traffic (count_a, wait_a, i);
    }
}

void rai::system::generate_usage_traffic (uint32_t count_a, uint32_t wait_a, size_t index_a)
{
    assert (nodes.size () > index_a);
    assert (count_a > 0);
    auto generate (std::make_shared <traffic_generator> (count_a, wait_a, nodes [index_a], *this));
    generate->run ();
}

void rai::system::generate_activity (rai::node & node_a, std::vector <rai::account> & accounts_a)
{
    auto what (random_pool.GenerateByte ());
    if (what < 0xc0)
    {
        generate_send_existing (node_a, accounts_a);
    }
    else
    {
        generate_send_new (node_a, accounts_a);
    }
}

rai::account rai::system::get_random_account (std::vector <rai::account> & accounts_a)
{
	auto index (random_pool.GenerateWord32 (0, accounts_a.size () - 1));
	auto result (accounts_a [index]);
	return result;
}

rai::uint128_t rai::system::get_random_amount (MDB_txn * transaction_a, rai::node & node_a, rai::account const & account_a)
{
    rai::uint128_t balance (node_a.ledger.account_balance (transaction_a, account_a));
    std::string balance_text (balance.convert_to <std::string> ());
    rai::uint128_union random_amount;
    random_pool.GenerateBlock (random_amount.bytes.data (), sizeof (random_amount.bytes));
    auto result (((rai::uint256_t {random_amount.number ()} * balance) / rai::uint256_t {std::numeric_limits <rai::uint128_t>::max ()}).convert_to <rai::uint128_t> ());
    std::string text (result.convert_to <std::string> ());
    return result;
}

void rai::system::generate_send_existing (rai::node & node_a, std::vector <rai::account> & accounts_a)
{
	rai::uint128_t amount;
	rai::account destination;
	rai::account source;
	{
		rai::account account;
		random_pool.GenerateBlock (account.bytes.data (), sizeof (account.bytes));
		rai::transaction transaction (node_a.store.environment, nullptr, false);
		rai::store_iterator entry (node_a.store.latest_begin (transaction, account));
		if (entry == node_a.store.latest_end ())
		{
			entry = node_a.store.latest_begin (transaction);
		}
		assert (entry != node_a.store.latest_end ());
		destination = rai::account (entry->first);
		source = get_random_account (accounts_a);
		amount = get_random_amount (transaction, node_a, source);
	}
    wallet (0)->send_action (source, destination, amount);
}

void rai::system::generate_send_new (rai::node & node_a, std::vector <rai::account> & accounts_a)
{
    assert (node_a.wallets.items.size () == 1);
	rai::uint128_t amount;
	rai::account source;
	{
		rai::transaction transaction (node_a.store.environment, nullptr, false);
		source = get_random_account (accounts_a);
		amount = get_random_amount (transaction, node_a, source);
	}
	auto pub (node_a.wallets.items.begin ()->second->deterministic_insert ());
	accounts_a.push_back (pub);
    node_a.wallets.items.begin ()->second->send_async (source, pub, amount, [] (std::unique_ptr <rai::block>) {});
}

void rai::system::generate_mass_activity (uint32_t count_a, rai::node & node_a)
{
	std::vector <rai::account> accounts;
    wallet (0)->insert_adhoc (rai::test_genesis_key.prv);
	accounts.push_back (rai::test_genesis_key.pub);
    auto previous (std::chrono::system_clock::now ());
    for (uint32_t i (0); i < count_a; ++i)
    {
        if ((i & 0xfff) == 0)
        {
            auto now (std::chrono::system_clock::now ());
            auto us (std::chrono::duration_cast <std::chrono::microseconds> (now - previous).count ());
            std::cerr << boost::str (boost::format ("Mass activity iteration %1% us %2% us/t %3%\n") % i % us % (us / 256));
            previous = now;
        }
        generate_activity (node_a, accounts);
		poll ();
    }
}

void rai::system::stop ()
{
	for (auto i : nodes)
	{
		i->stop ();
	}
	work.stop ();
}

rai::landing_store::landing_store ()
{
}

rai::landing_store::landing_store (rai::account const & source_a, rai::account const & destination_a, uint64_t start_a, uint64_t last_a) :
source (source_a),
destination (destination_a),
start (start_a),
last (last_a)
{
}

rai::landing_store::landing_store (bool & error_a, std::istream & stream_a)
{
	error_a = deserialize (stream_a);
}

bool rai::landing_store::deserialize (std::istream & stream_a)
{
	bool result;
	try
	{
		boost::property_tree::ptree tree;
		boost::property_tree::read_json (stream_a, tree);
		auto source_l (tree.get <std::string> ("source"));
		auto destination_l (tree.get <std::string> ("destination"));
		auto start_l (tree.get <std::string> ("start"));
		auto last_l (tree.get <std::string> ("last"));
		result = source.decode_account (source_l);
		if (!result)
		{
			result = destination.decode_account (destination_l);
			if (!result)
			{
				start = std::stoull (start_l);
				last = std::stoull (last_l);
			}
		}
	}
	catch (std::logic_error const &)
	{
		result = true;
	}
	catch (std::runtime_error const &)
	{
		result = true;
	}
	return result;
}

void rai::landing_store::serialize (std::ostream & stream_a) const
{
	boost::property_tree::ptree tree;
	tree.put ("source", source.to_account ());
	tree.put ("destination", destination.to_account ());
	tree.put ("start", std::to_string (start));
	tree.put ("last", std::to_string (last));
	boost::property_tree::write_json (stream_a, tree);
}

bool rai::landing_store::operator == (rai::landing_store const & other_a) const
{
	return source == other_a.source && destination == other_a.destination && start == other_a.start && last == other_a.last;
}

rai::landing::landing (rai::node & node_a, std::shared_ptr <rai::wallet> wallet_a, rai::landing_store & store_a, boost::filesystem::path const & path_a) :
path (path_a),
store (store_a),
wallet (wallet_a),
node (node_a)
{
}

void rai::landing::write_store ()
{
	std::ofstream store_file;
	store_file.open (path.string ());
	if (!store_file.fail ())
	{
		store.serialize (store_file);
	}
	else
	{
		std::stringstream str;
		store.serialize (str);
		BOOST_LOG (node.log) << boost::str (boost::format ("Error writing store file %1%") % str.str ());
	}
}

rai::uint128_t rai::landing::distribution_amount (uint64_t interval)
{
	// Halfing period ~= Exponent of 2 in secounds approixmately 1 year = 2^25 = 33554432
	// Interval = Exponent of 2 in seconds approximately 1 minute = 2^10 = 64
	uint64_t intervals_per_period (1 << (25 - interval_exponent));
	rai::uint128_t result;
	if (interval < intervals_per_period * 1)
	{
		// Total supply / 2^halfing period / intervals per period
		// 2^128 / 2^1 / (2^25 / 2^10)
		result = rai::uint128_t (1) << (127 - (25 - interval_exponent)); // 50%
	}
	else if (interval < intervals_per_period * 2)
	{
		result = rai::uint128_t (1) << (126 - (25 - interval_exponent)); // 25%
	}
	else if (interval < intervals_per_period * 3)
	{
		result = rai::uint128_t (1) << (125 - (25 - interval_exponent)); // 13%
	}
	else if (interval < intervals_per_period * 4)
	{
		result = rai::uint128_t (1) << (124 - (25 - interval_exponent)); // 6.3%
	}
	else if (interval < intervals_per_period * 5)
	{
		result = rai::uint128_t (1) << (123 - (25 - interval_exponent)); // 3.1%
	}
	else if (interval < intervals_per_period * 6)
	{
		result = rai::uint128_t (1) << (122 - (25 - interval_exponent)); // 1.6%
	}
	else if (interval < intervals_per_period * 7)
	{
		result = rai::uint128_t (1) << (121 - (25 - interval_exponent)); // 0.8%
	}
	else if (interval < intervals_per_period * 8)
	{
		result = rai::uint128_t (1) << (121 - (25 - interval_exponent)); // 0.8*
	}
	else
	{
		result = 0;
	}
	return result;
}

uint64_t rai::landing::seconds_since_epoch ()
{
	return std::chrono::duration_cast <std::chrono::seconds> (std::chrono::system_clock::now ().time_since_epoch ()).count ();
}

void rai::landing::distribute_one ()
{
	auto now (seconds_since_epoch ());
	rai::block_hash last (1);
	while (!last.is_zero () && store.last + distribution_interval.count () < now)
	{
		auto amount (distribution_amount ((store.last - store.start) >> interval_exponent));
		last = wallet->send_sync (store.source, store.destination, amount);
		if (!last.is_zero ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Successfully distributed %1% in block %2%") % amount % last.to_string ());
			store.last += distribution_interval.count ();
			write_store ();
		}
		else
		{
			BOOST_LOG (node.log) << "Error while sending distribution";
		}
	}
}

void rai::landing::distribute_ongoing ()
{
	distribute_one ();
	BOOST_LOG (node.log) << "Waiting for next distribution cycle";
	node.alarm.add (std::chrono::system_clock::now () + sleep_seconds, [this] () {distribute_ongoing ();});
}


std::chrono::seconds constexpr rai::landing::distribution_interval;
std::chrono::seconds constexpr rai::landing::sleep_seconds;