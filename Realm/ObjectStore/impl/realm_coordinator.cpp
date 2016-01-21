////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "realm_coordinator.hpp"

#include "async_query.hpp"
#include "cached_realm.hpp"
#include "external_commit_helper.hpp"
#include "object_store.hpp"
#include "transact_log_handler.hpp"
#include "index_set.hpp"

#include <realm/commit_log.hpp>
#include <realm/group_shared.hpp>
#include <realm/lang_bind_helper.hpp>
#include <realm/query.hpp>
#include <realm/table_view.hpp>

#include <cassert>
#include <set>
#include <unordered_map>

using namespace realm;
using namespace realm::_impl;

namespace {
// A transaction log handler that just validates that all operations made are
// ones supported by the object store
class TransactLogValidator {
    // Index of currently selected table
    size_t m_current_table = 0;

    // Tables which were created during the transaction being processed, which
    // can have columns inserted without a schema version bump
    std::vector<size_t> m_new_tables;

    REALM_NORETURN
    REALM_NOINLINE
    void schema_error()
    {
        throw std::runtime_error("Schema mismatch detected: another process has modified the Realm file's schema in an incompatible way");
    }

    // Throw an exception if the currently modified table already existed before
    // the current set of modifications
    bool schema_error_unless_new_table()
    {
        if (std::find(begin(m_new_tables), end(m_new_tables), m_current_table) == end(m_new_tables)) {
            schema_error();
        }
        return true;
    }

protected:
    size_t current_table() const noexcept { return m_current_table; }

public:
    // Schema changes which don't involve a change in the schema version are
    // allowed
    bool add_search_index(size_t) { return true; }
    bool remove_search_index(size_t) { return true; }

    // Creating entirely new tables without a schema version bump is allowed, so
    // we need to track if new columns are being added to a new table or an
    // existing one
    bool insert_group_level_table(size_t table_ndx, size_t, StringData)
    {
        // Shift any previously added tables after the new one
        for (auto& table : m_new_tables) {
            if (table >= table_ndx)
                ++table;
        }
        m_new_tables.push_back(table_ndx);
        return true;
    }
    bool insert_column(size_t, DataType, StringData, bool) { return schema_error_unless_new_table(); }
    bool insert_link_column(size_t, DataType, StringData, size_t, size_t) { return schema_error_unless_new_table(); }
    bool add_primary_key(size_t) { return schema_error_unless_new_table(); }
    bool set_link_type(size_t, LinkType) { return schema_error_unless_new_table(); }

    // Removing or renaming things while a Realm is open is never supported
    bool erase_group_level_table(size_t, size_t) { schema_error(); }
    bool rename_group_level_table(size_t, StringData) { schema_error(); }
    bool erase_column(size_t) { schema_error(); }
    bool erase_link_column(size_t, size_t, size_t) { schema_error(); }
    bool rename_column(size_t, StringData) { schema_error(); }
    bool remove_primary_key() { schema_error(); }
    bool move_column(size_t, size_t) { schema_error(); }
    bool move_group_level_table(size_t, size_t) { schema_error(); }

    bool select_descriptor(int levels, const size_t*)
    {
        // subtables not supported
        return levels == 0;
    }

    bool select_table(size_t group_level_ndx, int, const size_t*) noexcept
    {
        m_current_table = group_level_ndx;
        return true;
    }

    bool select_link_list(size_t, size_t, size_t) { return true; }

