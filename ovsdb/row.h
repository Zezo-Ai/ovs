/* Copyright (c) 2009, 2010, 2011, 2012 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OVSDB_ROW_H
#define OVSDB_ROW_H 1

#include <stddef.h>
#include <stdint.h>
#include "column.h"
#include "openvswitch/hmap.h"
#include "openvswitch/list.h"
#include "ovsdb-data.h"
#include "table.h"

struct ovsdb_column_set;

/* A weak reference.
 *
 * When a column in row A contains a weak reference to UUID of a row B this
 * constitutes a weak reference from A (the source) to B (the destination).
 *
 * Rows A and B may be in the same table or different tables.
 *
 * Weak references from a row to itself are allowed, but no "struct
 * ovsdb_weak_ref" structures are created for them.
 */
struct ovsdb_weak_ref {
    struct hmap_node dst_node;     /* In ovsdb_row's 'dst_refs' hmap. */
    struct ovs_list src_node;      /* In txn_row's 'deleted/added_refs'. */

    struct ovsdb_table *src_table; /* Source row table. */
    struct uuid src;               /* Source row uuid. */

    struct ovsdb_table *dst_table; /* Destination row table. */
    struct uuid dst;               /* Destination row uuid. */

    /* Source row's key-value pair that created this reference.
     * This information is needed in order to find and delete the reference
     * from the source row.  We need both key and value in order to avoid
     * accidential deletion of an updated data, i.e. if value in datum got
     * updated and the reference was created by the old value.
     * Storing column index in order to remove references from the correct
     * column.   'by_key' flag allows to distinguish 2 references in a corner
     * case where key and value are the same. */
    union ovsdb_atom key;
    union ovsdb_atom value;
    struct ovsdb_type type;        /* Datum type of the key-value pair. */
    unsigned int column_idx;       /* Row column index for this pair. */
    bool by_key;                   /* 'true' if reference is a 'key'. */
};

/* A row in a database table. */
struct ovsdb_row {
    struct hmap_node hmap_node;    /* Element in ovsdb_table's 'rows' hmap. */
    struct ovsdb_table *table;     /* Table to which this belongs. */
    struct ovsdb_txn_row *txn_row; /* Transaction that row is in, if any. */

    /* Weak references.  Updated and checked only at transaction commit. */
    struct hmap dst_refs;          /* Weak references to this row. */

    /* Number of strong refs to this row from other rows, in this table or
     * other tables, through 'uuid' columns that have a 'refTable' constraint
     * pointing to this table and a 'refType' of "strong".  A row with nonzero
     * 'n_refs' cannot be deleted.  Updated and checked only at transaction
     * commit. */
    size_t n_refs;

    /* One datum for each column (shash_count(&table->schema->columns)
     * elements). */
    struct ovsdb_datum fields[];

    /* Followed by table->schema->n_indexes "struct hmap_node"s.  In rows that
     * have have been committed as part of the database, the hmap_node with
     * index 'i' is contained in hmap table->indexes[i].  */
};

uint32_t ovsdb_weak_ref_hash(const struct ovsdb_weak_ref *);
struct ovsdb_weak_ref * ovsdb_row_find_weak_ref(const struct ovsdb_row *,
                                                const struct ovsdb_weak_ref *);
void ovsdb_weak_ref_destroy(struct ovsdb_weak_ref *);


struct ovsdb_row *ovsdb_row_create(const struct ovsdb_table *);
struct ovsdb_row *ovsdb_row_clone(const struct ovsdb_row *);
struct ovsdb_row *ovsdb_row_datum_clone(const struct ovsdb_row *);
void ovsdb_row_destroy(struct ovsdb_row *);

uint32_t ovsdb_row_hash_columns(const struct ovsdb_row *,
                                const struct ovsdb_column_set *,
                                uint32_t basis);
bool ovsdb_row_equal_columns(const struct ovsdb_row *,
                             const struct ovsdb_row *,
                             const struct ovsdb_column_set *);
int ovsdb_row_compare_columns_3way(const struct ovsdb_row *,
                                   const struct ovsdb_row *,
                                   const struct ovsdb_column_set *);
struct ovsdb_error *ovsdb_row_update_columns(struct ovsdb_row *,
                                             const struct ovsdb_row *,
                                             const struct ovsdb_column_set *,
                                             bool xor);
void ovsdb_row_columns_to_string(const struct ovsdb_row *,
                                 const struct ovsdb_column_set *, struct ds *);
struct ovsdb_error *ovsdb_row_from_json(struct ovsdb_row *,
                                        const struct json *,
                                        struct ovsdb_symbol_table *,
                                        struct ovsdb_column_set *included,
                                        bool is_diff)
    OVS_WARN_UNUSED_RESULT;
