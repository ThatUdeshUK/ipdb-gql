#include "gql_catalog.hpp"

#include "gql_sql_utils.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/main/table_description.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

static string QualifiedTable(const TableDescription &table) {
	return GqlQuoteIdentifier(table.database) + "." + GqlQuoteIdentifier(table.schema) + "." +
	       GqlQuoteIdentifier(table.table);
}

static unique_ptr<TableDescription> ResolveTable(Connection &connection, const string &input) {
	auto qualified = QualifiedName::Parse(input);
	if (qualified.name.empty()) {
		throw BinderException("A registered graph table name cannot be empty");
	}
	if (qualified.catalog == INVALID_CATALOG || qualified.schema == INVALID_SCHEMA) {
		auto defaults = GqlQuery(connection, "SELECT current_database(), current_schema()");
		if (qualified.catalog == INVALID_CATALOG) {
			qualified.catalog = defaults->GetValue(0, 0).GetValue<string>();
		}
		if (qualified.schema == INVALID_SCHEMA) {
			qualified.schema = defaults->GetValue(1, 0).GetValue<string>();
		}
	}
	auto table = connection.TableInfo(qualified.catalog, qualified.schema, qualified.name);
	if (!table) {
		throw BinderException("Registered graph table '%s' does not exist", input);
	}
	// TableInfo preserves the requested qualification. Store the resolved names
	// rather than relying on a future connection's search path.
	table->database = qualified.catalog;
	table->schema = qualified.schema;
	table->table = qualified.name;
	return table;
}

static const ColumnDefinition &ResolveColumn(const TableDescription &table, const string &requested) {
	for (const auto &column : table.columns) {
		if (StringUtil::CIEquals(column.Name(), requested)) {
			return column;
		}
	}
	throw BinderException("Column '%s' does not exist in registered graph table %s.%s", requested, table.schema,
	                      table.table);
}

static bool ValidateLabelColumn(const TableDescription &table, const ColumnDefinition &column, bool allow_list) {
	if (column.Type().id() == LogicalTypeId::VARCHAR) {
		return false;
	}
	if (allow_list && column.Type() == LogicalType::LIST(LogicalType::VARCHAR)) {
		return true;
	}
	throw BinderException("Graph label/type column '%s' in %s.%s must be %s, found %s", column.Name(), table.schema,
	                      table.table, allow_list ? "VARCHAR or VARCHAR[]" : "VARCHAR", column.Type().ToString());
}

static void ValidateKey(Connection &connection, const TableDescription &table, const ColumnDefinition &key) {
	auto column = GqlQuoteIdentifier(key.Name());
	auto result = GqlQuery(connection, "SELECT count(*)::UBIGINT, count(" + column + ")::UBIGINT, count(DISTINCT " +
	                                       column + ")::UBIGINT FROM " + QualifiedTable(table));
	auto rows = result->GetValue(0, 0).GetValue<uint64_t>();
	auto non_null = result->GetValue(1, 0).GetValue<uint64_t>();
	auto distinct = result->GetValue(2, 0).GetValue<uint64_t>();
	if (rows != non_null) {
		throw InvalidInputException("Registered graph key %s.%s.%s contains NULL values", table.schema, table.table,
		                            key.Name());
	}
	if (rows != distinct) {
		throw InvalidInputException("Registered graph key %s.%s.%s is not unique", table.schema, table.table,
		                            key.Name());
	}
}

static void ValidateEndpoint(Connection &connection, const TableDescription &edge, const ColumnDefinition &endpoint,
                             const TableDescription &vertex, const ColumnDefinition &key, const char *role) {
	auto edge_column = GqlQuoteIdentifier(endpoint.Name());
	auto vertex_column = GqlQuoteIdentifier(key.Name());
	auto sql = "SELECT count(*)::UBIGINT FROM " + QualifiedTable(edge) + " e LEFT JOIN " + QualifiedTable(vertex) +
	           " v ON e." + edge_column + " = v." + vertex_column + " WHERE e." + edge_column + " IS NULL OR v." +
	           vertex_column + " IS NULL";
	auto result = GqlQuery(connection, sql);
	if (result->GetValue(0, 0).GetValue<uint64_t>() != 0) {
		throw InvalidInputException("Registered graph %s endpoints contain NULL or missing vertex keys", role);
	}
}

static bool IsStructuralColumn(const string &name, const vector<string> &structural) {
	for (const auto &candidate : structural) {
		if (!candidate.empty() && StringUtil::CIEquals(name, candidate)) {
			return true;
		}
	}
	return false;
}

static void InsertProperties(Connection &connection, uint64_t element_table_id, const TableDescription &table,
                             const vector<string> &structural) {
	for (const auto &column : table.columns) {
		if (column.Generated() || IsStructuralColumn(column.Name(), structural)) {
			continue;
		}
		GqlQuery(connection, "INSERT INTO gql_internal.graph_property_mappings "
		                     "(element_table_id, property_name, column_name, "
		                     "gql_type, nullable, writable) VALUES (" +
		                         to_string(element_table_id) + ", " + GqlQuoteLiteral(column.Name()) + ", " +
		                         GqlQuoteLiteral(column.Name()) + ", " + GqlQuoteLiteral(column.Type().ToString()) +
		                         ", true, " + (table.readonly ? "false" : "true") + ")");
	}
}

