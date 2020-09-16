#include <algorithm>
#include <cstdio>
#include <experimental/filesystem>
#include <map>
#include <memory>
#include <regex>

#include "../etymoncpp/include/mallocptr.h"
#include "../etymoncpp/include/postgres.h"
#include "../etymoncpp/include/util.h"
#include "anonymize.h"
#include "camelcase.h"
#include "dbtype.h"
#include "names.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/pointer.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/reader.h"
#include "rapidjson/stringbuffer.h"
#include "schema.h"
#include "stage.h"
#include "util.h"

//using namespace std;
namespace fs = std::experimental::filesystem;
using namespace etymon;
namespace json = rapidjson;

constexpr json::ParseFlag pflags = json::kParseTrailingCommasFlag;

struct NameComparator {
    bool operator()(const json::Value::Member &lhs,
            const json::Value::Member &rhs) const {
        const char* s1 = lhs.name.GetString();
        const char* s2 = rhs.name.GetString();
        if (strcmp(s1, "id") == 0)
            return true;
        if (strcmp(s2, "id") == 0)
            return false;
        return (strcmp(s1, s2) < 0);
    }
};

bool looksLikeDateTime(const char* str)
{
    static regex dateTime("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}");
    return regex_search(str, dateTime);
}

// Collect statistics and anonimize data
void processJSONRecord(json::Document* root, json::Value* node,
        bool collectStats, bool anonymizeTable, const string& path,
        unsigned int depth, map<string,Counts>* stats)
{
    switch (node->GetType()) {
        case json::kNullType:
            if (collectStats && depth == 1)
                (*stats)[path.c_str() + 1].null++;
            break;
        case json::kTrueType:
        case json::kFalseType:
            if (anonymizeTable && possiblePersonalData(path.c_str())) {
                json::Pointer(path.c_str()).Set(*root, false);
            }
            if (collectStats && depth == 1)
                (*stats)[path.c_str() + 1].boolean++;
            break;
        case json::kNumberType:
            if (anonymizeTable && possiblePersonalData(path.c_str())) {
                json::Pointer(path.c_str()).Set(*root, 0);
            }
            if (collectStats && depth == 1) {
                (*stats)[path.c_str() + 1].number++;
                if (node->IsInt() || node->IsUint() || node->IsInt64() ||
                        node->IsUint64())
                    (*stats)[path.c_str() + 1].integer++;
                else
                    (*stats)[path.c_str() + 1].floating++;
            }
            break;
        case json::kStringType:
            if (anonymizeTable && possiblePersonalData(path.c_str())) {
                json::Pointer(path.c_str()).Set(*root, "");
            }
            if (collectStats && depth == 1) {
                (*stats)[path.c_str() + 1].string++;
                if (isUUID(node->GetString()))
                    (*stats)[path.c_str() + 1].uuid++;
                if (looksLikeDateTime(node->GetString()))
                    (*stats)[path.c_str() + 1].dateTime++;
            }
            break;
        case json::kArrayType:
            {
                int x = 0;
                for (json::Value::ValueIterator i = node->Begin();
                        i != node->End(); ++i) {
                    string newpath = path;
                    newpath += '/';
                    newpath += to_string(x);
                    processJSONRecord(root, i, collectStats, anonymizeTable,
                            newpath, depth + 1, stats);
                    x++;
                }
            }
            break;
        case json::kObjectType:
            sort(node->MemberBegin(), node->MemberEnd(), NameComparator());
            for (json::Value::MemberIterator i = node->MemberBegin();
                    i != node->MemberEnd(); ++i) {
                string newpath = path;
                newpath += '/';
                newpath += i->name.GetString();
                processJSONRecord(root, &(i->value), collectStats,
                        anonymizeTable, newpath, depth + 1, stats);
            }
            break;
        default:
            break;
    }
}

/* *
  * \brief  Main ETL processor for JSON data.
  *
  * This class handles most of the ETL processing for a FOLIO interface.
  * The large JSON files that have been retrieved from Okapi are
  * streamed in and parsed into individual JSON object records, in order
  * that only a single record needs to be held in memory at a time.
  * Several functions are performed during two passes over the data.  In
  * pass 1:  Statistics are collected on the data types, and a table
  * schema is generated based on the results.  In pass 2:  (i) Some data
  * are removed or altered as part of anonymization of personal data.
  * (ii) Each JSON object is normalized to enable later comparison with
  * historical data.  (iii) SQL insert statements are generated and
  * submitted to the database to stage the data for merging.
  */
