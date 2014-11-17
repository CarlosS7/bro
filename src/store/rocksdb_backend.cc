#include "rocksdb_backend_impl.hh"
#include "broker/broker.h"
#include <caf/binary_serializer.hpp>
#include <caf/binary_deserializer.hpp>

static std::string version_string()
	{
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%d.%d.%d",
	         BROKER_VERSION_MAJOR, BROKER_VERSION_MINOR, BROKER_VERSION_PATCH);
	return tmp;
	}

template <class T>
static void to_serial(const T& obj, std::string& rval)
	{
	caf::binary_serializer bs(std::back_inserter(rval));
	bs << obj;
	}

template <class T>
static std::string to_serial(const T& obj)
	{
	std::string rval;
	to_serial(obj, rval);
	return rval;
	}

template <class T>
static std::string to_serial(const T& obj, char keyspace)
	{
	std::string rval{keyspace};
	to_serial(obj, rval);
	return rval;
	}

template <class T>
static T from_serial(const char* blob, size_t num_bytes)
	{
	T rval;
	caf::binary_deserializer bd(blob, num_bytes);
	caf::uniform_typeid<T>()->deserialize(&rval, &bd);
	return rval;
	}

template <class T, class C>
static T from_serial(const C& bytes)
	{ return from_serial<T>(bytes.data(), bytes.size()); }

static rocksdb::Status
insert(rocksdb::DB* db, const broker::data& k, const broker::data& v,
       const broker::util::optional<broker::store::expiration_time>& e)
	{
	auto kserial = to_serial(k, 'a');
	auto vserial = to_serial(v);

	if ( ! e )
		return db->Put({}, kserial, vserial);

	auto evserial = to_serial(*e);
	rocksdb::WriteBatch batch;
	batch.Put(kserial, vserial);
	kserial[0] = 'e';
	batch.Put(kserial, evserial);
	return db->Write({}, &batch);
	}

broker::store::rocksdb_backend::rocksdb_backend(uint64_t exact_size_threshold)
	: pimpl(new impl(exact_size_threshold))
	{}

broker::store::rocksdb_backend::~rocksdb_backend() = default;

broker::store::rocksdb_backend::rocksdb_backend(rocksdb_backend&&) = default;

broker::store::rocksdb_backend&
broker::store::rocksdb_backend::operator=(rocksdb_backend&&) = default;

rocksdb::Status
broker::store::rocksdb_backend::open(std::string db_path,
                                     rocksdb::Options options)
	{
	rocksdb::DB* db;
	auto rval = rocksdb::DB::Open(options, db_path, &db);
	pimpl->db.reset(db);
	options.create_if_missing = true;
	pimpl->options = options;

	if ( rval.ok() )
		{
		auto ver = version_string();
		// Use key-space prefix 'm' to store metadata, 'a' for application
		// data, and 'e' for expiration values.
		rval = pimpl->db->Put({}, "mbroker_version", ver);
		return rval;
		}

	return rval;
	}

void broker::store::rocksdb_backend::do_increase_sequence()
	{ ++pimpl->sn; }

std::string broker::store::rocksdb_backend::do_last_error() const
	{ return pimpl->last_error; }

bool broker::store::rocksdb_backend::do_init(snapshot sss)
	{
	if ( ! do_clear() )
		return false;

	rocksdb::WriteBatch batch;

	for ( const auto& kv : sss.datastore )
		{
		auto kserial = to_serial(kv.first, 'a');
		auto vserial = to_serial(kv.second.item);
		batch.Put(kserial, vserial);

		if ( kv.second.expiry )
			{
			kserial[0] = 'e';
			auto evserial = to_serial(*kv.second.expiry);
			batch.Put(kserial, evserial);
			}
		}

	pimpl->sn = std::move(sss.sn);
	return pimpl->require_ok(pimpl->db->Write({}, &batch));
	}

const broker::store::sequence_num&
broker::store::rocksdb_backend::do_sequence() const
	{ return pimpl->sn; }

bool
broker::store::rocksdb_backend::do_insert(data k, data v,
                                          util::optional<expiration_time> e)
	{
	if ( ! pimpl->require_db() )
		return false;

	return pimpl->require_ok(::insert(pimpl->db.get(), k, v, e));
	}