static uint64_t InsertElementTable(Connection &connection, uint64_t graph_id, const char *kind,
                                   const TableDescription &table, const string &key_column, const char *ownership) {
	auto result = GqlQuery(
	    connection, "INSERT INTO gql_internal.graph_element_tables "
	                "(graph_id, element_kind, catalog_name, schema_name, "
	                "table_name, key_columns, "
	                "ownership, access_mode) VALUES (" +
	                    to_string(graph_id) + ", " + GqlQuoteLiteral(kind) + ", " + GqlQuoteLiteral(table.database) +
	                    ", " + GqlQuoteLiteral(table.schema) + ", " + GqlQuoteLiteral(table.table) + ", [" +
	                    GqlQuoteLiteral(key_column) + "], " + GqlQuoteLiteral(ownership) + ", " +
	                    (table.readonly ? "'READ_ONLY'" : "'READ_WRITE'") + ") RETURNING element_table_id");
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static void InsertLabelMapping(Connection &connection, uint64_t element_table_id, const string &column_name,
                               bool is_list) {
	if (column_name.empty()) {
		return;
	}
	GqlQuery(connection, "INSERT INTO gql_internal.graph_label_mappings "
	                     "(element_table_id, mapping_kind, column_name) VALUES (" +
	                         to_string(element_table_id) + ", " +
	                         GqlQuoteLiteral(is_list ? "LIST_COLUMN" : "SCALAR_COLUMN") + ", " +
	                         GqlQuoteLiteral(column_name) + ")");
}

void GqlAttachManagedGraphTables(Connection &connection, const string &graph_name, const string &vertex_table,
                                 const string &vertex_key, const string &vertex_label, const string &edge_table,
                                 const string &edge_key, const string &edge_source, const string &edge_target,
                                 const string &edge_label, bool validate) {
	GqlEnsureStorage(connection);
	auto vertex = ResolveTable(connection, vertex_table);
	auto edge = ResolveTable(connection, edge_table);
	auto &resolved_vertex_key = ResolveColumn(*vertex, vertex_key);
	auto &resolved_edge_key = ResolveColumn(*edge, edge_key);
	auto &resolved_edge_source = ResolveColumn(*edge, edge_source);
	auto &resolved_edge_target = ResolveColumn(*edge, edge_target);
	auto &resolved_vertex_label = ResolveColumn(*vertex, vertex_label);
	auto &resolved_edge_label = ResolveColumn(*edge, edge_label);
	auto vertex_label_is_list = ValidateLabelColumn(*vertex, resolved_vertex_label, true);
	auto edge_label_is_list = ValidateLabelColumn(*edge, resolved_edge_label, false);
	if (resolved_edge_source.Type() != resolved_vertex_key.Type() ||
	    resolved_edge_target.Type() != resolved_vertex_key.Type()) {
		throw BinderException("Graph edge endpoint types must exactly match the vertex key type (%s)",
		                      resolved_vertex_key.Type().ToString());
	}
	if (validate) {
		ValidateKey(connection, *vertex, resolved_vertex_key);
		ValidateKey(connection, *edge, resolved_edge_key);
		ValidateEndpoint(connection, *edge, resolved_edge_source, *vertex, resolved_vertex_key, "source");
		ValidateEndpoint(connection, *edge, resolved_edge_target, *vertex, resolved_vertex_key, "destination");
	}

	auto graph = GqlQuery(connection, "SELECT g.graph_id, coalesce(gs.storage_mode, 'EMPTY'), "
	                                  "(SELECT count(*) FROM gql_internal.graph_element_tables et WHERE et.graph_id = "
	                                  "g.graph_id) FROM gql_internal.graphs g LEFT JOIN "
	                                  "gql_internal.graph_storage gs USING (graph_id) WHERE g.graph_name = " +
	                                      GqlQuoteLiteral(graph_name));
	if (graph->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist; create it before COPY GRAPH", graph_name);
	}
	auto graph_id = graph->GetValue(0, 0).GetValue<uint64_t>();
	auto storage_mode = graph->GetValue(1, 0).GetValue<string>();
	auto element_tables = graph->GetValue(2, 0).GetValue<int64_t>();
	if (storage_mode == "TABLE_BACKED" || element_tables != 0) {
		throw InvalidInputException("Graph '%s' already has native table storage", graph_name);
	}
	if (storage_mode != "EMPTY") {
		throw InvalidInputException("Graph '%s' uses unsupported legacy storage mode '%s'; recreate and reload it with "
		                            "COPY GRAPH",
		                            graph_name, storage_mode);
	}

	auto vertex_id = InsertElementTable(connection, graph_id, "VERTEX", *vertex, resolved_vertex_key.Name(), "MANAGED");
	auto edge_id = InsertElementTable(connection, graph_id, "EDGE", *edge, resolved_edge_key.Name(), "MANAGED");
	InsertLabelMapping(connection, vertex_id, resolved_vertex_label.Name(), vertex_label_is_list);
	InsertLabelMapping(connection, edge_id, resolved_edge_label.Name(), edge_label_is_list);
	InsertProperties(connection, vertex_id, *vertex, {resolved_vertex_key.Name(), resolved_vertex_label.Name()});
	InsertProperties(connection, edge_id, *edge,
	                 {resolved_edge_key.Name(), resolved_edge_source.Name(), resolved_edge_target.Name(),
	                  resolved_edge_label.Name()});
	GqlQuery(connection, "INSERT INTO gql_internal.graph_edge_endpoints "
	                     "(edge_table_id, source_vertex_table_id, target_vertex_table_id, source_columns, "
	                     "target_columns, source_key_columns, target_key_columns) VALUES (" +
	                         to_string(edge_id) + ", " + to_string(vertex_id) + ", " + to_string(vertex_id) + ", [" +
	                         GqlQuoteLiteral(resolved_edge_source.Name()) + "], [" +
	                         GqlQuoteLiteral(resolved_edge_target.Name()) + "], [" +
	                         GqlQuoteLiteral(resolved_vertex_key.Name()) + "], [" +
	                         GqlQuoteLiteral(resolved_vertex_key.Name()) + "])");
	GqlQuery(
	    connection,
	    "UPDATE gql_internal.graph_storage SET storage_mode = 'TABLE_BACKED', default_catalog = " +
	        GqlQuoteLiteral(vertex->database) + ", default_schema = " + GqlQuoteLiteral(vertex->schema) +
	        ", schema_version = schema_version + 1, csr_policy = 'MANUAL' WHERE graph_id = " + to_string(graph_id));
	GqlQuery(connection, "UPDATE gql_internal.graphs SET graph_version = graph_version + 1 WHERE graph_id = " +
	                         to_string(graph_id));
}

static bool IsReferencedKeyType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}

