#ifndef MQ_UNDERGROUND_SDK_H
#define MQ_UNDERGROUND_SDK_H

#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <random>
#include <functional>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <curl/curl.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Wldap32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Normaliz.lib")
#pragma comment(lib, "libcurl_a.lib")



class MQUndergroundSDK {
public:
	MQUndergroundSDK(const std::string& username, const std::string& password);
	MQUndergroundSDK(const std::string& token);
	~MQUndergroundSDK();

	std::string Login(const std::string& pluginId, const std::string& version);
	std::string GetPluginAccess(const std::string& pluginId, const std::string& version, const std::string& data);
	std::string GetInitialPluginData(const std::string& pluginId, const std::string& version, const std::string& data);
	std::string ConnectPlugin(const std::string& pluginId, const std::string& version, const std::string& data);
	std::string DisconnectPlugin(const std::string& pluginId, const std::string& version, const std::string& data);


	std::string SendPluginHeartbeat(const std::string& pluginId, const std::string& version, const std::string& data);

private:
	std::string* username = nullptr;
	std::string* password = nullptr;
	std::string* token = nullptr;
	std::string domain = "https://mqunderground.com/api/";

	std::string MakeAPIRequest(const std::string& uri, const std::string& method, const std::string& data);
};

#endif // MQ_UNDERGROUND_SDK_H