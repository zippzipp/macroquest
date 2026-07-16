// LocalStorage.cpp : Defines the functions for the static library.
//

#include "pch.h"
#include "framework.h"
#include "LocalStorage.h"

LocalStorage::LocalStorage(const std::string& path) {
	InitDatabase(path);
}

LocalStorage::~LocalStorage() {
	ShutdownDatabase();
}

std::string* LocalStorage::GetItem(const std::string& key) {
	std::string* value = nullptr;
	sqlite3_stmt* stmt = nullptr;
	const char* sql = "SELECT ID, KEY, VALUE FROM Items WHERE KEY = ?";

	// Prepare the SQL statement
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	if (rc != SQLITE_OK) {
		SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db));
		return NULL;  // Returning an empty item, consider a better error handling approach
	}

	// Bind the key to the SQL statement
	sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);

	// Execute the query and check if we got a result
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		value = new std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
	}
	else {
		SPDLOG_ERROR("No item found with the key: {}", key);
	}

	// Finalize the statement to prevent memory leaks
	sqlite3_finalize(stmt);

	return value;
}

bool LocalStorage::SetItem(const std::string& key, const std::string& value) {
	sqlite3_stmt* stmt = nullptr;
	const char* sql = "INSERT OR REPLACE INTO Items (KEY, VALUE) VALUES (?, ?);";

	// Prepare the SQL statement
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	if (rc != SQLITE_OK) {
		SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db));
		return false;  // Returning false to indicate failure
	}

	// Bind the key and value to the SQL statement
	sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_STATIC);

	// Execute the query
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		SPDLOG_ERROR("Failed to execute upsert: {}", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return false;
	}

	// Finalize the statement to prevent memory leaks
	sqlite3_finalize(stmt);
	return true;
}

std::vector<StorageItem> LocalStorage::SearchByKey(const std::string& key, const std::string& extraWhere = "", const std::string& extraValue = "") {
	std::vector<StorageItem> items;
	sqlite3_stmt* stmt = nullptr;
	std::string sql = "SELECT ID, KEY, VALUE FROM Items WHERE KEY LIKE ?";

	// Add extra WHERE clause if provided
	if (!extraWhere.empty()) {
		sql += " AND " + extraWhere;  // Ensure the additional WHERE clause is safely concatenated
	}

	// Prepare the SQL statement
	int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
	if (rc != SQLITE_OK) {
		SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db));
		return items;  // Returning an empty vector, consider a better error handling approach
	}

	// Bind the key to the SQL statement
	sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);

	// Bind the additional value if extraWhere is not empty
	if (!extraWhere.empty() && !extraValue.empty()) {
		sqlite3_bind_text(stmt, 2, extraValue.c_str(), -1, SQLITE_STATIC);
	}

	// Execute the query and check if we got a result
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		StorageItem item;
		item.key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
		item.value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
		items.push_back(item);
	}

	// Finalize the statement to prevent memory leaks
	sqlite3_finalize(stmt);

	return items;
}

bool LocalStorage::UnsetItem(const std::string& key) {
	sqlite3_stmt* stmt = nullptr;
	const char* sql = "DELETE FROM Items WHERE \"key\" = ?;";

	// Prepare the SQL statement
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	if (rc != SQLITE_OK) {
		SPDLOG_ERROR("Failed to prepare statement: {}", sqlite3_errmsg(db));
		return false;  // Returning false to indicate failure
	}

	// Bind the key to the SQL statement
	sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);

	// Execute the query
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		SPDLOG_ERROR("Failed to delete item: {}", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return false;
	}

	// Finalize the statement to prevent memory leaks
	sqlite3_finalize(stmt);
	return true;  // Return true to indicate success
}


void LocalStorage::InitDatabase(const std::string& path) {
	if (!sqlite3_open(path.c_str(), &db)) {
		char* err_message = 0;
		const char* sql = "CREATE TABLE IF NOT EXISTS Items ("  \
			"id INTEGER PRIMARY KEY AUTOINCREMENT," \
			"\"key\" TEXT NOT NULL UNIQUE," \
			"value TEXT NOT NULL);";

		int	rc = sqlite3_exec(db, sql, 0, 0, &err_message);
		if (rc != SQLITE_OK) {
			SPDLOG_ERROR("Cannot open mq-underground localstorage: {}", err_message);
			sqlite3_free(err_message);
		}
	}
}
void LocalStorage::ShutdownDatabase() {
	if (db != nullptr) {
		int rc = sqlite3_close(db);
		if (rc != SQLITE_OK) {
			SPDLOG_ERROR("Failed to close database: {}", sqlite3_errmsg(db));
		}
		db = nullptr;
	}
}