static uint64_t FindSchemaElement(Connection &connection, uint64_t graph_id, const string &kind,
                                  const string &type_name) {
	auto result = GqlQuery(connection, "SELECT schema_element_id FROM gql_internal.graph_schema_elements WHERE "
	                                   "graph_id = " +
	                                       to_string(graph_id) + " AND element_kind = " + GqlQuoteLiteral(kind) +
	                                       " AND lower(type_name) = " + GqlQuoteLiteral(StringUtil::Lower(type_name)));
	if (result->RowCount() != 1) {
		throw BinderException("Referenced graph mapping names unknown %s type '%s'", StringUtil::Lower(kind),
		                      type_name);
	}
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static void InsertStaticLabels(Connection &connection, uint64_t element_table_id, uint64_t schema_element_id,
                               const string &fallback) {
	auto labels = GqlQuery(connection, "SELECT label_name FROM gql_internal.graph_schema_labels WHERE "
	                                   "schema_element_id = " +
	                                       to_string(schema_element_id) + " ORDER BY label_ordinal");
	if (labels->RowCount() == 0) {
		GqlQuery(connection, "INSERT INTO gql_internal.graph_label_mappings "
		                     "(element_table_id, label_name, mapping_kind) VALUES (" +
		                         to_string(element_table_id) + ", " + GqlQuoteLiteral(fallback) + ", 'STATIC')");
		return;
	}
	for (idx_t row = 0; row < labels->RowCount(); row++) {
		GqlQuery(connection, "INSERT INTO gql_internal.graph_label_mappings "
		                     "(element_table_id, label_name, mapping_kind) VALUES (" +
		                         to_string(element_table_id) + ", " +
		                         GqlQuoteLiteral(labels->GetValue(0, row).GetValue<string>()) + ", 'STATIC')");
	}
}

static void InsertReferencedProperties(Connection &connection, uint64_t element_table_id, uint64_t schema_element_id,
                                       const TableDescription &table, const vector<GqlPropertyColumnMapping> &mappings,
                                       bool validate) {
	auto properties = GqlQuery(connection, "SELECT property_name, gql_type, nullable FROM "
	                                       "gql_internal.graph_schema_properties WHERE schema_element_id = " +
	                                           to_string(schema_element_id) + " ORDER BY property_ordinal");
	if (properties->RowCount() != mappings.size()) {
		throw BinderException("Referenced table %s.%s must map every declared property exactly once", table.schema,
		                      table.table);
	}
	unordered_set<string> mapped_properties;
	for (const auto &mapping : mappings) {
		auto normalized = StringUtil::Lower(mapping.property_name);
		if (!mapped_properties.insert(normalized).second) {
			throw BinderException("Duplicate referenced property mapping '%s'", mapping.property_name);
		}
		idx_t schema_row = DConstants::INVALID_INDEX;
		for (idx_t row = 0; row < properties->RowCount(); row++) {
			if (StringUtil::CIEquals(properties->GetValue(0, row).GetValue<string>(), mapping.property_name)) {
				schema_row = row;
				break;
			}
		}
		if (schema_row == DConstants::INVALID_INDEX) {
			throw BinderException("Property '%s' is not declared by the mapped graph type", mapping.property_name);
		}
		auto &column = ResolveColumn(table, mapping.source_column);
		auto gql_type = properties->GetValue(1, schema_row).GetValue<string>();
		auto expected = GqlTypedPropertyDuckType(gql_type);
		if (!StringUtil::CIEquals(column.Type().ToString(), expected)) {
			throw BinderException("Referenced property '%s' expects %s but column %s.%s.%s has type %s",
			                      mapping.property_name, expected, table.schema, table.table, column.Name(),
			                      column.Type().ToString());
		}
		auto nullable = properties->GetValue(2, schema_row).GetValue<bool>();
		if (validate && !nullable) {
			auto nulls = GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + QualifiedTable(table) + " WHERE " +
			                                      GqlQuoteIdentifier(column.Name()) + " IS NULL");
			if (nulls->GetValue(0, 0).GetValue<uint64_t>() != 0) {
				throw InvalidInputException("Referenced property %s.%s.%s violates NOT NULL graph schema", table.schema,
				                            table.table, column.Name());
			}
		}
		GqlQuery(connection, "INSERT INTO gql_internal.graph_property_mappings "
		                     "(element_table_id, property_name, column_name, gql_type, nullable, writable) VALUES (" +
		                         to_string(element_table_id) + ", " + GqlQuoteLiteral(mapping.property_name) + ", " +
		                         GqlQuoteLiteral(column.Name()) + ", " + GqlQuoteLiteral(expected) + ", " +
		                         (nullable ? "true" : "false") + ", false)");
	}
}