struct json *ovsdb_row_to_json(const struct ovsdb_row *,
                               const struct ovsdb_column_set *include);
void ovsdb_row_to_string(const struct ovsdb_row *, struct ds *);

static inline const struct uuid *
ovsdb_row_get_uuid(const struct ovsdb_row *row)
{
    return &row->fields[OVSDB_COL_UUID].keys[0].uuid;
}

static inline struct uuid *
ovsdb_row_get_uuid_rw(struct ovsdb_row *row)
{
    ovsdb_datum_unshare(&row->fields[OVSDB_COL_UUID], &ovsdb_type_uuid);
    return &row->fields[OVSDB_COL_UUID].keys[0].uuid;
}

static inline const struct uuid *
ovsdb_row_get_version(const struct ovsdb_row *row)
{
    return &row->fields[OVSDB_COL_VERSION].keys[0].uuid;
}

static inline struct uuid *
ovsdb_row_get_version_rw(struct ovsdb_row *row)
{
    ovsdb_datum_unshare(&row->fields[OVSDB_COL_VERSION], &ovsdb_type_uuid);
    return &row->fields[OVSDB_COL_VERSION].keys[0].uuid;
}

static inline uint32_t
ovsdb_row_hash(const struct ovsdb_row *row)
{
    return uuid_hash(ovsdb_row_get_uuid(row));
}

/* Returns the offset in bytes from the start of an ovsdb_row for 'table' to
 * the hmap_node for the index numbered 'i'. */
static inline size_t
ovsdb_row_index_offset__(const struct ovsdb_table *table, size_t i)
{
    size_t n_fields = shash_count(&table->schema->columns);
    return (offsetof(struct ovsdb_row, fields)
            + n_fields * sizeof(struct ovsdb_datum)
            + i * sizeof(struct hmap_node));
}

/* Returns the hmap_node in 'row' for the index numbered 'i'. */
static inline struct hmap_node *
ovsdb_row_get_index_node(struct ovsdb_row *row, size_t i)
{
    return (void *) ((char *) row + ovsdb_row_index_offset__(row->table, i));
}

/* Returns the ovsdb_row given 'index_node', which is a pointer to that row's
 * hmap_node for the index numbered 'i' within 'table'. */
static inline struct ovsdb_row *
ovsdb_row_from_index_node(struct hmap_node *index_node,
                          const struct ovsdb_table *table, size_t i)
{
    return (void *) ((char *) index_node - ovsdb_row_index_offset__(table, i));
}

/* An unordered collection of rows. */
struct ovsdb_row_set {
    const struct ovsdb_row **rows;
    size_t n_rows, allocated_rows;
};

#define OVSDB_ROW_SET_INITIALIZER { NULL, 0, 0 }

void ovsdb_row_set_init(struct ovsdb_row_set *);
void ovsdb_row_set_destroy(struct ovsdb_row_set *);
void ovsdb_row_set_add_row(struct ovsdb_row_set *, const struct ovsdb_row *);

struct json *ovsdb_row_set_to_json(const struct ovsdb_row_set *,
                                   const struct ovsdb_column_set *);

void ovsdb_row_set_sort(struct ovsdb_row_set *,
                        const struct ovsdb_column_set *);

/* A hash table of rows.  A specified set of columns is used for hashing and
 * comparing rows.
 *
 * The row hash doesn't necessarily own its rows.  They may be owned by, for
 * example, an ovsdb_table. */
struct ovsdb_row_hash {
    struct hmap rows;
    struct ovsdb_column_set columns;
};

#define OVSDB_ROW_HASH_INITIALIZER(RH) \
    { HMAP_INITIALIZER(&(RH).rows), OVSDB_COLUMN_SET_INITIALIZER }

struct ovsdb_row_hash_node {
    struct hmap_node hmap_node;
    const struct ovsdb_row *row;
};

void ovsdb_row_hash_init(struct ovsdb_row_hash *,
                         const struct ovsdb_column_set *);
void ovsdb_row_hash_destroy(struct ovsdb_row_hash *, bool destroy_rows);
size_t ovsdb_row_hash_count(const struct ovsdb_row_hash *);
bool ovsdb_row_hash_contains(const struct ovsdb_row_hash *,
                             const struct ovsdb_row *);
bool ovsdb_row_hash_contains_all(const struct ovsdb_row_hash *,
                                 const struct ovsdb_row_hash *);
bool ovsdb_row_hash_insert(struct ovsdb_row_hash *, const struct ovsdb_row *);
bool ovsdb_row_hash_contains__(const struct ovsdb_row_hash *,
                               const struct ovsdb_row *, size_t hash);
bool ovsdb_row_hash_insert__(struct ovsdb_row_hash *,
                             const struct ovsdb_row *, size_t hash);

#endif /* ovsdb/row.h */