class JSONHandler :
    public json::BaseReaderHandler<json::UTF8<>, JSONHandler> {
public:
    int pass;
    const ldp_options& opt;
    log* lg;
    int level = 0;
    bool active = false;
    string record;
    const TableSchema& tableSchema;
    // Collection of statistics
    map<string,Counts>* stats;
    // Loading to database
    etymon::odbc_conn* conn;
    const dbtype& dbt;
    size_t recordCount = 0;
    size_t totalRecordCount = 0;
    string insertBuffer;
    JSONHandler(int pass, const ldp_options& options, log* lg,
            const TableSchema& table, etymon::odbc_conn* conn,
            const dbtype& dbt, map<string,Counts>* statistics) :
        pass(pass), opt(options), lg(lg), tableSchema(table),
        stats(statistics), conn(conn), dbt(dbt) {}
    bool StartObject();
    bool EndObject(json::SizeType memberCount);
    bool StartArray();
    bool EndArray(json::SizeType elementCount);
    bool Key(const char* str, json::SizeType length, bool copy);
    bool String(const char* str, json::SizeType length, bool copy);
    bool Int(int i);
    bool Uint(unsigned u);
    bool Bool(bool b);
    bool Null();
    bool Int64(int64_t i);
    bool Uint64(uint64_t u);
    bool Double(double d);
};

bool JSONHandler::StartObject()
{
    if (level == 2) {
        record = '{';
    } else {
        if (level > 2)
            record += '{';
    }
    level++;
    return true;
}

static void beginInserts(const string& table, string* buffer)
{
    string loadingTable;
    loadingTableName(table, &loadingTable);
    *buffer = "INSERT INTO " + loadingTable + " VALUES ";
}

static void endInserts(const ldp_options& opt, log* lg, const string& table,
        string* buffer, etymon::odbc_conn* conn)
{
    *buffer += ";\n";
    lg->write(level::detail, "", "", "Loading data for table: " + table, -1);
    conn->exec(*buffer);
    buffer->clear();
}

static void writeTuple(const ldp_options& opt, log* lg, const dbtype& dbt,
        const TableSchema& table, const json::Document& doc,
        size_t* recordCount, size_t* totalRecordCount, string* insertBuffer)
{
    if (*recordCount > 0)
        *insertBuffer += ',';
    *insertBuffer += '(';

    //*insertBuffer += "DEFAULT,";

    const char* id = doc["id"].GetString();
    // id
    string idenc;
    dbt.encode_string_const(id, &idenc);
    *insertBuffer += idenc;
    *insertBuffer += ',';

    string s;
    double d;
    for (const auto& column : table.columns) {
        if (column.columnName == "id")
            continue;
        const char* sourceColumnName = column.sourceColumnName.c_str();
        if (doc.HasMember(sourceColumnName) == false) {
            *insertBuffer += "NULL,";
            continue;
        }
        const json::Value& jsonValue = doc[sourceColumnName];
        if (jsonValue.IsNull()) {
            *insertBuffer += "NULL,";
            continue;
        }
        switch (column.columnType) {
        case ColumnType::bigint:
            *insertBuffer += to_string(jsonValue.GetInt());
            break;
        case ColumnType::boolean:
            *insertBuffer += ( jsonValue.GetBool() ?  "TRUE" : "FALSE" );
            break;
        case ColumnType::numeric:
            d = jsonValue.GetDouble();
            s = to_string(d);
            if (d > 10000000000.0) {
                lg->write(level::warning, "", "",
                          "Numeric value exceeds 10^10:\n"
                          "    Table: " + table.tableName + "\n"
                          "    Column: " + column.columnName + "\n"
                          "    ID: " + id + "\n"
                          "    Value: " + to_string(d) + "\n"
                          "    Action: Value set to 0", -1);
                s = "0";
            }
            *insertBuffer += s;
            break;
        case ColumnType::id:
        case ColumnType::timestamptz:
        case ColumnType::varchar:
            dbt.encode_string_const(jsonValue.GetString(), &s);
            // Check if varchar exceeds maximum string length (65535).
            if (s.length() >= 65535) {
                lg->write(level::warning, "", "",
                        "String length exceeds database limit:\n"
                        "    Table: " + table.tableName + "\n"
                        "    Column: " + column.columnName + "\n"
                        "    ID: " + id + "\n"
                        "    Action: Value set to NULL", -1);
                s = "NULL";
            }
            *insertBuffer += s;
            break;
        }
        *insertBuffer += ",";
    }

    string data;
    json::StringBuffer jsonText;
    json::PrettyWriter<json::StringBuffer> writer(jsonText);
    doc.Accept(writer);
    dbt.encode_string_const(jsonText.GetString(), &data);
    // Check if pretty-printed JSON exceeds maximum string length (65535).
    if (data.length() > 65535) {
        // Formatted JSON object size exceeds database limit.  Try
        // compact-printed JSON.
        json::StringBuffer jsonText;
        json::Writer<json::StringBuffer> writer(jsonText);
        doc.Accept(writer);
        dbt.encode_string_const(jsonText.GetString(), &data);
        if (data.length() > 65535) {
            lg->write(level::warning, "", "", 
                    "JSON object size exceeds database limit:\n"
                    "    Table: " + table.tableName + "\n"
                    "    ID: " + id + "\n"
                    "    Action: Value for column \"data\" set to NULL", -1);
            data = "NULL";
        }
    }

    //print(Print::warning, opt, "storing record as:\n" + data + "\n");

    *insertBuffer += data;
    //*insertBuffer += string(",") + dbt.currentTimestamp();
    *insertBuffer += ",1)";
    (*recordCount)++;
    (*totalRecordCount)++;
    //if (*totalRecordCount % 100000 == 0)
    //    fprintf(stderr, "%zu\n", *totalRecordCount);
}

