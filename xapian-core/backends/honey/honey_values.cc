/** @file honey_values.cc
 * @brief HoneyValueManager class
 */
/* Copyright (C) 2008,2009,2010,2011,2012,2016,2017,2018 Olly Betts
 * Copyright (C) 2008,2009 Lemur Consulting Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <config.h>

#include "honey_values.h"

#include "honey_cursor.h"
#include "honey_postlist.h"
#include "honey_postlisttable.h"
#include "honey_termlist.h"
#include "honey_termlisttable.h"

#include "bitstream.h"
#include "debuglog.h"
#include "backends/documentinternal.h"
#include "pack.h"

#include "xapian/error.h"
#include "xapian/valueiterator.h"

#include <algorithm>
#include <memory>

using namespace Honey;
using namespace std;

// FIXME:
//  * multi-values?
//  * values named instead of numbered?

void
ValueChunkReader::assign(const char * p_, size_t len, Xapian::docid did_)
{
    p = p_;
    end = p_ + len;
    did = did_;
    if (!unpack_string(&p, end, value))
	throw Xapian::DatabaseCorruptError("Failed to unpack first value");
}

void
ValueChunkReader::next()
{
    if (p == end) {
	p = NULL;
	return;
    }

    Xapian::docid delta;
    if (!unpack_uint(&p, end, &delta)) {
	throw Xapian::DatabaseCorruptError("Failed to unpack streamed value "
					   "docid");
    }
    did += delta + 1;
    if (!unpack_string(&p, end, value))
	throw Xapian::DatabaseCorruptError("Failed to unpack streamed value");
}

void
ValueChunkReader::skip_to(Xapian::docid target)
{
    if (p == NULL || target <= did)
	return;

    size_t value_len;
    while (p != end) {
	// Get the next docid
	Xapian::docid delta;
	if (rare(!unpack_uint(&p, end, &delta))) {
	    throw Xapian::DatabaseCorruptError("Failed to unpack streamed "
					       "value docid");
	}
	did += delta + 1;

	// Get the length of the string
	if (rare(!unpack_uint(&p, end, &value_len))) {
	    throw Xapian::DatabaseCorruptError("Failed to unpack streamed "
					       "value length");
	}

	// Check that it's not too long
	if (rare(value_len > size_t(end - p))) {
	    throw Xapian::DatabaseCorruptError("Failed to unpack streamed "
					       "value");
	}

	// Assign the value and return only if we've reached the target
	if (did >= target) {
	    value.assign(p, value_len);
	    p += value_len;
	    return;
	}
	p += value_len;
    }
    p = NULL;
}

void
HoneyValueManager::add_value(Xapian::docid did, Xapian::valueno slot,
			     const string & val)
{
    map<Xapian::valueno, map<Xapian::docid, string> >::iterator i;
    i = changes.find(slot);
    if (i == changes.end()) {
	i = changes.insert(make_pair(slot, map<Xapian::docid, string>())).first;
    }
    i->second[did] = val;
}

void
HoneyValueManager::remove_value(Xapian::docid did, Xapian::valueno slot)
{
    map<Xapian::valueno, map<Xapian::docid, string> >::iterator i;
    i = changes.find(slot);
    if (i == changes.end()) {
	i = changes.insert(make_pair(slot, map<Xapian::docid, string>())).first;
    }
    i->second[did] = string();
}

Xapian::docid
HoneyValueManager::get_chunk_containing_did(Xapian::valueno slot,
					    Xapian::docid did,
					    string &chunk) const
{
    LOGCALL(DB, Xapian::docid, "HoneyValueManager::get_chunk_containing_did", slot | did | chunk);
    if (!cursor.get())
	cursor.reset(postlist_table.cursor_get());
    if (!cursor.get()) RETURN(0);

    bool exact = cursor->find_entry(make_valuechunk_key(slot, did));
    if (!exact) {
	// If we didn't find a chunk starting with docid did, then we need
	// to check if the chunk contains did.
	const char * p = cursor->current_key.data();
	const char * end = p + cursor->current_key.size();

	// Check that it is a value stream chunk.
	if (end - p < 2 || *p++ != '\0' || *p++ != '\xd8') RETURN(0);

	// Check that it's for the right value slot.
	Xapian::valueno v;
	if (!unpack_uint(&p, end, &v)) {
	    throw Xapian::DatabaseCorruptError("Bad value key");
	}
	if (v != slot) RETURN(0);

	// And get the first docid for the chunk so we can return it.
	if (!unpack_uint_preserving_sort(&p, end, &did) || p != end) {
	    throw Xapian::DatabaseCorruptError("Bad value key");
	}
    }

    cursor->read_tag();
    chunk = cursor->current_tag;
    // FIXME: fails, not sure why: swap(chunk, cursor->current_tag);

    RETURN(did);
}

static const size_t CHUNK_SIZE_THRESHOLD = 2000;

namespace Honey {

class ValueUpdater {
    HoneyPostListTable& table;

    Xapian::valueno slot;

    string ctag;

    ValueChunkReader reader;

    string tag;

    Xapian::docid prev_did;

    Xapian::docid first_did;

    Xapian::docid new_first_did;

    Xapian::docid last_allowed_did;

    void append_to_stream(Xapian::docid did, const string & value) {
	Assert(did);
	if (tag.empty()) {
	    new_first_did = did;
	} else {
	    AssertRel(did,>,prev_did);
	    pack_uint(tag, did - prev_did - 1);
	}
	prev_did = did;
	pack_string(tag, value);
	if (tag.size() >= CHUNK_SIZE_THRESHOLD) write_tag();
    }

    void write_tag() {
	// If the first docid has changed, delete the old entry.
	if (first_did && new_first_did != first_did) {
	    table.del(make_valuechunk_key(slot, first_did));
	}
	if (!tag.empty()) {
	    table.add(make_valuechunk_key(slot, new_first_did), tag);
	}
	first_did = 0;
	tag.resize(0);
    }

  public:
    ValueUpdater(HoneyPostListTable& table_, Xapian::valueno slot_)
	: table(table_), slot(slot_), first_did(0), last_allowed_did(0) { }

    ~ValueUpdater() {
	while (!reader.at_end()) {
	    // FIXME: use skip_to and some splicing magic instead?
	    append_to_stream(reader.get_docid(), reader.get_value());
	    reader.next();
	}
	write_tag();
    }

    void update(Xapian::docid did, const string & value) {
	if (last_allowed_did && did > last_allowed_did) {
	    // The next change needs to go in a later existing chunk than the
	    // one we're currently updating, so we copy over the rest of the
	    // entries from the current chunk, write out the updated chunk and
	    // drop through to the case below will read in that later chunk.
	    // FIXME: use some string splicing magic instead of this loop.
	    while (!reader.at_end()) {
		// last_allowed_did should be an upper bound for this chunk.
		AssertRel(reader.get_docid(),<=,last_allowed_did);
		append_to_stream(reader.get_docid(), reader.get_value());
		reader.next();
	    }
	    write_tag();
	    last_allowed_did = 0;
	}
	if (last_allowed_did == 0) {
	    last_allowed_did = HONEY_MAX_DOCID;
	    Assert(tag.empty());
	    new_first_did = 0;
	    unique_ptr<HoneyCursor> cursor(table.cursor_get());
	    if (cursor->find_entry(make_valuechunk_key(slot, did))) {
		// We found an exact match, so the first docid is the one
		// we looked for.
		first_did = did;
	    } else {
		Assert(!cursor->after_end());
		// Otherwise we need to unpack it from the key we found.
		// We may have found a non-value-chunk entry in which case
		// docid_from_key() returns 0.
		first_did = docid_from_key(slot, cursor->current_key);
	    }

	    // If there are no further chunks, then the last docid that can go
	    // in this chunk is the highest valid docid.  If there are further
	    // chunks then it's one less than the first docid of the next
	    // chunk.
	    if (first_did) {
		// We found a value chunk.
		cursor->read_tag();
		// FIXME:swap(cursor->current_tag, ctag);
		ctag = cursor->current_tag;
		reader.assign(ctag.data(), ctag.size(), first_did);
	    }
	    if (cursor->next()) {
		const string & key = cursor->current_key;
		Xapian::docid next_first_did = docid_from_key(slot, key);
		if (next_first_did) last_allowed_did = next_first_did - 1;
		Assert(last_allowed_did);
		AssertRel(last_allowed_did,>=,first_did);
	    }
	}

	// Copy over entries until we get to the one we want to
	// add/modify/delete.
	// FIXME: use skip_to and some splicing magic instead?
	while (!reader.at_end() && reader.get_docid() < did) {
	    append_to_stream(reader.get_docid(), reader.get_value());
	    reader.next();
	}
	if (!reader.at_end() && reader.get_docid() == did) reader.next();
	if (!value.empty()) {
	    // Add/update entry for did.
	    append_to_stream(did, value);
	}
    }
};

}

void
HoneyValueManager::merge_changes()
{
    for (auto&& i : changes) {
	Xapian::valueno slot = i.first;
	Honey::ValueUpdater updater(postlist_table, slot);
	for (auto&& j : i.second) {
	    updater.update(j.first, j.second);
	}
    }
    changes.clear();
}

string
HoneyValueManager::add_document(Xapian::docid did, const Xapian::Document &doc,
				map<Xapian::valueno, ValueStats> &val_stats)
{
    Xapian::ValueIterator it = doc.values_begin();
    if (it == doc.values_end()) {
	// No document values.
	auto i = slots.find(did);
	if (i != slots.end()) {
	    // Document's values already added or modified in this batch.
	    i->second = string();
	}
	return string();
    }

    Xapian::valueno count = doc.internal->values_count();
    Xapian::VecCOW<Xapian::termpos> slotvec(count);

    Xapian::valueno first_slot = it.get_valueno();
    Xapian::valueno last_slot = first_slot;
    while (it != doc.values_end()) {
	Xapian::valueno slot = it.get_valueno();
	slotvec.push_back(slot);
	const string& value = *it;

	// Update the statistics.
	auto i = val_stats.insert(make_pair(slot, ValueStats()));
	ValueStats& stats = i.first->second;
	if (i.second) {
	    // There were no statistics stored already, so read them.
	    get_value_stats(slot, stats);
	}

	// Now, modify the stored statistics.
	if ((stats.freq)++ == 0) {
	    // If the value count was previously zero, set the upper and lower
	    // bounds to the newly added value.
	    stats.lower_bound = value;
	    stats.upper_bound = value;
	} else {
	    // Otherwise, simply make sure they reflect the new value.
	    //
	    // Check the upper bound first, as for some common uses of value
	    // slots (dates) the values will tend to get larger not smaller
	    // over time.
	    int cmp = value.compare(stats.upper_bound);
	    if (cmp >= 0) {
		if (cmp > 0) stats.upper_bound = value;
	    } else if (value < stats.lower_bound) {
		stats.lower_bound = value;
	    }
	}

	add_value(did, slot, value);
	last_slot = slot;
	++it;
    }

    if (!termlist_table.is_open()) {
	return string();
    }

    string enc;
    pack_uint(enc, last_slot);
    BitWriter slots_used(enc);
    if (count > 1) {
	slots_used.encode(first_slot, last_slot);
	slots_used.encode(count - 2, last_slot - first_slot);
	slots_used.encode_interpolative(slotvec, 0, slotvec.size() - 1);
    }

    return slots_used.freeze();
}

void
HoneyValueManager::delete_document(Xapian::docid did,
				   map<Xapian::valueno, ValueStats>& val_stats)
{
    Assert(termlist_table.is_open());
    map<Xapian::docid, string>::iterator it = slots.find(did);
    string s;
    if (it != slots.end()) {
	swap(s, it->second);
    } else {
	// Get from table, making a swift exit if this document has no terms or
	// values.
	if (!termlist_table.get_exact_entry(termlist_table.make_key(did), s))
	    return;
	slots.insert(make_pair(did, string()));
    }

    const char* p = s.data();
    const char* end = p + s.size();
    size_t slot_enc_size;
    if (!unpack_uint(&p, end, &slot_enc_size)) {
	throw Xapian::DatabaseCorruptError("Termlist encoding corrupt");
    }
    if (slot_enc_size == 0)
	return;

    end = p + slot_enc_size;
    Xapian::valueno last_slot;
    if (!unpack_uint(&p, end, &last_slot)) {
	throw Xapian::DatabaseCorruptError("Slots used data corrupt");
    }

    if (p != end) {
	BitReader rd(p, end);
	Xapian::valueno first_slot = rd.decode(last_slot);
	Xapian::valueno slot_count = rd.decode(last_slot - first_slot) + 2;
	rd.decode_interpolative(0, slot_count - 1, first_slot, last_slot);

	Xapian::valueno slot = first_slot;
	while (slot != last_slot) {
	    auto i = val_stats.insert(make_pair(slot, ValueStats()));
	    ValueStats & stats = i.first->second;
	    if (i.second) {
		// There were no statistics stored already, so read them.
		get_value_stats(slot, stats);
	    }

	    // Now, modify the stored statistics.
	    AssertRelParanoid(stats.freq, >, 0);
	    if (--(stats.freq) == 0) {
		stats.lower_bound.resize(0);
		stats.upper_bound.resize(0);
	    }

	    remove_value(did, slot);

	    slot = rd.decode_interpolative_next();
	}

    }

    Xapian::valueno slot = last_slot;
    {
	{
	    // FIXME: share code with above
	    auto i = val_stats.insert(make_pair(slot, ValueStats()));
	    ValueStats & stats = i.first->second;
	    if (i.second) {
		// There were no statistics stored already, so read them.
		get_value_stats(slot, stats);
	    }

	    // Now, modify the stored statistics.
	    AssertRelParanoid(stats.freq, >, 0);
	    if (--(stats.freq) == 0) {
		stats.lower_bound.resize(0);
		stats.upper_bound.resize(0);
	    }

	    remove_value(did, slot);
	}
    }
}

string
HoneyValueManager::replace_document(Xapian::docid did,
				    const Xapian::Document &doc,
				    map<Xapian::valueno, ValueStats>& val_stats)
{
    if (doc.get_docid() == did) {
	// If we're replacing a document with itself, but the optimisation for
	// this higher up hasn't kicked in (e.g. because we've added/replaced
	// a document since this one was read) and the values haven't changed,
	// then the call to delete_document() below will remove the values
	// before the subsequent add_document() can read them.
	//
	// The simplest way to handle this is to force the document to read its
	// values, which we only need to do this is the docid matches.  Note
	// that this check can give false positives as we don't also check the
	// database, so for example replacing document 4 in one database with
	// document 4 from another will unnecessarily trigger this, but forcing
	// the values to be read is fairly harmless, and this is unlikely to be
	// a common case.  (Currently we will end up fetching the values
	// anyway, but there's scope to change that in the future).
	doc.internal->ensure_values_fetched();
    }
    delete_document(did, val_stats);
    return add_document(did, doc, val_stats);
}

string
HoneyValueManager::get_value(Xapian::docid did, Xapian::valueno slot) const
{
    map<Xapian::valueno, map<Xapian::docid, string> >::const_iterator i;
    i = changes.find(slot);
    if (i != changes.end()) {
	map<Xapian::docid, string>::const_iterator j;
	j = i->second.find(did);
	if (j != i->second.end()) return j->second;
    }

    // Read it from the table.
    string chunk;
    Xapian::docid first_did;
    first_did = get_chunk_containing_did(slot, did, chunk);
    if (first_did == 0) return string();

    ValueChunkReader reader(chunk.data(), chunk.size(), first_did);
    reader.skip_to(did);
    if (reader.at_end() || reader.get_docid() != did) return string();
    return reader.get_value();
}

void
HoneyValueManager::get_all_values(map<Xapian::valueno, string> & values,
				  Xapian::docid did) const
{
    Assert(values.empty());
    if (!termlist_table.is_open()) {
	// Either the database has been closed, or else there's no termlist
	// table.  Check if the postlist table is open to determine which is
	// the case.
	if (!postlist_table.is_open())
	    HoneyTable::throw_database_closed();
	throw Xapian::FeatureUnavailableError("Database has no termlist");
    }

    string s;
    if (!termlist_table.get_exact_entry(termlist_table.make_key(did), s))
	return;

    const char* p = s.data();
    const char* end = p + s.size();
    size_t slot_enc_size = *p++;

    if ((slot_enc_size & 0x80) == 0) {
	// If the top bit is clear we have a 7-bit bitmap of slots used.
	Xapian::valueno slot = 0;
	while (slot_enc_size) {
	    if (slot_enc_size & 1) {
		values.insert(make_pair(slot, get_value(did, slot)));
	    }
	    ++slot;
	    slot_enc_size >>= 1;
	}
	return;
    }

    slot_enc_size &= 0x7f;
    if (slot_enc_size == 0) {
	if (!unpack_uint(&p, end, &slot_enc_size)) {
	    throw Xapian::DatabaseCorruptError("Termlist encoding corrupt");
	}
    }

    end = p + slot_enc_size;
    Xapian::valueno last_slot;
    if (!unpack_uint(&p, end, &last_slot)) {
	throw Xapian::DatabaseCorruptError("Slots used data corrupt");
    }

    if (p != end) {
	BitReader rd(p, end);
	Xapian::valueno first_slot = rd.decode(last_slot);
	Xapian::valueno slot_count = rd.decode(last_slot - first_slot) + 2;
	rd.decode_interpolative(0, slot_count - 1, first_slot, last_slot);

	Xapian::valueno slot = first_slot;
	while (slot != last_slot) {
	    values.insert(make_pair(slot, get_value(did, slot)));
	    slot = rd.decode_interpolative_next();
	}
    }
    values.insert(make_pair(last_slot, get_value(did, last_slot)));
}

void
HoneyValueManager::get_value_stats(Xapian::valueno slot) const
{
    LOGCALL_VOID(DB, "HoneyValueManager::get_value_stats", slot);
    // Invalidate the cache first in case an exception is thrown.
    mru_slot = Xapian::BAD_VALUENO;
    get_value_stats(slot, mru_valstats);
    mru_slot = slot;
}

void
HoneyValueManager::get_value_stats(Xapian::valueno slot,
				   ValueStats& stats) const
{
    LOGCALL_VOID(DB, "HoneyValueManager::get_value_stats", slot | Literal("[stats]"));

    string tag;
    if (postlist_table.get_exact_entry(pack_honey_valuestats_key(slot), tag)) {
	const char * pos = tag.data();
	const char * end = pos + tag.size();

	if (!unpack_uint(&pos, end, &(stats.freq))) {
	    if (pos == 0) {
		throw Xapian::DatabaseCorruptError("Incomplete stats item in "
						   "value table");
	    }
	    throw Xapian::RangeError("Frequency statistic in value table is "
				     "too large");
	}
	if (!unpack_string(&pos, end, stats.lower_bound)) {
	    if (pos == 0) {
		throw Xapian::DatabaseCorruptError("Incomplete stats item in "
						   "value table");
	    }
	    throw Xapian::RangeError("Lower bound in value table is too "
				     "large");
	}
	size_t len = end - pos;
	if (len == 0) {
	    stats.upper_bound = stats.lower_bound;
	} else {
	    stats.upper_bound.assign(pos, len);
	}
    } else {
	stats.clear();
    }
}

void
HoneyValueManager::set_value_stats(map<Xapian::valueno, ValueStats>& val_stats)
{
    LOGCALL_VOID(DB, "HoneyValueManager::set_value_stats", val_stats);
    map<Xapian::valueno, ValueStats>::const_iterator i;
    for (i = val_stats.begin(); i != val_stats.end(); ++i) {
	string key = pack_honey_valuestats_key(i->first);
	const ValueStats & stats = i->second;
	if (stats.freq != 0) {
	    string new_value;
	    pack_uint(new_value, stats.freq);
	    pack_string(new_value, stats.lower_bound);
	    // We don't store or count empty values, so neither of the bounds
	    // can be empty.  So we can safely store an empty upper bound when
	    // the bounds are equal.
	    if (stats.lower_bound != stats.upper_bound)
		new_value += stats.upper_bound;
	    postlist_table.add(key, new_value);
	} else {
	    postlist_table.del(key);
	}
    }
    val_stats.clear();
    mru_slot = Xapian::BAD_VALUENO;
}