    // Non-schema changes are all allowed
    void parse_complete() { }
    bool insert_empty_rows(size_t, size_t, size_t, bool) { return true; }
    bool erase_rows(size_t, size_t, size_t, bool) { return true; }
    bool swap_rows(size_t, size_t) { return true; }
    bool clear_table() noexcept { return true; }
    bool link_list_set(size_t, size_t) { return true; }
    bool link_list_insert(size_t, size_t) { return true; }
    bool link_list_erase(size_t) { return true; }
    bool link_list_nullify(size_t) { return true; }
    bool link_list_clear(size_t) { return true; }
    bool link_list_move(size_t, size_t) { return true; }
    bool link_list_swap(size_t, size_t) { return true; }
    bool set_int(size_t, size_t, int_fast64_t) { return true; }
    bool set_bool(size_t, size_t, bool) { return true; }
    bool set_float(size_t, size_t, float) { return true; }
    bool set_double(size_t, size_t, double) { return true; }
    bool set_string(size_t, size_t, StringData) { return true; }
    bool set_binary(size_t, size_t, BinaryData) { return true; }
    bool set_date_time(size_t, size_t, DateTime) { return true; }
    bool set_table(size_t, size_t) { return true; }
    bool set_mixed(size_t, size_t, const Mixed&) { return true; }
    bool set_link(size_t, size_t, size_t, size_t) { return true; }
    bool set_null(size_t, size_t) { return true; }
    bool nullify_link(size_t, size_t, size_t) { return true; }
    bool insert_substring(size_t, size_t, size_t, StringData) { return true; }
    bool erase_substring(size_t, size_t, size_t, size_t) { return true; }
    bool optimize_table() { return true; }
    bool set_int_unique(size_t, size_t, int_fast64_t) { return true; }
    bool set_string_unique(size_t, size_t, StringData) { return true; }
};

// Extends TransactLogValidator to also track changes and report it to the
// binding context if any properties are being observed
class TransactLogObserver : public TransactLogValidator {
    ChangeInfo& get_change(size_t i)
    {
        if (m_changes.size() <= i) {
            m_changes.resize(std::max(m_changes.size() * 2, i + 1));
        }
        return m_changes[i];
    }

    bool mark_dirty(size_t row, __unused size_t col)
    {
        auto& table = get_change(current_table());
        auto it = table.moves.find(row);
        if (it != end(table.moves)) {
            row = it->second;
        }
        table.changed.insert(row);

        return true;
    }

    struct LinkListInfo {
        size_t table_ndx;
        size_t row_ndx;
        size_t col_ndx;

        IndexSet inserts;
        IndexSet deletes;
        IndexSet changes;
        std::vector<std::pair<size_t, size_t>> moves;
        bool did_clear = false;
    };

    // Change information for the currently selected LinkList, if any
    LinkListInfo* m_active_linklist = nullptr;
    std::vector<LinkListInfo> m_observered_linkviews;

public:
    std::vector<ChangeInfo> m_changes;

    void parse_complete()
    {
    }

    bool insert_group_level_table(size_t, size_t, StringData)
    {
        return false;
    }

    bool insert_empty_rows(size_t, size_t, size_t, bool)
    {
        // rows are only inserted at the end, so no need to do anything
        return true;
    }

    bool erase_rows(size_t row_ndx, size_t, size_t prior_num_rows, bool unordered)
    {
        REALM_ASSERT(unordered);

        auto& table = get_change(current_table());
        auto last_row_ndx = prior_num_rows - 1;
        auto it = table.moves.find(last_row_ndx);
        if (it != end(table.moves)) {
            last_row_ndx = it->second;
        }
        table.moves[row_ndx] = last_row_ndx;
        ++table.deletions;

        return true;
    }

    bool clear_table()
    {
        return true;
    }

    bool select_link_list(size_t col, size_t row, size_t)
    {
        m_active_linklist = nullptr;
        for (auto& o : m_observered_linkviews) {
            if (o.table_ndx == current_table() && o.row_ndx == row && o.col_ndx == col) {
                m_active_linklist = &o;
                break;
            }
        }
        return true;
    }

    bool link_list_set(size_t index, size_t)
    {
        if (!m_active_linklist)
            return true;

        m_active_linklist->changes.add(index);
        return true;
    }

    bool link_list_insert(size_t index, size_t)
    {
        if (!m_active_linklist)
            return true;

        m_active_linklist->changes.shift_for_insert_at(index);
        m_active_linklist->inserts.insert_at(index);

        for (auto& move : m_active_linklist->moves) {
            if (move.second >= index)
                ++move.second;
        }

        return true;
    }