bool JSONHandler::EndObject(json::SizeType memberCount)
{
    if (level == 3) {

        record += '}';

        lg->detail("New record parsed for table: " + tableSchema.tableName +
                ":\n" + record);

        char* buffer = strdup(record.c_str());
        malloc_ptr bufferptr(buffer);

        json::Document doc;
        doc.ParseInsitu<pflags>(buffer);

        string path;
        bool collectStats = (pass == 1);
        bool anonymizeTable;
        if (pass == 1) {
            anonymizeTable = false;
        } else {
            //anonymizeTable =
            //    (strcmp(tableSchema.tableName.c_str(), "user_users") == 0);
            anonymizeTable = false;
        }
        // Collect statistics and anonymize data.
        processJSONRecord(&doc, &doc, collectStats, anonymizeTable, path, 0,
                stats);

        if (pass == 2) {

            if (insertBuffer.length() > 16500000) {
            //if (insertBuffer.length() > 10000000) {
                endInserts(opt, lg, tableSchema.tableName, &insertBuffer, conn);
                beginInserts(tableSchema.tableName, &insertBuffer);
                recordCount = 0;
            }

            writeTuple(opt, lg, dbt, tableSchema, doc, &recordCount,
                    &totalRecordCount, &insertBuffer);
        }

    } else {
        if (level > 3)
            record += "},";
    }
    level--;
    return true;
}

bool JSONHandler::StartArray()
{
    if (level == 1) {
        active = true;
        if (pass == 2)
            beginInserts(tableSchema.tableName, &insertBuffer);
    } else {
        if (level > 1)
            record += '[';
    }
    level++;
    return true;
}

bool JSONHandler::EndArray(json::SizeType elementCount)
{
    if (level == 2) {
        active = false;
        if (recordCount > 0)
            if (pass == 2)
                endInserts(opt, lg, tableSchema.tableName, &insertBuffer, conn);
    } else {
        if (level > 2)
            record += "],";
    }
    level--;
    return true;
}

bool JSONHandler::Key(const char* str, json::SizeType length, bool copy)
{
    record += '\"';
    record += str;
    record += "\":";
    return true;
}

static void encodeJSON(const char* str, string* newstr)
{
    char buffer[8];
    const char *p = str;
    char c;
    while ( (c=*p) != '\0') {
        switch (c) {
            case '"':
                *newstr += "\\\"";
                break;
            case '\\':
                (*newstr) += "\\\\";
                break;
            case '\b':
                *newstr += "\\b";
                break;
            case '\f':
                *newstr += "\\f";
                break;
            case '\n':
                *newstr += "\\n";
                break;
            case '\r':
                *newstr += "\\r";
                break;
            case '\t':
                *newstr += "\\t";
                break;
            default:
                if (isprint(c)) {
                    *newstr += c;
                } else {
                    sprintf(buffer, "\\u%04X", (unsigned char) c);
                    *newstr += buffer;
                }
        }
        p++;
    }
}

bool JSONHandler::String(const char* str, json::SizeType length, bool copy)
{
    if (active && (level > 2) ) {
        record += '\"';
        string encStr;
        encodeJSON(str, &encStr);
        record += encStr;
        record += "\",";
    }
    return true;
}

bool JSONHandler::Int(int i)
{
    if ( active && (level > 2) ) {
        record += to_string(i);
        record += ',';
    }
    return true;
}

