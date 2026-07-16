// LocalStorage.cpp : Defines the functions for the static library.
//


#include "pch.h"
#include "framework.h"
#include "MQUndergroundSDK.h"


static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s)
{
	size_t newLength = size * nmemb;
	try
	{
		s->append((char*)contents, newLength);
	}
	catch (std::bad_alloc& e)
	{
		return 0;
	}
	return newLength;
}

std::string StripQuotes(std::string input) {
	if (input.size() >= 2 && input.front() == '"' && input.back() == '"') {
		return input.substr(1, input.size() - 2);
	}
	return input;
}


MQUndergroundSDK::MQUndergroundSDK(const std::string& username, const std::string& password)
	: username(new std::string(username)), password(new std::string(password)), token(nullptr) {

}

// Constructor for token
MQUndergroundSDK::MQUndergroundSDK(const std::string& token)
	: username(nullptr), password(nullptr), token(new std::string(token)) {

}

MQUndergroundSDK::~MQUndergroundSDK() {
	if (username) {
		delete username;
	}
	if (password) {
		delete password;
	}
	if (token) {
		delete token;
	}

}

std::string MQUndergroundSDK::Login(const std::string& pluginId, const std::string& version) {
	if (username == nullptr || password == nullptr) {
		return "false";
	}

	std::string response = MakeAPIRequest("auth", "POST", "pluginId=" + pluginId + "&version=" + version + "&email=" + *username + "&password=" + *password);


	return response;
}

std::string MQUndergroundSDK::GetPluginAccess(const std::string& pluginId, const std::string& version, const std::string& data) {
	return MakeAPIRequest("plugins/"+pluginId, "POST", "version=" + version + "&type=access"+data);
}

std::string MQUndergroundSDK::GetInitialPluginData(const std::string& pluginId, const std::string& version, const std::string& data) {
	return MakeAPIRequest("plugins/" + pluginId, "POST", "version=" + version + "&type=initial" + data);
}

std::string MQUndergroundSDK::SendPluginHeartbeat(const std::string& pluginId, const std::string& version, const std::string& data) {
	return MakeAPIRequest("plugins/" + pluginId, "POST", "version=" + version + "&type=heartbeat" + data);
}
std::string MQUndergroundSDK::ConnectPlugin(const std::string& pluginId, const std::string& version, const std::string& data) {
	return MakeAPIRequest("plugins/" + pluginId, "POST", "version=" + version + "&type=connect" + data);
}
std::string MQUndergroundSDK::DisconnectPlugin(const std::string& pluginId, const std::string& version, const std::string& data) {
	return MakeAPIRequest("plugins/" + pluginId, "POST", "version=" + version + "&type=disconnect" + data);
}


std::string MQUndergroundSDK::MakeAPIRequest(const std::string& uri, const std::string& method, const std::string& data) {
	CURL* curl = curl_easy_init();
	if (!curl) {
		throw std::runtime_error("Curl initialization failed");
	}

	std::string response_data;
	std::string full_url = domain + uri;
	curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

	struct curl_slist* headers = NULL;
	headers = curl_slist_append(headers, "Accept: application/json");

	if (token && !token->empty()) {
		std::string bearer = "Authorization: Bearer " + *token;
		headers = curl_slist_append(headers, bearer.c_str());
	}

	if (method == "POST") {
		headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
	}
	else if (method == "GET") {
		curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	}

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(curl);

	if (res != CURLE_OK) {
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		throw std::runtime_error(curl_easy_strerror(res));
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code != 200) {
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		throw std::runtime_error(StripQuotes(response_data));
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return StripQuotes(response_data);
}