    bool link_list_erase(size_t index)
    {
        if (!m_active_linklist)
            return true;

        m_active_linklist->changes.erase_at(index);
        m_active_linklist->inserts.erase_at(index);
        // this is probably wrong for mixed insert/delete
        m_active_linklist->deletes.add_shifted(m_active_linklist->inserts.unshift(index));

        for (size_t i = 0; i < m_active_linklist->moves.size(); ++i) {
            auto& move = m_active_linklist->moves[i];
            if (move.second == index) {
                m_active_linklist->moves.erase(m_active_linklist->moves.begin() + i);
                --i;
            }
            else if (move.second > index)
                --move.second;
        }

        return true;
    }

    bool link_list_nullify(size_t index)
    {
        return link_list_erase(index);
    }

    bool link_list_swap(size_t index1, size_t index2)
    {
        link_list_set(index1, 0);
        link_list_set(index2, 0);
        return true;
    }

    bool link_list_clear(size_t /*old_size*/)
    {
        if (!m_active_linklist)
            return true;

        m_active_linklist->did_clear = true;
        m_active_linklist->changes.clear();
        m_active_linklist->inserts.clear();
        m_active_linklist->deletes.clear();
        m_active_linklist->moves.clear();

        return true;
    }

    bool link_list_move(size_t from, size_t to)
    {
        if (!m_active_linklist)
            return true;

        bool sadhkljh = from < to;

        from = m_active_linklist->inserts.unshift(from);
        from = m_active_linklist->deletes.unshift(from);

        // needs to shift prev moves
        m_active_linklist->moves.push_back({from, to});

        if (sadhkljh) {
            m_active_linklist->changes.erase_at(from);
            m_active_linklist->inserts.erase_at(from);
            m_active_linklist->deletes.add(from);

            m_active_linklist->changes.shift_for_insert_at(from);
            m_active_linklist->inserts.shift_for_insert_at(from);
        }
        else {
            m_active_linklist->changes.shift_for_insert_at(from);
            m_active_linklist->inserts.shift_for_insert_at(from);

            m_active_linklist->changes.erase_at(from);
            m_active_linklist->inserts.erase_at(from);
            m_active_linklist->deletes.add(from);
        }

        return true;
    }

    // Things that just mark the field as modified
    bool set_int(size_t col, size_t row, int_fast64_t) { return mark_dirty(row, col); }
    bool set_bool(size_t col, size_t row, bool) { return mark_dirty(row, col); }
    bool set_float(size_t col, size_t row, float) { return mark_dirty(row, col); }
    bool set_double(size_t col, size_t row, double) { return mark_dirty(row, col); }
    bool set_string(size_t col, size_t row, StringData) { return mark_dirty(row, col); }
    bool set_binary(size_t col, size_t row, BinaryData) { return mark_dirty(row, col); }
    bool set_date_time(size_t col, size_t row, DateTime) { return mark_dirty(row, col); }
    bool set_table(size_t col, size_t row) { return mark_dirty(row, col); }
    bool set_mixed(size_t col, size_t row, const Mixed&) { return mark_dirty(row, col); }
    bool set_link(size_t col, size_t row, size_t, size_t) { return mark_dirty(row, col); }
    bool set_null(size_t col, size_t row) { return mark_dirty(row, col); }
    bool nullify_link(size_t col, size_t row, size_t) { return mark_dirty(row, col); }
    bool insert_substring(size_t col, size_t row, size_t, StringData) { return mark_dirty(row, col); }
    bool erase_substring(size_t col, size_t row, size_t, size_t) { return mark_dirty(row, col); }
    bool set_int_unique(size_t col, size_t row, int_fast64_t) { return mark_dirty(row, col); }
    bool set_string_unique(size_t col, size_t row, StringData) { return mark_dirty(row, col); }
};
} // anonymous namespace

static std::mutex s_coordinator_mutex;
static std::unordered_map<std::string, std::weak_ptr<RealmCoordinator>> s_coordinators_per_path;