bool JSONHandler::Uint(unsigned u)
{
    if ( active && (level > 2) ) {
        record += to_string(u);
        record += ',';
    }
    return true;
}

bool JSONHandler::Int64(int64_t i)
{
    if ( active && (level > 2) ) {
        record += to_string(i);
        record += ',';
    }
    return true;
}

bool JSONHandler::Uint64(uint64_t u)
{
    if ( active && (level > 2) ) {
        record += to_string(u);
        record += ',';
    }
    return true;
}

bool JSONHandler::Double(double d)
{
    if ( active && (level > 2) ) {
        record += to_string(d);
        record += ',';
    }
    return true;
}

bool JSONHandler::Bool(bool b)
{
    if ( active && (level > 2) )
        record += b ? "true," : "false,";
    return true;

}

bool JSONHandler::Null()
{
    if ( active && (level > 2) )
        record += "null,";
    return true;
}

size_t readPageCount(const ldp_options& opt, log* lg, const string& loadDir,
        const string& tableName)
{
    string filename = loadDir;
    etymon::join(&filename, tableName);
    filename += "_count.txt";
    if ( !(fs::exists(filename)) ) {
        lg->write(level::warning, "", "", "File not found: " + filename, -1);
        return 0;
    }
    etymon::file f(filename, "r");
    size_t count;
    int r = fscanf(f.fp, "%zu", &count);
    if (r < 1 || r == EOF)
        throw runtime_error("unable to read page count from " + filename);
    return count;
}

static void stagePage(const ldp_options& opt, log* lg, int pass,
        const TableSchema& tableSchema, etymon::odbc_env* odbc,
        etymon::odbc_conn* conn, const dbtype &dbt, map<string,Counts>* stats,
        const string& filename, char* readBuffer, size_t readBufferSize)
{
    json::Reader reader;
    etymon::file f(filename, "r");
    json::FileReadStream is(f.fp, readBuffer, readBufferSize);
    JSONHandler handler(pass, opt, lg, tableSchema, conn, dbt, stats);
    reader.Parse(is, handler);
}

static void composeDataFilePath(const string& loadDir,
        const TableSchema& table, const string& suffix, string* path)
{
    *path = loadDir;
    etymon::join(path, table.tableName);
    *path += suffix;
}

static void indexLoadingTable(log* lg, const TableSchema& table,
                              etymon::odbc_conn* conn, dbtype* dbt)
{
    lg->trace("Creating indexes on table: " + table.tableName);
    string loadingTable;
    loadingTableName(table.tableName, &loadingTable);
    // If there is no table schema, define a primary key on (id) and return.
    if (table.columns.size() == 0) {
        string sql =
            "ALTER TABLE " + loadingTable + "\n"
            "    ADD PRIMARY KEY (id);";
        lg->detail(sql);
        conn->exec(sql);
        return;
    }
    // If there is a table schema, define the primary key or indexes.
    for (const auto& column : table.columns) {
        if (column.columnName == "id") {
            string sql =
                "ALTER TABLE " + loadingTable + "\n"
                "    ADD PRIMARY KEY (id);";
            lg->detail(sql);
            conn->exec(sql);
        } else {
            if (string(dbt->type_string()) == "PostgreSQL"
                && column.columnName != "data") {
                string sql =
                    "CREATE INDEX ON\n"
                    "    " + loadingTable + "\n"
                    "    (\"" + column.columnName + "\");";
                lg->detail(sql);
                conn->exec(sql);
            }
        }
    }
}

static void createLoadingTable(const ldp_options& opt, log* lg,
        const TableSchema& table, etymon::odbc_env* odbc, etymon::odbc_conn* conn,
        const dbtype& dbt)
{
    string loadingTable;
    loadingTableName(table.tableName, &loadingTable);
    string sql;

    string rskeys;
    dbt.redshift_keys("id", "id", &rskeys);
    sql = "CREATE TABLE ";
    sql += loadingTable;
    sql += " (\n"
        "    id VARCHAR(36) NOT NULL,\n";
    string columnType;
    for (const auto& column : table.columns) {
        if (column.columnName != "id") {
            sql += "    \"";
            sql += column.columnName;
            sql += "\" ";
            ColumnSchema::columnTypeToString(column.columnType, &columnType);
            sql += columnType;
            sql += ",\n";
        }
    }
    sql += string("    data ") + dbt.json_type() + ",\n"
        "    tenant_id SMALLINT NOT NULL\n"
        ")" + rskeys + ";";
    lg->write(level::detail, "", "", sql, -1);
    conn->exec(sql);

    // Add comment on table.
    if (table.moduleName != "mod-agreements") {
        sql = "COMMENT ON TABLE " + loadingTable + "\n"
            "    IS '";
        sql += table.sourcePath;
        sql += " in ";
        sql += table.moduleName;
        sql += ": ";
        sql += "https://dev.folio.org/reference/api/#";
        sql += table.moduleName;
        sql += "';";
        lg->write(level::detail, "", "",
                "Setting comment on table: " + table.tableName, -1);
        conn->exec(sql);
    }

    sql =
        "GRANT SELECT ON " + loadingTable + "\n"
        "    TO " + opt.ldpconfig_user + ";";
    lg->detail(sql);
    conn->exec(sql);

    sql =
        "GRANT SELECT ON " + loadingTable + "\n"
        "    TO " + opt.ldp_user + ";";
    lg->detail(sql);
    conn->exec(sql);
}

