#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ClientContext;
class Connection;

string GqlTypedPropertyDuckType(const string &gql_type);

struct GqlElementTableBinding {
	uint64_t element_table_id = 0;
	string catalog_name;
	string schema_name;
	string table_name;
	string key_column;
	string ownership;
	string label_column;
	bool label_is_list = false;
	vector<string> static_labels;
	unordered_map<string, string> property_columns;
	unordered_map<string, string> property_indexes;
};

struct GqlTableGraphBinding {
	uint64_t graph_id = 0;
	GqlElementTableBinding vertex;
	GqlElementTableBinding edge;
	string edge_source_column;
	string edge_target_column;
	string source_kind;
	string source_catalog;
	string snapshot_policy;
	bool has_pinned_snapshot = false;
	uint64_t pinned_snapshot_id = 0;
	string access_mode;
};

struct GqlPropertyColumnMapping {
	string source_column;
	string property_name;
};

struct GqlReferencedTableMapping {
	string vertex_table;
	string vertex_schema_type;
	string vertex_key;
	vector<GqlPropertyColumnMapping> vertex_properties;
	string edge_table;
	string edge_schema_type;
	string edge_key;
	string edge_source;
	string edge_target;
	string source_schema_type;
	string target_schema_type;
	vector<GqlPropertyColumnMapping> edge_properties;
	string snapshot_policy = "LIVE";
	string access_mode = "READ_ONLY";
	bool validate = true;
};

//! Returns true when the graph has native table storage. Throws when native
//! metadata is incomplete or inconsistent.
bool GqlTryLoadTableGraph(ClientContext &context, const string &graph_name, GqlTableGraphBinding &result);

//! Attach graph-owned wide tables to an existing empty graph. The caller owns
//! the transaction and must create the tables before calling this function.
void GqlAttachManagedGraphTables(Connection &connection, const string &graph_name, const string &vertex_table,
                                 const string &vertex_key, const string &vertex_label, const string &edge_table,
                                 const string &edge_key, const string &edge_source, const string &edge_target,
                                 const string &edge_label, bool validate);

//! Attach read-only external tables to an existing typed graph. DuckGQL owns
//! only the mappings and never drops or mutates the referenced source tables.
void GqlAttachReferencedGraphTables(Connection &connection, const string &graph_name,
                                    const GqlReferencedTableMapping &mapping);

} // namespace duckdb