std::shared_ptr<RealmCoordinator> RealmCoordinator::get_coordinator(StringData path)
{
    std::lock_guard<std::mutex> lock(s_coordinator_mutex);

    auto& weak_coordinator = s_coordinators_per_path[path];
    if (auto coordinator = weak_coordinator.lock()) {
        return coordinator;
    }

    auto coordinator = std::make_shared<RealmCoordinator>();
    weak_coordinator = coordinator;
    return coordinator;
}

std::shared_ptr<RealmCoordinator> RealmCoordinator::get_existing_coordinator(StringData path)
{
    std::lock_guard<std::mutex> lock(s_coordinator_mutex);
    auto it = s_coordinators_per_path.find(path);
    return it == s_coordinators_per_path.end() ? nullptr : it->second.lock();
}

std::shared_ptr<Realm> RealmCoordinator::get_realm(Realm::Config config)
{
    std::lock_guard<std::mutex> lock(m_realm_mutex);
    if ((!m_config.read_only && !m_notifier) || (m_config.read_only && m_cached_realms.empty())) {
        m_config = config;
        if (!config.read_only && !m_notifier) {
            try {
                m_notifier = std::make_unique<ExternalCommitHelper>(*this);
            }
            catch (std::system_error const& ex) {
                throw RealmFileException(RealmFileException::Kind::AccessError, config.path, ex.code().message());
            }
        }
    }
    else {
        if (m_config.read_only != config.read_only) {
            throw MismatchedConfigException("Realm at path already opened with different read permissions.");
        }
        if (m_config.in_memory != config.in_memory) {
            throw MismatchedConfigException("Realm at path already opened with different inMemory settings.");
        }
        if (m_config.encryption_key != config.encryption_key) {
            throw MismatchedConfigException("Realm at path already opened with a different encryption key.");
        }
        if (m_config.schema_version != config.schema_version && config.schema_version != ObjectStore::NotVersioned) {
            throw MismatchedConfigException("Realm at path already opened with different schema version.");
        }
        // FIXME: verify that schema is compatible
        // Needs to verify that all tables present in both are identical, and
        // then updated m_config with any tables present in config but not in
        // it
        // Public API currently doesn't make it possible to have non-matching
        // schemata so it's not a huge issue
        if ((false) && m_config.schema != config.schema) {
            throw MismatchedConfigException("Realm at path already opened with different schema");
        }
    }

    if (config.cache) {
        for (auto& cachedRealm : m_cached_realms) {
            if (cachedRealm.is_cached_for_current_thread()) {
                // can be null if we jumped in between ref count hitting zero and
                // unregister_realm() getting the lock
                if (auto realm = cachedRealm.realm()) {
                    return realm;
                }
            }
        }
    }

    auto realm = std::make_shared<Realm>(config);
    realm->init(shared_from_this());
    m_cached_realms.emplace_back(realm, m_config.cache);
    return realm;
}

std::shared_ptr<Realm> RealmCoordinator::get_realm()
{
    return get_realm(m_config);
}

const Schema* RealmCoordinator::get_schema() const noexcept
{
    return m_cached_realms.empty() ? nullptr : m_config.schema.get();
}

RealmCoordinator::RealmCoordinator() = default;

RealmCoordinator::~RealmCoordinator()
{
    std::lock_guard<std::mutex> coordinator_lock(s_coordinator_mutex);
    for (auto it = s_coordinators_per_path.begin(); it != s_coordinators_per_path.end(); ) {
        if (it->second.expired()) {
            it = s_coordinators_per_path.erase(it);
        }
        else {
            ++it;
        }
    }
}

void RealmCoordinator::unregister_realm(Realm* realm)
{
    std::lock_guard<std::mutex> lock(m_realm_mutex);
    for (size_t i = 0; i < m_cached_realms.size(); ++i) {
        auto& cached_realm = m_cached_realms[i];
        if (!cached_realm.expired() && !cached_realm.is_for_realm(realm)) {
            continue;
        }

        if (i + 1 < m_cached_realms.size()) {
            cached_realm = std::move(m_cached_realms.back());
        }
        m_cached_realms.pop_back();
    }
}