int broker::store::rocksdb_backend::do_increment(const data& k, int64_t by)
	{
	// TODO: merge operator
	auto r = lookup_with_expiry(k);

	if ( ! r )
		return -1;

	auto val = *r;

	if ( ! val )
		{
		if ( pimpl->require_ok(::insert(pimpl->db.get(), k, data{by}, {})) )
			return 0;

		return -1;
		}

	if ( ! visit(detail::increment_visitor{by}, val->item) )
		{
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "attempt to increment non-integral tag %d",
		         static_cast<int>(which(val->item)));
		pimpl->last_error = tmp;
		return 1;
		}

	if ( pimpl->require_ok(::insert(pimpl->db.get(), k, val->item,
	                                val->expiry)) )
		return 0;

	return -1;
	}

int broker::store::rocksdb_backend::do_add_to_set(const data& k, data element)
	{
	// TODO: merge operator
	auto r = lookup_with_expiry(k);

	if ( ! r )
		return -1;

	auto val = *r;

	if ( ! val )
		{
		if ( pimpl->require_ok(::insert(pimpl->db.get(), k,
		                                set{std::move(element)}, {})) )
			return 0;

		return -1;
		}

	broker::set* s = get<broker::set>(val->item);

	if ( ! s )
		{
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "attempt to add to non-set tag %d",
		         static_cast<int>(which(val->item)));
		pimpl->last_error = tmp;
		return 1;
		}

	s->emplace(std::move(element));

	if ( pimpl->require_ok(::insert(pimpl->db.get(), k, val->item,
	                                val->expiry)) )
		return 0;

	return -1;
	}

int broker::store::rocksdb_backend::do_remove_from_set(const data& k,
                                                       const data& element)
	{
	// TODO: merge operator
	auto r = lookup_with_expiry(k);

	if ( ! r )
		return -1;

	auto val = *r;

	if ( ! val )
		{
		if ( pimpl->require_ok(::insert(pimpl->db.get(), k, set{}, {})) )
			return 0;

		return -1;
		}

	broker::set* s = get<broker::set>(val->item);

	if ( ! s )
		{
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "attempt to remove from non-set tag %d",
		         static_cast<int>(which(val->item)));
		pimpl->last_error = tmp;
		return 1;
		}

	s->erase(element);

	if ( pimpl->require_ok(::insert(pimpl->db.get(), k, val->item,
	                                val->expiry)) )
		return 0;

	return -1;
	}

bool broker::store::rocksdb_backend::do_erase(const data& k)
	{
	if ( ! pimpl->require_db() )
		return false;

	auto kserial = to_serial(k, 'a');

	if ( ! pimpl->require_ok(pimpl->db->Delete({}, kserial)) )
		return false;

	kserial[0] = 'e';
	return pimpl->require_ok(pimpl->db->Delete({}, kserial));
	}

bool broker::store::rocksdb_backend::do_clear()
	{
	if ( ! pimpl->require_db() )
		return false;

	std::string db_path = pimpl->db->GetName();
	pimpl->db.reset();
	auto stat = rocksdb::DestroyDB(db_path, rocksdb::Options{});

	if ( ! pimpl->require_ok(stat) )
		return false;

	return pimpl->require_ok(open(std::move(db_path), pimpl->options));
	}

broker::util::optional<broker::util::optional<broker::data>>
broker::store::rocksdb_backend::do_lookup(const data& k) const
	{
	if ( ! pimpl->require_db() )
		return {};

	auto kserial = to_serial(k, 'a');
	std::string vserial;
	bool value_found;

	if ( ! pimpl->db->KeyMayExist({}, kserial, &vserial, &value_found) )
		return util::optional<data>{};

	if ( value_found )
		return {from_serial<data>(vserial)};

	auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);

	if ( stat.IsNotFound() )
		return util::optional<data>{};

	if ( ! pimpl->require_ok(stat) )
		return {};

	return {from_serial<data>(vserial)};
	}

broker::util::optional<bool>
broker::store::rocksdb_backend::do_exists(const data& k) const
	{
	if ( ! pimpl->require_db() )
		return {};

	auto kserial = to_serial(k, 'a');
	std::string vserial;

	if ( ! pimpl->db->KeyMayExist(rocksdb::ReadOptions{}, kserial, &vserial) )
		return false;

	auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);

	if ( stat.IsNotFound() )
		return false;

	if ( ! pimpl->require_ok(stat) )
		return {};

	return true;
	}