bool stageTable(const ldp_options& opt, log* lg, TableSchema* table,
        etymon::odbc_env* odbc, etymon::odbc_conn* conn, dbtype* dbt,
        const string& loadDir)
{
    size_t pageCount = readPageCount(opt, lg, loadDir, table->tableName);

    lg->write(level::detail, "", "",
            "Staging: " + table->tableName + ": page count: " +
            to_string(pageCount), -1);

    // TODO remove this and create the load table from merge.cpp after
    // pass 1
    //createStagingTable(opt, table->tableName, db);

    map<string,Counts> stats;
    char readBuffer[65536];

    for (int pass = 1; pass <= 2; pass++) {

        lg->write(level::detail, "", "",
                "Staging: " + table->tableName +
                (pass == 1 ?  ": analyze" : ": load"), -1);

        for (size_t page = 0; page < pageCount; page++) {
            string path;
            composeDataFilePath(loadDir, *table,
                    "_" + to_string(page) + ".json", &path);
            lg->write(level::detail, "", "",
                    "Staging: " + table->tableName +
                    (pass == 1 ?  ": analyze" : ": load") + ": page: " +
                    to_string(page), -1);
            stagePage(opt, lg, pass, *table, odbc, conn, *dbt, &stats, path,
                    readBuffer, sizeof readBuffer);
        }

        if (opt.load_from_dir != "") {
            string path;
            composeDataFilePath(loadDir, *table, "_test.json", &path);
            if (fs::exists(path)) {
                lg->write(level::detail, "", "",
                        "Staging: " + table->tableName +
                        (pass == 1 ?  ": analyze" : ": load") +
                        ": test file", -1);
                stagePage(opt, lg, pass, *table, odbc, conn, *dbt, &stats, path,
                        readBuffer, sizeof readBuffer);
            }
        }

        if (pass == 1) {
            for (const auto& [field, counts] : stats) {
                lg->write(level::detail, "", "",
                        "Stats: in field: " + field, -1);
                lg->write(level::detail, "", "",
                        "Stats: string: " + to_string(counts.string), -1);
                lg->write(level::detail, "", "",
                        "Stats: datetime: " + to_string(counts.dateTime), -1);
                lg->write(level::detail, "", "",
                        "Stats: bool: " + to_string(counts.boolean), -1);
                lg->write(level::detail, "", "",
                        "Stats: number: " + to_string(counts.number), -1);
                lg->write(level::detail, "", "",
                        "Stats: int: " + to_string(counts.integer), -1);
                lg->write(level::detail, "", "",
                        "Stats: float: " + to_string(counts.floating), -1);
                lg->write(level::detail, "", "",
                        "Stats: null: " + to_string(counts.null), -1);
            }
        }

        if (pass == 1) {
            for (const auto& [field, counts] : stats) {
                ColumnSchema column;
                bool ok =
                    ColumnSchema::selectColumnType(lg, table->tableName,
                            table->sourcePath, field, counts,
                            &column.columnType);
                if (!ok)
                    return false;
                string typeStr;
                ColumnSchema::columnTypeToString(column.columnType, &typeStr);
                string newattr;
                decode_camel_case(field.c_str(), &newattr);
                lg->write(level::detail, "", "",
                        string("Column: ") + newattr + string(" ") + typeStr,
                        -1);
                column.columnName = newattr;
                column.sourceColumnName = field;
                table->columns.push_back(column);
            }
            createLoadingTable(opt, lg, *table, odbc, conn, *dbt);
        }

        if (pass == 2)
            indexLoadingTable(lg, *table, conn, dbt);
    }

    return true;
}