void RealmCoordinator::clear_cache()
{
    std::vector<WeakRealm> realms_to_close;
    {
        std::lock_guard<std::mutex> lock(s_coordinator_mutex);

        for (auto& weak_coordinator : s_coordinators_per_path) {
            auto coordinator = weak_coordinator.second.lock();
            if (!coordinator) {
                continue;
            }

            coordinator->m_notifier = nullptr;

            // Gather a list of all of the realms which will be removed
            for (auto& cached_realm : coordinator->m_cached_realms) {
                if (auto realm = cached_realm.realm()) {
                    realms_to_close.push_back(realm);
                }
            }
        }

        s_coordinators_per_path.clear();
    }

    // Close all of the previously cached Realms. This can't be done while
    // s_coordinator_mutex is held as it may try to re-lock it.
    for (auto& weak_realm : realms_to_close) {
        if (auto realm = weak_realm.lock()) {
            realm->close();
        }
    }
}

void RealmCoordinator::send_commit_notifications()
{
    REALM_ASSERT(!m_config.read_only);
    m_notifier->notify_others();
}

void RealmCoordinator::pin_version(uint_fast64_t version, uint_fast32_t index)
{
    if (m_async_error) {
        return;
    }

    SharedGroup::VersionID versionid(version, index);
    if (!m_advancer_sg) {
        try {
            std::unique_ptr<Group> read_only_group;
            Realm::open_with_config(m_config, m_advancer_history, m_advancer_sg, read_only_group);
            REALM_ASSERT(!read_only_group);
            m_advancer_sg->begin_read(versionid);
        }
        catch (...) {
            m_async_error = std::current_exception();
            m_advancer_sg = nullptr;
            m_advancer_history = nullptr;
        }
    }
    else if (m_new_queries.empty()) {
        // If this is the first query then we don't already have a read transaction
        m_advancer_sg->begin_read(versionid);
    }
    else if (versionid < m_advancer_sg->get_version_of_current_transaction()) {
        // Ensure we're holding a readlock on the oldest version we have a
        // handover object for, as handover objects don't
        m_advancer_sg->end_read();
        m_advancer_sg->begin_read(versionid);
    }
}

void RealmCoordinator::register_query(std::shared_ptr<AsyncQuery> query)
{
    auto version = query->version();
    auto& self = Realm::Internal::get_coordinator(query->get_realm());
    {
        std::lock_guard<std::mutex> lock(self.m_query_mutex);
        self.pin_version(version.version, version.index);
        self.m_new_queries.push_back(std::move(query));
    }
}

void RealmCoordinator::clean_up_dead_queries()
{
    auto swap_remove = [&](auto& container) {
        bool did_remove = false;
        for (size_t i = 0; i < container.size(); ++i) {
            if (container[i]->is_alive())
                continue;

            // Ensure the query is destroyed here even if there's lingering refs
            // to the async query elsewhere
            container[i]->release_query();

            if (container.size() > i + 1)
                container[i] = std::move(container.back());
            container.pop_back();
            --i;
            did_remove = true;
        }
        return did_remove;
    };

    if (swap_remove(m_queries)) {
        // Make sure we aren't holding on to read versions needlessly if there
        // are no queries left, but don't close them entirely as opening shared
        // groups is expensive
        if (m_queries.empty() && m_query_sg) {
            m_query_sg->end_read();
        }
    }
    if (swap_remove(m_new_queries)) {
        if (m_new_queries.empty() && m_advancer_sg) {
            m_advancer_sg->end_read();
        }
    }
}

void RealmCoordinator::on_change()
{
    run_async_queries();

    std::lock_guard<std::mutex> lock(m_realm_mutex);
    for (auto& realm : m_cached_realms) {
        realm.notify();
    }
}