broker::util::optional<std::unordered_set<broker::data>>
broker::store::rocksdb_backend::do_keys() const
	{
	if ( ! pimpl->require_db() )
		return {};

	rocksdb::ReadOptions options;
	options.fill_cache = false;
	std::unique_ptr<rocksdb::Iterator> it(pimpl->db->NewIterator(options));
	std::unordered_set<data> rval;

	for ( it->Seek("a"); it->Valid() && it->key().starts_with("a"); it->Next() )
		{
		auto s = it->key();
		s.remove_prefix(1);
		rval.emplace(from_serial<data>(s));
		}

	if ( ! pimpl->require_ok(it->status()) )
		return {};

	return rval;
	}

broker::util::optional<uint64_t> broker::store::rocksdb_backend::do_size() const
	{
	if ( ! pimpl->require_db() )
		return {};

	uint64_t rval;

	if ( pimpl->db->GetIntProperty("rocksdb.estimate-num-keys", &rval) &&
	     rval > pimpl->exact_size_threshold )
		return rval;

	rocksdb::ReadOptions options;
	options.fill_cache = false;
	std::unique_ptr<rocksdb::Iterator> it(pimpl->db->NewIterator(options));
	rval = 0;

	for ( it->Seek("a"); it->Valid() && it->key().starts_with("a"); it->Next() )
		++rval;

	if ( pimpl->require_ok(it->status()) )
		return rval;

	return {};
	}

broker::util::optional<broker::store::snapshot>
broker::store::rocksdb_backend::do_snap() const
	{
	if ( ! pimpl->require_db() )
		return {};

	rocksdb::ReadOptions options;
	options.fill_cache = false;
	std::unique_ptr<rocksdb::Iterator> it(pimpl->db->NewIterator(options));
	snapshot rval;
	rval.sn = pimpl->sn;

	for ( it->Seek("a"); it->Valid() && it->key().starts_with("a"); it->Next() )
		{
		auto ks = it->key();
		auto vs = it->value();
		ks.remove_prefix(1);
		rval.datastore.emplace(from_serial<data>(ks),
		                       value{from_serial<data>(vs)});
		}

	if ( ! pimpl->require_ok(it->status()) )
		return {};

	for ( it->Seek("e"); it->Valid() && it->key().starts_with("e"); it->Next() )
		{
		auto ks = it->key();
		auto vs = it->value();
		ks.remove_prefix(1);
		auto key = from_serial<data>(ks);
		rval.datastore[key].expiry = from_serial<expiration_time>(vs);
		}

	if ( ! pimpl->require_ok(it->status()) )
		return {};

	return rval;
	}

broker::util::optional<std::deque<broker::store::expirable>>
broker::store::rocksdb_backend::do_expiries() const
	{
	if ( ! pimpl->require_db() )
		return {};

	rocksdb::ReadOptions options;
	options.fill_cache = false;
	std::unique_ptr<rocksdb::Iterator> it(pimpl->db->NewIterator(options));
	std::deque<expirable> rval;

	for ( it->Seek("e"); it->Valid() && it->key().starts_with("e"); it->Next() )
		{
		auto ks = it->key();
		auto vs = it->value();
		ks.remove_prefix(1);
		auto key = from_serial<data>(ks);
		auto expiry = from_serial<expiration_time>(vs);
		rval.emplace_back(expirable{std::move(key), std::move(expiry)});
		}

	if ( ! pimpl->require_ok(it->status()) )
		return {};

	return rval;
	}

broker::util::optional<broker::util::optional<broker::store::value>>
broker::store::rocksdb_backend::lookup_with_expiry(const data& k) const
	{
	if ( ! pimpl->require_db() )
		return {};

	auto kserial = to_serial(k, 'a');
	std::string vserial;
	bool value_found;

	if ( ! pimpl->db->KeyMayExist({}, kserial, &vserial, &value_found) )
		return util::optional<value>{};

	if ( ! value_found )
		{
		auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);

		if ( stat.IsNotFound() )
			return util::optional<value>{};

		if ( ! pimpl->require_ok(stat) )
			return {};
		}

	kserial[0] = 'e';
	value rval{from_serial<data>(vserial)};

	if ( ! pimpl->db->KeyMayExist({}, kserial, &vserial, &value_found) )
		return {rval};

	if ( ! value_found )
		{
		auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);

		if ( stat.IsNotFound() )
			return {rval};

		if ( ! pimpl->require_ok(stat) )
			return {};
		}

	rval.expiry = from_serial<expiration_time>(vserial);
	return {rval};
	}
