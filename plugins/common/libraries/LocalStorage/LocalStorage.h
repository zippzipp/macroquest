#ifndef LOCAL_STORAGE_H
#define LOCAL_STORAGE_H

#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <random>
#include <functional>
#include <spdlog/spdlog.h>
#include <filesystem>
#include "sqlite3.h"
#pragma comment(lib, "sqlite3")

struct StorageItem {
	std::string key;
	std::string value;
};


class LocalStorage {
public:
	LocalStorage(const std::string& path);
	~LocalStorage();

	bool SetItem(const std::string& key, const std::string& value);
	std::string* GetItem(const std::string& key);
	bool UnsetItem(const std::string& key);
	std::vector<StorageItem> SearchByKey(const std::string& key, const std::string& extraWhere, const std::string& extraValue);
private:
	sqlite3* db;

	void InitDatabase(const std::string& path);
	void ShutdownDatabase();
};

#endif // LOCAL_STORAGE_H