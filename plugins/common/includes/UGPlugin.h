#ifndef UG_PLUGIN_H
#define UG_PLUGIN_H

#define UG_SHOW_LOGS

#include <string>
#include <fstream>
#include <filesystem>
#include <functional>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include "common/libraries/LocalStorage/LocalStorage.h"
#include "common/libraries/MQUndergroundSDK/MQUndergroundSDK.h"

#pragma comment(lib, "LocalStorage")
#pragma comment(lib, "MQUndergroundSDK")

namespace fs = std::filesystem;
inline std::string CreateLogFilename(std::string_view baseName, std::chrono::system_clock::time_point timestamp);

class UGPlugin {
public:
	std::string id;
	std::string name;
	std::string version;
	std::string usage;
	std::string limits;
	LocalStorage* localStorage;

	UGPlugin(){}

	UGPlugin(const std::string& name, const std::string& id, const std::string& version) {
		this->name = name;
		this->id = id;
		this->version = version;
		InitializeLocalStorage("mq-underground.db");
		InitializeLogging();
	}
	~UGPlugin() {
		delete localStorage;
	}

	bool InitializePlugin() {
		token = localStorage->GetItem("mq-underground-token");
		authEmail = localStorage->GetItem("mq-underground-email");

		if (token) {
			GetPluginAccess();
			return true;
		}
		else {
			return false;
		}
	}

	bool HasAccess() {
		return accessStatus.find("Access") == 0;
	}

	bool InvalidVersion() {
		return accessStatus == "No access|Invalid version";
	}

	bool AtLimit() {
		return accessStatus.find("Limits") == 0;
	}


	void Login(const std::string& email, const std::string& password) 
	{
		MQUndergroundSDK sdk(email, password);
		token = new std::string(sdk.Login(id, version));
		localStorage->SetItem("mq-underground-token", *token);
		localStorage->SetItem("mq-underground-email", email);
		authEmail = new std::string(email);
	}

	void LogOut() {
		token = nullptr;
		authEmail = nullptr;
		localStorage->UnsetItem("mq-underground-token");
		localStorage->UnsetItem("mq-underground-email");
	}

	std::string GetPluginAccess() {
#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("GetPluginAccess - Start");
#endif
		if (token)
		{
			MQUndergroundSDK sdk(*token);
			accessStatus = sdk.GetPluginAccess(id, version, "");

			if (accessStatus.find("Limits") == 0 || accessStatus.find("Access") == 0) {
				std::vector<std::string> limitData = mq::split(accessStatus, '|');
				usage = limitData[1];
				limits = limitData[2];

#ifdef UG_SHOW_LOGS
				SPDLOG_DEBUG("GetPluginAccess - Success {}", accessStatus);
#endif
			}
			else {
#ifdef UG_SHOW_LOGS
				SPDLOG_DEBUG("GetPluginAccess - Success (No Access) {}", accessStatus);
#endif
			}
			return accessStatus;
		}

#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("GetPluginAccess - Failed (NO Token)");
#endif
		return "NO ACCESS";
	}

	std::string GetInitialPluginData() {
#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("GetInitialPluginData - Start");
#endif
		if (token)
		{
			MQUndergroundSDK sdk(*token);
			std::string data = sdk.GetInitialPluginData(id, version, "");

#ifdef UG_SHOW_LOGS
			SPDLOG_DEBUG("GetInitialPluginData - Success {}", data);
#endif
			return data;	
		}

#ifdef UG_SHOW_LOGS
			SPDLOG_DEBUG("GetInitialPluginData - Failed (No Token)");
#endif
	}

	bool SendHeartbeat(std::string& data) {

#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("SendHeartbeat - Start");
#endif
		if (token && connectionId != nullptr)
		{
			MQUndergroundSDK sdk(*token);
#ifdef UG_SHOW_LOGS
			SPDLOG_DEBUG("id - {}", id);
			SPDLOG_DEBUG("version - {}", version);
			SPDLOG_DEBUG("data - {}", data);
			SPDLOG_DEBUG("connectionId - {}", *connectionId);
#endif
			sdk.SendPluginHeartbeat(id, version, "&connectionId=" + *connectionId + data);
#ifdef UG_SHOW_LOGS
			SPDLOG_DEBUG("SendHeartbeat - Success");
#endif
			return true;
		}
#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("SendHeartbeat - Failed (No Token or ConnectionId)");
#endif
		return false;
	}