static void AppendFingerprintField(string &fingerprint, const string &value) {
	fingerprint += to_string(value.size()) + ":" + value + ";";
}

static string ReadSingleColumn(const Value &value, const char *description);

static uint64_t ReadSourceTableOid(Connection &connection, const TableDescription &table) {
	auto result =
	    GqlQuery(connection, "SELECT table_oid::UBIGINT FROM duckdb_tables() WHERE "
	                         "lower(database_name) = " +
	                             GqlQuoteLiteral(StringUtil::Lower(table.database)) +
	                             " AND lower(schema_name) = " + GqlQuoteLiteral(StringUtil::Lower(table.schema)) +
	                             " AND lower(table_name) = " + GqlQuoteLiteral(StringUtil::Lower(table.table)));
	if (result->RowCount() != 1 || result->GetValue(0, 0).IsNull()) {
		throw BinderException("Referenced source table %s.%s.%s is not an attached base table", table.database,
		                      table.schema, table.table);
	}
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static string ReadDuckLakeTableUuid(Connection &connection, const TableDescription &table) {
	auto result = GqlQuery(
	    connection, "SELECT table_uuid::VARCHAR FROM " + GqlQuoteIdentifier(table.database) +
	                    ".table_info() WHERE lower(table_name) = " + GqlQuoteLiteral(StringUtil::Lower(table.table)));
	if (result->RowCount() == 0 || result->GetValue(0, 0).IsNull()) {
		throw BinderException("Referenced DuckLake table %s.%s.%s has no persistent table UUID", table.database,
		                      table.schema, table.table);
	}
	if (result->RowCount() != 1) {
		throw NotImplementedException("DuckLake table name '%s' is ambiguous across source schemas; referenced "
		                              "graphs currently require mapped table names to be unique within the catalog",
		                              table.table);
	}
	return result->GetValue(0, 0).GetValue<string>();
}

static string ReadSourceTableIdentity(Connection &connection, const TableDescription &table,
                                      const string &source_kind) {
	if (StringUtil::CIEquals(source_kind, "DUCKLAKE")) {
		auto database =
		    GqlQuery(connection, "SELECT lower(type) FROM duckdb_databases() WHERE lower(database_name) = " +
		                             GqlQuoteLiteral(StringUtil::Lower(table.database)));
		if (database->RowCount() == 1 && !database->GetValue(0, 0).IsNull() &&
		    database->GetValue(0, 0).GetValue<string>() == "ducklake") {
			return "uuid:" + ReadDuckLakeTableUuid(connection, table);
		}
	}
	return "oid:" + to_string(ReadSourceTableOid(connection, table));
}

static uint64_t ReadReferencedDuckLakeSnapshot(Connection &connection, const string &catalog) {
	auto result =
	    GqlQuery(connection, "SELECT id::UBIGINT FROM " + GqlQuoteIdentifier(catalog) + ".current_snapshot()");
	if (result->RowCount() != 1 || result->GetValue(0, 0).IsNull()) {
		throw InvalidInputException("DuckLake catalog '%s' did not expose a current snapshot", catalog);
	}
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static bool ReadSourceColumnNullable(Connection &connection, const TableDescription &table, const string &column_name) {
	auto result =
	    GqlQuery(connection, "SELECT is_nullable FROM duckdb_columns() WHERE lower(database_name) = " +
	                             GqlQuoteLiteral(StringUtil::Lower(table.database)) +
	                             " AND lower(schema_name) = " + GqlQuoteLiteral(StringUtil::Lower(table.schema)) +
	                             " AND lower(table_name) = " + GqlQuoteLiteral(StringUtil::Lower(table.table)) +
	                             " AND lower(column_name) = " + GqlQuoteLiteral(StringUtil::Lower(column_name)));
	if (result->RowCount() != 1 || result->GetValue(0, 0).IsNull()) {
		throw BinderException("Referenced source column %s.%s.%s.%s no longer exists", table.database, table.schema,
		                      table.table, column_name);
	}
	return result->GetValue(0, 0).GetValue<bool>();
}

static void AppendSourceColumnFingerprint(Connection &connection, string &fingerprint, const TableDescription &table,
                                          const string &role, const string &column_name) {
	auto &column = ResolveColumn(table, column_name);
	AppendFingerprintField(fingerprint, role);
	AppendFingerprintField(fingerprint, StringUtil::Lower(column.Name()));
	AppendFingerprintField(fingerprint, column.Type().ToString());
	AppendFingerprintField(fingerprint,
	                       ReadSourceColumnNullable(connection, table, column.Name()) ? "NULL" : "NOT_NULL");
}

// This is intentionally based only on graph-visible source structure. Adding
// an unmapped/private source column must not invalidate a referenced graph.
// Persistent DuckLake table UUIDs (and native DuckDB table OIDs) distinguish a
// compatible table from a new table recreated under the same qualified name.
static string BuildReferencedSchemaFingerprint(Connection &connection, uint64_t graph_id, const string &source_kind) {
	string fingerprint = "v1|";
	auto tables = GqlQuery(connection, "SELECT element_table_id, element_kind, catalog_name, schema_name, table_name, "
	                                   "key_columns FROM gql_internal.graph_element_tables WHERE graph_id = " +
	                                       to_string(graph_id) + " AND ownership = 'REFERENCED' ORDER BY element_kind");
	if (tables->RowCount() != 2) {
		throw InvalidInputException("Referenced graph metadata must contain one vertex and one edge table");
	}
	for (idx_t row = 0; row < tables->RowCount(); row++) {
		auto element_table_id = tables->GetValue(0, row).GetValue<uint64_t>();
		auto kind = tables->GetValue(1, row).GetValue<string>();
		auto catalog = tables->GetValue(2, row).GetValue<string>();
		auto schema = tables->GetValue(3, row).GetValue<string>();
		auto table_name = tables->GetValue(4, row).GetValue<string>();
		auto table = ResolveTable(connection, GqlQuoteIdentifier(catalog) + "." + GqlQuoteIdentifier(schema) + "." +
		                                          GqlQuoteIdentifier(table_name));
		AppendFingerprintField(fingerprint, kind);
		AppendFingerprintField(fingerprint, StringUtil::Lower(table->database));
		AppendFingerprintField(fingerprint, StringUtil::Lower(table->schema));
		AppendFingerprintField(fingerprint, StringUtil::Lower(table->table));
		AppendFingerprintField(fingerprint, ReadSourceTableIdentity(connection, *table, source_kind));
		AppendSourceColumnFingerprint(connection, fingerprint, *table, "KEY",
		                              ReadSingleColumn(tables->GetValue(5, row), "element key"));

		if (kind == "EDGE") {
			auto endpoints = GqlQuery(connection, "SELECT source_columns, target_columns FROM "
			                                      "gql_internal.graph_edge_endpoints WHERE edge_table_id = " +
			                                          to_string(element_table_id));
			if (endpoints->RowCount() != 1) {
				throw InvalidInputException("Referenced graph contains invalid edge endpoint metadata");
			}
			AppendSourceColumnFingerprint(connection, fingerprint, *table, "SOURCE",
			                              ReadSingleColumn(endpoints->GetValue(0, 0), "edge source"));
			AppendSourceColumnFingerprint(connection, fingerprint, *table, "TARGET",
			                              ReadSingleColumn(endpoints->GetValue(1, 0), "edge destination"));
		}

		auto labels =
		    GqlQuery(connection, "SELECT mapping_kind, coalesce(label_name, ''), coalesce(column_name, '') "
		                         "FROM gql_internal.graph_label_mappings WHERE element_table_id = " +
		                             to_string(element_table_id) + " ORDER BY mapping_kind, label_name, column_name");
		for (idx_t label = 0; label < labels->RowCount(); label++) {
			auto mapping_kind = labels->GetValue(0, label).GetValue<string>();
			AppendFingerprintField(fingerprint, "LABEL");
			AppendFingerprintField(fingerprint, mapping_kind);
			AppendFingerprintField(fingerprint, labels->GetValue(1, label).GetValue<string>());
			auto column_name = labels->GetValue(2, label).GetValue<string>();
			if (!column_name.empty()) {
				AppendSourceColumnFingerprint(connection, fingerprint, *table, "LABEL_COLUMN", column_name);
			}
		}

		auto types = GqlQuery(connection, "SELECT se.element_kind, se.type_name, tm.discriminator_kind, "
		                                  "coalesce(tm.discriminator_value, '') FROM "
		                                  "gql_internal.graph_element_type_mappings tm JOIN "
		                                  "gql_internal.graph_schema_elements se USING (schema_element_id) WHERE "
		                                  "tm.element_table_id = " +
		                                      to_string(element_table_id) + " ORDER BY se.element_kind, se.type_name");
		for (idx_t type = 0; type < types->RowCount(); type++) {
			AppendFingerprintField(fingerprint, "TYPE");
			for (idx_t column = 0; column < 4; column++) {
				AppendFingerprintField(fingerprint, types->GetValue(column, type).GetValue<string>());
			}
		}

		auto properties = GqlQuery(connection, "SELECT property_name, column_name, gql_type, nullable FROM "
		                                       "gql_internal.graph_property_mappings WHERE element_table_id = " +
		                                           to_string(element_table_id) + " ORDER BY property_name");
		for (idx_t property = 0; property < properties->RowCount(); property++) {
			AppendFingerprintField(fingerprint, "PROPERTY");
			AppendFingerprintField(fingerprint, properties->GetValue(0, property).GetValue<string>());
			AppendFingerprintField(fingerprint, properties->GetValue(2, property).GetValue<string>());
			AppendFingerprintField(fingerprint,
			                       properties->GetValue(3, property).GetValue<bool>() ? "NULL" : "NOT_NULL");
			AppendSourceColumnFingerprint(connection, fingerprint, *table, "PROPERTY_COLUMN",
			                              properties->GetValue(1, property).GetValue<string>());
		}
	}
	return fingerprint;
}

void GqlAttachReferencedGraphTables(Connection &connection, const string &graph_name,
                                    const GqlReferencedTableMapping &mapping) {
	GqlEnsureStorage(connection);
	auto vertex = ResolveTable(connection, mapping.vertex_table);
	auto edge = ResolveTable(connection, mapping.edge_table);
	if (!StringUtil::CIEquals(vertex->database, edge->database)) {
		throw BinderException("Referenced vertex and edge tables must use the same catalog");
	}
	auto &vertex_key = ResolveColumn(*vertex, mapping.vertex_key);
	auto &edge_key = ResolveColumn(*edge, mapping.edge_key);
	auto &edge_source = ResolveColumn(*edge, mapping.edge_source);
	auto &edge_target = ResolveColumn(*edge, mapping.edge_target);
	if (!IsReferencedKeyType(vertex_key.Type()) || !IsReferencedKeyType(edge_key.Type())) {
		throw BinderException("Referenced graph keys must use integer types");
	}
	if (edge_source.Type() != vertex_key.Type() || edge_target.Type() != vertex_key.Type()) {
		throw BinderException("Referenced edge endpoint types must exactly match vertex key type %s",
		                      vertex_key.Type().ToString());
	}
	if (!StringUtil::CIEquals(mapping.source_schema_type, mapping.vertex_schema_type) ||
	    !StringUtil::CIEquals(mapping.target_schema_type, mapping.vertex_schema_type)) {
		throw BinderException("The first referenced-graph slice requires both edge endpoints to map to node type '%s'",
		                      mapping.vertex_schema_type);
	}
	if (mapping.validate) {
		ValidateKey(connection, *vertex, vertex_key);
		ValidateKey(connection, *edge, edge_key);
		ValidateEndpoint(connection, *edge, edge_source, *vertex, vertex_key, "source");
		ValidateEndpoint(connection, *edge, edge_target, *vertex, vertex_key, "destination");
	}

	auto graph = GqlQuery(connection, "SELECT g.graph_id, gs.storage_mode FROM gql_internal.graphs g JOIN "
	                                  "gql_internal.graph_storage gs USING (graph_id) WHERE g.graph_name = " +
	                                      GqlQuoteLiteral(graph_name));
	if (graph->RowCount() != 1 || graph->GetValue(1, 0).GetValue<string>() != "EMPTY") {
		throw InvalidInputException("Graph '%s' must be empty before attaching referenced tables", graph_name);
	}
	auto graph_id = graph->GetValue(0, 0).GetValue<uint64_t>();
	auto vertex_schema_id = FindSchemaElement(connection, graph_id, "NODE", mapping.vertex_schema_type);
	auto edge_schema_id = FindSchemaElement(connection, graph_id, "EDGE", mapping.edge_schema_type);

	auto vertex_id = InsertElementTable(connection, graph_id, "VERTEX", *vertex, vertex_key.Name(), "REFERENCED");
	auto edge_id = InsertElementTable(connection, graph_id, "EDGE", *edge, edge_key.Name(), "REFERENCED");
	GqlQuery(connection, "UPDATE gql_internal.graph_element_tables SET access_mode = 'READ_ONLY' WHERE "
	                     "element_table_id IN (" +
	                         to_string(vertex_id) + ", " + to_string(edge_id) + ")");
	InsertStaticLabels(connection, vertex_id, vertex_schema_id, mapping.vertex_schema_type);
	InsertStaticLabels(connection, edge_id, edge_schema_id, mapping.edge_schema_type);
	InsertReferencedProperties(connection, vertex_id, vertex_schema_id, *vertex, mapping.vertex_properties,
	                           mapping.validate);
	InsertReferencedProperties(connection, edge_id, edge_schema_id, *edge, mapping.edge_properties, mapping.validate);
	GqlQuery(connection, "INSERT INTO gql_internal.graph_element_type_mappings "
	                     "(element_table_id, schema_element_id, discriminator_kind) VALUES (" +
	                         to_string(vertex_id) + ", " + to_string(vertex_schema_id) + ", 'STATIC'), (" +
	                         to_string(edge_id) + ", " + to_string(edge_schema_id) + ", 'STATIC')");
	GqlQuery(connection, "INSERT INTO gql_internal.graph_edge_endpoints "
	                     "(edge_table_id, source_vertex_table_id, target_vertex_table_id, source_columns, "
	                     "target_columns, source_key_columns, target_key_columns) VALUES (" +
	                         to_string(edge_id) + ", " + to_string(vertex_id) + ", " + to_string(vertex_id) + ", [" +
	                         GqlQuoteLiteral(edge_source.Name()) + "], [" + GqlQuoteLiteral(edge_target.Name()) +
	                         "], [" + GqlQuoteLiteral(vertex_key.Name()) + "], [" + GqlQuoteLiteral(vertex_key.Name()) +
	                         "])");

	auto database = GqlQuery(connection, "SELECT lower(type) FROM duckdb_databases() WHERE database_name = " +
	                                         GqlQuoteLiteral(vertex->database));
	if (database->RowCount() != 1) {
		throw BinderException("Referenced source catalog '%s' is not attached", vertex->database);
	}
	auto database_type = database->GetValue(0, 0).GetValue<string>();
	string source_kind;
	if (database_type == "ducklake") {
		source_kind = "DUCKLAKE";
	} else if (database_type == "duckdb") {
		source_kind = "DUCKDB";
	} else {
		throw NotImplementedException("Referenced graphs do not yet support catalog type '%s'", database_type);
	}
	string snapshot = "NULL";
	if (source_kind == "DUCKLAKE") {
		snapshot = to_string(ReadReferencedDuckLakeSnapshot(connection, vertex->database));
	} else if (StringUtil::CIEquals(mapping.snapshot_policy, "PINNED")) {
		throw BinderException("SNAPSHOT_POLICY 'PINNED' requires a DuckLake source catalog attached with "
		                      "SNAPSHOT_VERSION");
	}
	auto pinned_snapshot = StringUtil::CIEquals(mapping.snapshot_policy, "PINNED") ? snapshot : "NULL";
	auto fingerprint = BuildReferencedSchemaFingerprint(connection, graph_id, source_kind);
	GqlQuery(connection, "INSERT INTO gql_internal.graph_sources "
	                     "(graph_id, source_kind, source_catalog, snapshot_policy, pinned_snapshot_id, access_mode, "
	                     "registered_snapshot_id, last_validated_snapshot_id, schema_fingerprint) VALUES (" +
	                         to_string(graph_id) + ", " + GqlQuoteLiteral(source_kind) + ", " +
	                         GqlQuoteLiteral(vertex->database) + ", " + GqlQuoteLiteral(mapping.snapshot_policy) +
	                         ", " + pinned_snapshot + ", " + GqlQuoteLiteral(mapping.access_mode) + ", " + snapshot +
	                         ", " + (mapping.validate ? snapshot : "NULL") + ", " + GqlQuoteLiteral(fingerprint) + ")");
	GqlQuery(connection, "UPDATE gql_internal.graph_storage SET storage_mode = 'TABLE_BACKED', default_catalog = " +
	                         GqlQuoteLiteral(vertex->database) +
	                         ", default_schema = " + GqlQuoteLiteral(vertex->schema) +
	                         ", schema_version = schema_version + 1, "
	                         "csr_policy = 'MANUAL' WHERE graph_id = " +
	                         to_string(graph_id));
	GqlQuery(connection, "UPDATE gql_internal.graphs SET graph_version = graph_version + 1 WHERE graph_id = " +
	                         to_string(graph_id));
}

static string ReadSingleColumn(const Value &value, const char *description) {
	const auto &children = ListValue::GetChildren(value);
	if (children.size() != 1 || children[0].IsNull()) {
		throw InvalidInputException("Table-backed graph %s must contain exactly one column", description);
	}
	return children[0].GetValue<string>();
}

static void LoadProperties(Connection &connection, GqlElementTableBinding &table) {
	auto result = GqlQuery(connection, "SELECT property_name, column_name FROM "
	                                   "gql_internal.graph_property_mappings WHERE element_table_id = " +
	                                       to_string(table.element_table_id));
	for (idx_t row = 0; row < result->RowCount(); row++) {
		table.property_columns.emplace(result->GetValue(0, row).GetValue<string>(),
		                               result->GetValue(1, row).GetValue<string>());
	}
}

static void LoadPropertyIndexes(Connection &connection, GqlElementTableBinding &table) {
	auto result = GqlQuery(connection, "SELECT property_name, index_name FROM "
	                                   "gql_internal.graph_property_indexes WHERE element_table_id = " +
	                                       to_string(table.element_table_id));
	for (idx_t row = 0; row < result->RowCount(); row++) {
		table.property_indexes.emplace(result->GetValue(0, row).GetValue<string>(),
		                               result->GetValue(1, row).GetValue<string>());
	}
}

static void LoadLabel(Connection &connection, GqlElementTableBinding &table) {
	auto result = GqlQuery(connection, "SELECT mapping_kind, column_name, label_name FROM "
	                                   "gql_internal.graph_label_mappings WHERE "
	                                   "element_table_id = " +
	                                       to_string(table.element_table_id));
	if (result->RowCount() == 0) {
		return;
	}
	for (idx_t row = 0; row < result->RowCount(); row++) {
		auto mapping_kind = result->GetValue(0, row).GetValue<string>();
		if (mapping_kind == "STATIC") {
			if (!table.label_column.empty() || result->GetValue(2, row).IsNull()) {
				throw InvalidInputException("Table-backed graph contains inconsistent static label metadata");
			}
			table.static_labels.push_back(result->GetValue(2, row).GetValue<string>());
			continue;
		}
		if ((mapping_kind != "SCALAR_COLUMN" && mapping_kind != "LIST_COLUMN") || result->RowCount() != 1 ||
		    !table.static_labels.empty()) {
			throw NotImplementedException("Table-backed MATCH does not support mixed label mapping kind '%s'",
			                              mapping_kind);
		}
		table.label_is_list = mapping_kind == "LIST_COLUMN";
		table.label_column = result->GetValue(1, row).GetValue<string>();
	}
}

bool GqlTryLoadTableGraph(ClientContext &context, const string &graph_name, GqlTableGraphBinding &result) {
	Connection connection(*context.db);
	auto storage = GqlQuery(connection, "SELECT g.graph_id, gs.storage_mode FROM gql_internal.graphs g JOIN "
	                                    "gql_internal.graph_storage gs USING (graph_id) WHERE g.graph_name = " +
	                                        GqlQuoteLiteral(graph_name));
	if (storage->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist", graph_name);
	}
	result.graph_id = storage->GetValue(0, 0).GetValue<uint64_t>();
	if (storage->GetValue(1, 0).GetValue<string>() != "TABLE_BACKED") {
		return false;
	}

	auto tables =
	    GqlQuery(connection, "SELECT element_table_id, element_kind, catalog_name, schema_name, "
	                         "table_name, "
	                         "key_columns, ownership FROM gql_internal.graph_element_tables WHERE graph_id = " +
	                             to_string(result.graph_id) + " ORDER BY element_kind");
	if (tables->RowCount() != 2) {
		throw InvalidInputException("Table-backed graph '%s' must contain one vertex and one edge table", graph_name);
	}
	for (idx_t row = 0; row < tables->RowCount(); row++) {
		auto kind = tables->GetValue(1, row).GetValue<string>();
		auto *target = kind == "VERTEX" ? &result.vertex : kind == "EDGE" ? &result.edge : nullptr;
		if (!target || target->element_table_id != 0) {
			throw InvalidInputException("Table-backed graph '%s' contains invalid element table metadata", graph_name);
		}
		target->element_table_id = tables->GetValue(0, row).GetValue<uint64_t>();
		target->catalog_name = tables->GetValue(2, row).GetValue<string>();
		target->schema_name = tables->GetValue(3, row).GetValue<string>();
		target->table_name = tables->GetValue(4, row).GetValue<string>();
		target->key_column = ReadSingleColumn(tables->GetValue(5, row), "element key");
		target->ownership = tables->GetValue(6, row).GetValue<string>();
		LoadLabel(connection, *target);
		LoadProperties(connection, *target);
		LoadPropertyIndexes(connection, *target);
	}

	auto endpoints = GqlQuery(connection, "SELECT source_vertex_table_id, target_vertex_table_id, source_columns, "
	                                      "target_columns FROM gql_internal.graph_edge_endpoints WHERE "
	                                      "edge_table_id = " +
	                                          to_string(result.edge.element_table_id));
	if (endpoints->RowCount() != 1 ||
	    endpoints->GetValue(0, 0).GetValue<uint64_t>() != result.vertex.element_table_id ||
	    endpoints->GetValue(1, 0).GetValue<uint64_t>() != result.vertex.element_table_id) {
		throw InvalidInputException("Table-backed graph '%s' has invalid endpoint metadata", graph_name);
	}
	result.edge_source_column = ReadSingleColumn(endpoints->GetValue(2, 0), "edge source");
	result.edge_target_column = ReadSingleColumn(endpoints->GetValue(3, 0), "edge destination");
	auto source = GqlQuery(connection, "SELECT source_kind, source_catalog, snapshot_policy, pinned_snapshot_id, "
	                                   "access_mode, schema_fingerprint FROM "
	                                   "gql_internal.graph_sources WHERE graph_id = " +
	                                       to_string(result.graph_id));
	if (source->RowCount() == 1) {
		result.source_kind = source->GetValue(0, 0).GetValue<string>();
		result.source_catalog = source->GetValue(1, 0).GetValue<string>();
		result.snapshot_policy = source->GetValue(2, 0).GetValue<string>();
		if (!source->GetValue(3, 0).IsNull()) {
			result.has_pinned_snapshot = true;
			result.pinned_snapshot_id = source->GetValue(3, 0).GetValue<uint64_t>();
		}
		result.access_mode = source->GetValue(4, 0).GetValue<string>();
		auto registered_fingerprint = source->GetValue(5, 0).GetValue<string>();
		try {
			if (StringUtil::CIEquals(result.snapshot_policy, "PINNED")) {
				if (!StringUtil::CIEquals(result.source_kind, "DUCKLAKE") || !result.has_pinned_snapshot) {
					throw InvalidInputException("Pinned graph '%s' has incomplete DuckLake snapshot metadata",
					                            graph_name);
				}
				auto observed = ReadReferencedDuckLakeSnapshot(connection, result.source_catalog);
				if (observed != result.pinned_snapshot_id) {
					throw InvalidInputException(
					    "Pinned graph '%s' requires DuckLake catalog '%s' attached with SNAPSHOT_VERSION %llu; "
					    "observed snapshot %llu",
					    graph_name, result.source_catalog, result.pinned_snapshot_id, observed);
				}
			}
			auto current_fingerprint =
			    BuildReferencedSchemaFingerprint(connection, result.graph_id, result.source_kind);
			if (registered_fingerprint != current_fingerprint) {
				throw InvalidInputException("Referenced graph '%s' source schema or table identity changed; recreate "
				                            "the graph mapping",
				                            graph_name);
			}
		} catch (const InvalidInputException &) {
			throw;
		} catch (const std::exception &error) {
			throw InvalidInputException("Referenced graph '%s' source schema validation failed: %s", graph_name,
			                            error.what());
		}
	} else if (source->RowCount() != 0) {
		throw InvalidInputException("Table-backed graph '%s' contains duplicate source metadata", graph_name);
	}
	return true;
}

} // namespace duckdb