void RealmCoordinator::run_async_queries()
{
    std::unique_lock<std::mutex> lock(m_query_mutex);

    clean_up_dead_queries();

    if (m_queries.empty() && m_new_queries.empty()) {
        return;
    }

    if (!m_async_error) {
        open_helper_shared_group();
    }

    if (m_async_error) {
        move_new_queries_to_main();
        return;
    }

    TransactLogObserver obs;
    advance_helper_shared_group_to_latest(obs);

    // Make a copy of the queries vector so that we can release the lock while
    // we run the queries
    auto queries_to_run = m_queries;
    lock.unlock();

    for (auto& query : queries_to_run) {
        query->run(obs.m_changes);
    }

    // Reacquire the lock while updating the fields that are actually read on
    // other threads
    {
        lock.lock();
        for (auto& query : queries_to_run) {
            query->prepare_handover();
        }
    }

    clean_up_dead_queries();
}

void RealmCoordinator::open_helper_shared_group()
{
    if (!m_query_sg) {
        try {
            std::unique_ptr<Group> read_only_group;
            Realm::open_with_config(m_config, m_query_history, m_query_sg, read_only_group);
            REALM_ASSERT(!read_only_group);
            m_query_sg->begin_read();
        }
        catch (...) {
            // Store the error to be passed to the async queries
            m_async_error = std::current_exception();
            m_query_sg = nullptr;
            m_query_history = nullptr;
        }
    }
    else if (m_queries.empty()) {
        m_query_sg->begin_read();
    }
}

void RealmCoordinator::move_new_queries_to_main()
{
    m_queries.reserve(m_queries.size() + m_new_queries.size());
    std::move(m_new_queries.begin(), m_new_queries.end(), std::back_inserter(m_queries));
    m_new_queries.clear();
}

void RealmCoordinator::advance_helper_shared_group_to_latest(TransactLogObserver& obs)
{
    if (m_new_queries.empty()) {
        LangBindHelper::advance_read(*m_query_sg, *m_query_history, obs);
        return;
    }

    // Sort newly added queries by their source version so that we can pull them
    // all forward to the latest version in a single pass over the transaction log
    std::sort(m_new_queries.begin(), m_new_queries.end(), [](auto const& lft, auto const& rgt) {
        return lft->version() < rgt->version();
    });

    // Import all newly added queries to our helper SG
    for (auto& query : m_new_queries) {
        LangBindHelper::advance_read(*m_advancer_sg, *m_advancer_history, query->version());
        query->attach_to(*m_advancer_sg);
    }

    // Advance both SGs to the newest version
    LangBindHelper::advance_read(*m_advancer_sg, *m_advancer_history);
    LangBindHelper::advance_read(*m_query_sg, *m_query_history, obs,
                                 m_advancer_sg->get_version_of_current_transaction());

    // Transfer all new queries over to the main SG
    for (auto& query : m_new_queries) {
        query->detatch();
        query->attach_to(*m_query_sg);
    }

    move_new_queries_to_main();
    m_advancer_sg->end_read();
}

void RealmCoordinator::advance_to_ready(Realm& realm)
{
    decltype(m_queries) queries;

    auto& sg = Realm::Internal::get_shared_group(realm);
    auto& history = Realm::Internal::get_history(realm);

    {
        std::lock_guard<std::mutex> lock(m_query_mutex);

        SharedGroup::VersionID version;
        for (auto& query : m_queries) {
            version = query->version();
            if (version != SharedGroup::VersionID()) {
                break;
            }
        }

        // no untargeted async queries; just advance to latest
        if (version.version == 0) {
            transaction::advance(sg, history, realm.m_binding_context.get());
            return;
        }
        // async results are out of date; ignore
        else if (version < sg.get_version_of_current_transaction()) {
            return;
        }

        transaction::advance(sg, history, realm.m_binding_context.get(), version);

        for (auto& query : m_queries) {
            if (query->deliver(sg, m_async_error)) {
                queries.push_back(query);
            }
        }
    }

    for (auto& query : queries) {
        query->call_callbacks();
    }
}

void RealmCoordinator::process_available_async(Realm& realm)
{
    auto& sg = Realm::Internal::get_shared_group(realm);
    decltype(m_queries) queries;
    {
        std::lock_guard<std::mutex> lock(m_query_mutex);
        for (auto& query : m_queries) {
            if (query->deliver(sg, m_async_error)) {
                queries.push_back(query);
            }
        }
    }

    for (auto& query : queries) {
        query->call_callbacks();
    }
}