	bool Connect(const std::string& data = "") {
#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("Connect - Start");
#endif
		if (token)
		{
			MQUndergroundSDK sdk(*token);
			connectionId = new std::string(sdk.ConnectPlugin(id, version, data));
#ifdef UG_SHOW_LOGS
			SPDLOG_DEBUG("Connect - Success");
#endif
			return true;
		}
#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("Connect - Failed (No Token)");
#endif
		return false;
	}

	bool Disconnect(const std::string& data = "") {
#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("Disconnect - Start");
#endif
		if (token && connectionId != nullptr)
		{
			MQUndergroundSDK sdk(*token);
			sdk.DisconnectPlugin(id, version, "&connectionId=" + *connectionId + data);
			delete connectionId;
			connectionId = nullptr;

#ifdef UG_SHOW_LOGS
			SPDLOG_DEBUG("Disconnect - Success");
#endif
			return true;
		}

#ifdef UG_SHOW_LOGS
		SPDLOG_DEBUG("Disconnect - Failed (No Token or ConnectionId)");
#endif
		return false;
	}

	boolean IsLoggedIn() {
		return token != nullptr;
	}

	std::string GetEmail() {
		return *authEmail;
	}

	void InitializeLogging()
	{
#ifdef UG_SHOW_LOGS
		fs::path loggingPath = "PluignLogs";

		auto logger = std::make_shared<spdlog::logger>("MQ");
		spdlog::set_default_logger(logger);

		if (IsDebuggerPresent())
		{
			logger->sinks().push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
		}

#if LOG_FILENAMES
		spdlog::set_pattern("%L %Y-%m-%d %T.%f [%n] %v (%@)");
#else
		spdlog::set_pattern("%L %Y-%m-%d %T.%f [%n] %v");
#endif
		spdlog::flush_on(spdlog::level::trace);
		spdlog::set_level(spdlog::level::trace);

		fmt::memory_buffer filename;
		auto out = fmt::format_to(fmt::appender(filename), "{}\\{}\\{}", gPathLogs, "PluginLogs", CreateLogFilename(name, std::chrono::system_clock::now()));
		*out = 0;

		// Create file sink
		try
		{
			auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename.data(), true);
			logger->sinks().push_back(fileSink);
		}
		catch (const spdlog::spdlog_ex& ex)
		{
			SPDLOG_WARN("Failed to create file logger: {}, ex: {}",
				std::string_view(filename.data(), filename.size()), ex.what());
		}
		SPDLOG_DEBUG("Log Created");
#endif
	}


protected:
	void InitializeLocalStorage(const std::string& database) {
		localStorage = new LocalStorage((std::filesystem::path(gPathConfig) / database).string());
		localStorage->SetItem(std::string(name+"-version"), version);

		token = localStorage->GetItem("mq-underground-token");
		authEmail = localStorage->GetItem("mq-underground-email");
	}


private:
	std::string* token = nullptr;
	std::string* authEmail = nullptr;
	std::string* connectionId = nullptr;
	std::string accessStatus;

};



std::string CreateLogFilename(std::string_view baseName, std::chrono::system_clock::time_point timestamp)
{
	auto now_as_time_t = std::chrono::system_clock::to_time_t(timestamp);

	// Convert time_point to broken-down local time.
	std::tm local_tm;
	localtime_s(&local_tm, &now_as_time_t);

	// Calculate microseconds
	auto since_epoch = timestamp.time_since_epoch();
	auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
	auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch - seconds);

	fmt::memory_buffer buffer;
	fmt::format_to(fmt::appender(buffer),
		"{}-{:04d}{:02d}{:02d}T{:02d}{:02d}{:02d}.{:06}.log", baseName,
		local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
		local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec, microseconds.count());

	return fmt::to_string(buffer);
}


#endif