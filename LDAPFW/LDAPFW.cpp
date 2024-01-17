// LDAPFW.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <numeric>
#include <vector>
#include <fstream>
#include <json/json.h>
#include "Injections.h"
#include "common.h"
#include<windows.h>  
#include <stdexcept>
#include <detours.h>
#include <strsafe.h>
#include <shlwapi.h>
#pragma comment(lib,"shlwapi.lib")
#include "shlobj.h"
#include <locale>
#include <codecvt>
#include <exception>
#include <format>
#include "config.h"
#include "utils.h"
#include "rules.h"
#include "service.h"

#define LDAPFW_VERSION "0.0.7"
#define GLOBAL_LDAPFW_EVENT_UNPROTECT TEXT("Global\\LdapFwUninstalledEvent")
#define LDAPFW_PIPE_NAME TEXT("\\\\.\\Pipe\\LDAPFW")
#define PIPE_BUFFER_SIZE 1024
#define PROVIDER_NAME TEXT("LDAPFW")
#define DLL_PATH TEXT("%SystemRoot%\\system32\\ldapFW.dll")
#define SYMBOLS_PATH_ENV_VAR "_NT_SYMBOL_PATH"

HANDLE hPipe;
HANDLE hNamedPipe;
HANDLE ntdsaiHandle = LoadLibrary(TEXT("c:\\windows\\system32\\ntdsai.dll"));

enum class eventSignal { signalSetEvent, signalResetEvent };

struct LdapFwOffsetException : public std::exception {
	const char* what() const throw () {
		return "Cannot resolve LDAP function offset";
	}
};

inline const bool const StringToBool(wchar_t* str)
{
	WCHAR lowerStr[WCHAR_MAX];

	wcscpy_s(lowerStr, str);
	_wcslwr_s(lowerStr);
	return wcscmp(lowerStr, L"true") == 0 ? true : false;
}

std::vector<std::string> ALLOWED_ARGUMENTS = { "/help", "/status", "/validate", "/install", "/uninstall", "/update" };

struct compare
{
	std::string key;
	compare(std::string const& s) : key(s) {}

	bool operator()(std::string const& s) {
		return (s == key);
	}
};

void printInColor(const char* message, int color)
{
	HANDLE hStd = GetStdHandle(STD_OUTPUT_HANDLE);
	WORD wCurrentColorAttrs;
	CONSOLE_SCREEN_BUFFER_INFO csbiInfo;
	GetConsoleScreenBufferInfo(hStd, &csbiInfo);
	wCurrentColorAttrs = csbiInfo.wAttributes;

	SetConsoleTextAttribute(hStd, color);
	std::cout << message;
	SetConsoleTextAttribute(hStd, wCurrentColorAttrs);
}

bool writeFileToSysfolder(const std::wstring& sourcePath, const std::wstring& sourceFileName)
{
	wchar_t  destPath[INFO_BUFFER_SIZE];
	DWORD  bufCharCount = INFO_BUFFER_SIZE;

	if (!GetSystemDirectory(destPath, INFO_BUFFER_SIZE))
	{
		std::cout << "ERROR: Couldn't get the system directory [" << GetLastError() << "]." << std::endl;
		return false;
	}

	std::wstring destPathStr = destPath;
	destPathStr += TEXT("\\");
	destPathStr += sourceFileName;

	if (!CopyFile(sourcePath.c_str(), destPathStr.c_str(), false))
	{
		std::wcout << "ERROR: " << sourcePath << " copy to system folder failed[" << GetLastError() << "]." << std::endl;
		return false;
	}

	return true;
}

bool deleteFileFromSysfolder(std::wstring fileName)
{

	wchar_t  destPath[INFO_BUFFER_SIZE];
	DWORD  bufCharCount = INFO_BUFFER_SIZE;

	if (!GetSystemDirectory(destPath, INFO_BUFFER_SIZE))
	{
		std::cout << "ERROR: Couldn't get the system directory [" << GetLastError() << "]." << std::endl;
		return false;
	}

	std::wstring destPathStr = destPath;
	destPathStr += TEXT("\\");
	destPathStr += fileName;

	if (!DeleteFile(destPathStr.c_str()))
	{
		DWORD LastError = GetLastError();
		if (LastError != ERROR_FILE_NOT_FOUND)
		{
			std::wcout << "ERROR: " << destPathStr << " delete operation from system folder failed[" << GetLastError() << "]." << std::endl;
			return false;
		}
	}

	return true;
}

void sendSignalToGlobalEvent(wchar_t* globalEventName, eventSignal eSig)
{
	HANDLE hEvent = createGlobalEvent(true, false, globalEventName);

	if (hEvent == nullptr)
	{
		_tprintf(TEXT("Could not get handle to event %s, error: %d\n"), globalEventName, GetLastError());
		return;
	}

	if (eSig == eventSignal::signalSetEvent)
	{
		if (SetEvent(hEvent) == 0)
		{
			_tprintf(TEXT("Setting the event %s failed: %d.\n"), globalEventName, GetLastError());
		}
	}
	else
	{
		if (ResetEvent(hEvent) == 0)
		{
			_tprintf(TEXT("Resetting the event %s failed: %d.\n"), globalEventName, GetLastError());
		}
	}
}

bool doesEnvironmentVariableExist(const char* variable)
{
	if (getenv(variable)) {
		return true;
	}
	else {
		return false;
	}
}

bool isLdapFwInstalled()
{
	auto lsassProcessId = FindProcessId(L"lsass.exe");
	if (lsassProcessId == 0) {
		std::cout << "Error, could not find lsass.exe process";
		exit(-1);
	}

	if (isDllLoaded(lsassProcessId, L"c:\\windows\\system32\\ldapfw.dll")) {
		return true;
	}
	else {
		return false;
	}
}

bool injectFirewall() 
{
	auto lsassProcessId = FindProcessId(L"lsass.exe");
	if (lsassProcessId == 0) {
		std::cout << "Error, could not find lsass.exe process";
	}
	else {
		if (!isLdapFwInstalled()) {
			return hookProcessLoadLibrary(lsassProcessId);
		} else {
			std::cout << "LDAP Firewall already installed." << std::endl;
		}
	}

	return false;
}

void installFirewall()
{
	if (isProcessProtected(L"lsass.exe")) {
		std::cout << "LSA protection is enabled, cannot proceed." << std::endl;
		exit(-1);
	}

	if (!copyDllsToSystemPath()) {
		exit(-1);
	}
	
	addEventSource();
}

void startFirewall()
{
	std::string config = loadConfigFile();
	validateJsonOrExit(config);

	std::string configWithOffsets = "";
	std::string logPathBeforeElevation = generateLogPath();

	elevateCurrentProcessToSystem();

	if (isLdapFwInstalled()) {
		std::cout << "Already installed";
		exit(0);
	}

	try {
		configWithOffsets = enrichConfig(config, logPathBeforeElevation);
	}
	catch (LdapFwOffsetException& e) {
		std::cout << "Failed to load debug symbols, aborting";
		exit(-1);
	}
	catch (Json::RuntimeError& e) {
		std::cout << "Failed to parse config.json, aborting";
		exit(-1);
	}

	std::cout << "Installing LDAP Firewall..." << std::endl;

	createAllGloblEvents();

	setupNamedPipe();

	if (injectFirewall()) {
		std::cout << "Loading LDAP Firewall configuration..." << std::endl;
		writeToNamedPipe(configWithOffsets);
	}

	cleanup();
}

bool stopFirewall() 
{
	elevateCurrentProcessToSystem();

	if (!isLdapFwInstalled()) {
		std::cout << "LDAPFW not installed" << std::endl;
		return false;
	}

	std::cout << "Uninstalling LDAP Firewall..." << std::endl;

	sendSignalToGlobalEvent((wchar_t*)GLOBAL_LDAPFW_EVENT_UNPROTECT, eventSignal::signalSetEvent);

	cleanup();

	return true;
}

void uninstallFirewall()
{
	auto lsassProcessId = FindProcessId(L"lsass.exe");
	if (lsassProcessId == 0) {
		std::cout << "Error, could not find lsass.exe process";
	}

	std::cout << "Waiting for DLLs to unload";

	while (isDllLoaded(lsassProcessId, L"c:\\windows\\system32\\ldapfw.dll")) {
		std::cout << ".";
		Sleep(1000);
	}

	std::cout << std::endl;

	deleteFileFromSysfolder(LDAP_FW_DLL_NAME);
	if (!deleteFileFromSysfolder(LDAP_MESSAGES_DLL_NAME)) {
		std::cout << "Please make sure the Event Viewer is closed" << std::endl;
		exit(-1);
	}

	if (deleteEventSource())
	{
		std::cout << "Event Log successfully removed..." << std::endl;
	}
	else
	{
		std::cout << "deleteEventSource failed: " << GetLastError() << std::endl;
		exit(-1);
	}
}

void printHelp() 
{
	std::cout << "Usage: ldapFwManager /<Command> [options]" << std::endl << std::endl;
	std::cout << "Command:" << std::endl;
	std::cout << "----------" << std::endl;
	std::cout << "/install - install LDAP Firewall as a service" << std::endl;
	std::cout << "/uninstall - remove LDAP Firewall service" << std::endl;
	std::cout << "/update - reload config.json and update the LDAPFW configuration (while installed)" << std::endl;
	std::cout << "/status - print status" << std::endl;
	std::cout << "/validate - verify that the config.json file is formatted correctly" << std::endl;
	std::cout << "/help - show this message and exit" << std::endl;
}

void setupNamedPipe()
{
	hNamedPipe = CreateNamedPipe(
		LDAPFW_PIPE_NAME,
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
		PIPE_UNLIMITED_INSTANCES,
		PIPE_BUFFER_SIZE,
		PIPE_BUFFER_SIZE,
		NMPWAIT_USE_DEFAULT_WAIT,
		NULL);
}

bool writeToNamedPipe(const std::string config)
{
	if (hNamedPipe != INVALID_HANDLE_VALUE) {
		if (ConnectNamedPipe(hNamedPipe, NULL) != ERROR_PIPE_CONNECTED)
		{
			DWORD cbBytes;

			BOOL bResult = WriteFile(
				hNamedPipe,
				config.c_str(),
				strlen(config.c_str()),
				&cbBytes,
				NULL);

			FlushFileBuffers(hNamedPipe);

			if ((!bResult) || (strlen(config.c_str()) != cbBytes))
			{
				std::cout << "Error occurred while writing to the server: " << GetLastError() << std::endl;
				CloseHandle(hNamedPipe);
				return false;
			}
		} else {
			std::cout << "Error while setting up NamedPipe: " << GetLastError() << std::endl;
		}
	}

	return true;
}

std::string generateLogPath()
{
	TCHAR moduleFileName[MAX_PATH];

	if (!GetModuleFileName(nullptr, moduleFileName, MAX_PATH)) {
		std::cout << "GetModuleFileName failed " << GetLastError();
		exit(-1);
	}

	std::wstring::size_type posOfDirectory = std::wstring(moduleFileName).find_last_of(L"\\/");
	std::string currentDirectory = wideStringToString(std::wstring(moduleFileName).substr(0, posOfDirectory  + 1));

	return currentDirectory + "LDAPFW.log";
}

std::string generateSymbolsPath()
{
	TCHAR buffer[MAX_PATH] = { 0 };
	GetModuleFileName(NULL, buffer, MAX_PATH);
	std::wstring::size_type pos = std::wstring(buffer).find_last_of(L"\\/");
	std::wstring folderPathW = std::wstring(buffer).substr(0, pos);
	
	LPWSTR pFolderPath = &folderPathW[0];
	wchar_t folderPath[MAX_PATH];
	wcscpy_s(folderPath, pFolderPath);
	PathAppend(folderPath, SYMBOLS_DIRECTORY);
	CreateDirectory(folderPath, NULL);
	

	std::wstring symbolsPathW(folderPath);
	std::string symbolsPath(symbolsPathW.begin(), symbolsPathW.end());

	return std::format("SRV*{}*http://msdl.microsoft.com/download/symbols", symbolsPath);;
}

int calculateFunctionOffset(const char* functionName)
{

	PVOID fncPtr = DetourFindFunction("c:\\windows\\system32\\ntdsai.dll", functionName);

	if (!fncPtr) {
		throw LdapFwOffsetException();
	}

	return (char*)fncPtr - (char*)ntdsaiHandle;
}

bool createSymbolsEnvironmentVariableIfNotExist()
{
	if (doesEnvironmentVariableExist(SYMBOLS_PATH_ENV_VAR)) {
		return true;
	}

	std::string symbolsPath = generateSymbolsPath();

	std::cout << "Setting symbols path to: " << symbolsPath << std::endl;

	if (0 != _putenv_s(SYMBOLS_PATH_ENV_VAR, symbolsPath.c_str())) {
		std::cout << "Could not set " << SYMBOLS_PATH_ENV_VAR << " environment variable" << std::endl;
		return false;
	}

	return true;
}

void validateJsonOrExit(const std::string& jsonConfig)
{
	Json::Value root;
	std::stringstream configBuffer;
	configBuffer << jsonConfig;

	try {
		configBuffer >> root;
	}
	catch (Json::RuntimeError& e) {
		std::cout << "Invalid config.json file";
		exit(-1);
	}
}

std::string enrichConfig(const std::string& jsonConfig, std::string logPath)
{
	createSymbolsEnvironmentVariableIfNotExist();

	Json::Value root;
	std::stringstream configBuffer;
	configBuffer << jsonConfig;
	configBuffer >> root;

	root["offsets"]["addRequest"] = calculateFunctionOffset("?AddRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@PEAVLDAP_REQUEST@@PEAULDAPMsg@@PEAPEAUReferral_@@PEAPEAUControls_@@PEAULDAPString@@5H@Z");
	root["offsets"]["delRequest"] = calculateFunctionOffset("?DelRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@PEAVLDAP_REQUEST@@PEAULDAPMsg@@PEAPEAUReferral_@@PEAPEAUControls_@@PEAULDAPString@@5H@Z");
	root["offsets"]["modifyRequest"] = calculateFunctionOffset("?ModifyRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@PEAVLDAP_REQUEST@@PEAULDAPMsg@@PEAPEAUReferral_@@PEAPEAUControls_@@PEAULDAPString@@5H@Z");
	root["offsets"]["modifyDNRequest"] = calculateFunctionOffset("?ModifyDNRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@PEAVLDAP_REQUEST@@PEAULDAPMsg@@PEAPEAUReferral_@@PEAPEAUControls_@@PEAULDAPString@@5H@Z");
	root["offsets"]["compareRequest"] = calculateFunctionOffset("?CompareRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@PEAVLDAP_REQUEST@@PEAULDAPMsg@@PEAPEAUReferral_@@PEAPEAUControls_@@PEAULDAPString@@5@Z");
	root["offsets"]["init"] = calculateFunctionOffset("?Init@LDAP_CONN@@QEAAHPEAUsockaddr@@KPEAU_ATQ_CONTEXT_PUBLIC@@K@Z");
	root["offsets"]["cleanup"] = calculateFunctionOffset("?Cleanup@LDAP_CONN@@QEAAXXZ");
	root["offsets"]["setSecurityContextAtts"] = calculateFunctionOffset("?SetSecurityContextAtts@LDAP_CONN@@AEAA?AW4_enum1@@PEAVLDAP_SECURITY_CONTEXT@@KKHPEAULDAPString@@@Z");
	root["offsets"]["getUserNameA"] = calculateFunctionOffset("?GetUserNameA@LDAP_SECURITY_CONTEXT@@QEAAXPEAPEAG@Z");
	root["offsets"]["getUserSIDFromCurrentToken"] = calculateFunctionOffset("?GetUserSIDFromCurrentToken@@YAKPEAU_THSTATE@@PEAPEAX@Z");

	try {
		root["offsets"]["searchRequest"] = calculateFunctionOffset("?SearchRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@HPEAHPEAVLDAP_REQUEST@@KPEAULDAPMsg@@PEAPEAUReferral_@@PEAPEAUControls_@@PEAULDAPString@@6PEAKPEAPEAUberval@@@Z");
		root["offsets"]["searchRequestVersion"] = 1;
	}
	catch (LdapFwOffsetException e) {
		root["offsets"]["searchRequest"] = calculateFunctionOffset("?SearchRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@HPEAHPEAVLDAP_REQUEST@@KPEAULDAPMsg@@PEAPEAUReferral_@@PEAPEAUControls_@@PEAULDAPString@@6PEAU_SEARCH_LOGGING@@PEAKPEAPEAUberval@@@Z");
		root["offsets"]["searchRequestVersion"] = 2;
	}

	try {
		root["offsets"]["extendedRequest"] = calculateFunctionOffset("?ExtendedRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@PEAVLDAP_REQUEST@@PEAULDAPMsg@@PEAPEAUReferral_@@PEAPEAUControls_@@PEAULDAPString@@5PEAULDAPOID@@5@Z");
		root["offsets"]["extendedRequestVersion"] = 5;
	} catch (LdapFwOffsetException e) {
		root["offsets"]["extendedRequest"] = calculateFunctionOffset("?ExtendedRequest@LDAP_CONN@@AEAA?AW4_enum1@@PEAU_THSTATE@@PEAVLDAP_REQUEST@@PEAULDAPMsg@@PEAPEAUReferral_@@PEAULDAPString@@4PEAULDAPOID@@4@Z");
		root["offsets"]["extendedRequestVersion"] = 4;
	}
		
	root["logPath"] = logPath;

	Json::FastWriter fastWriter;
	return fastWriter.write(root);
}

std::string getLocalFilePath(std::string fileName) 
{
	char modulePath[MAX_PATH];

	if (!GetModuleFileNameA(nullptr, modulePath, MAX_PATH))
	{
		std::cout << "GetModuleFileName failed: " << GetLastError() << std::endl;
		return "";
	}

	std::string::size_type pos = std::string(modulePath).find_last_of("\\/");

	if (pos == std::string::npos) {
		return "";
	}
	else {
		return std::string(modulePath).substr(0, pos) + "\\" + fileName;
	}
}

std::string loadConfigFile()
{
	std::string line, text;
	std::ifstream config_doc(getLocalFilePath("config.json"), std::ifstream::binary);
	
	while (std::getline(config_doc, line))
	{
		text += line;
	}

	return text;
}

void createAllGloblEvents()
{
	globalUnprotectEvent = createGlobalEvent(true, false, (wchar_t*)GLOBAL_LDAPFW_EVENT_UNPROTECT);
}

void cleanup()
{
	if (hNamedPipe) {
		DisconnectNamedPipe(hNamedPipe);
		CloseHandle(hNamedPipe);
	}

	WaitForSingleObject(globalUnprotectEvent, 5000);
	CloseHandle(globalUnprotectEvent);
}

bool copyDllsToSystemPath()
{
	bool success = true;
	
	std::wstring ldapFWDllPath = stringToWideString(getLocalFilePath(wideStringToString(LDAP_FW_DLL_NAME)));
	std::wstring ldapMessagesDllPath = stringToWideString(getLocalFilePath(wideStringToString(LDAP_MESSAGES_DLL_NAME)));

	if (!writeFileToSysfolder(ldapFWDllPath, LDAP_FW_DLL_NAME)) {
		success = false;
	}
	
	if (!writeFileToSysfolder(ldapMessagesDllPath, LDAP_MESSAGES_DLL_NAME)) {
		success = false;
	}

	return success;
}

bool isFlagInArgs(int argc, char* argv[], std::string flag)
{
	for (int i = 0; i < argc; i++) {
		if (argv[i] == flag) {
			return true;
		}
	}

	return false;
}

void printStatus() {
	elevateCurrentProcessToSystem();
	bool ldapFwInstalled = isLdapFwInstalled();
	bool offsetsValid = true;

	std::string configFile = loadConfigFile();

	try {
		configFile = enrichConfig(configFile, "");
	}
	catch (LdapFwOffsetException& e) {
		offsetsValid = false;
	}

	Config loadedConfig = loadConfigFromJson(configFile);

	bool readyToInstall = !ldapFwInstalled && offsetsValid;

	std::cout << "LDAP Firewall v" << LDAPFW_VERSION << std::endl << std::endl;
	std::cout << "Status:" << std::endl << "---------------------" << std::endl;
	std::cout << "LDAPFW Installed:\t\t" << BoolToString(ldapFwInstalled) << std::endl;
	std::cout << "Symbols loaded:\t\t\t" << BoolToString(offsetsValid) << std::endl;
	
	int color = FOREGROUND_RED;
	if (ldapFwInstalled) {
		std::cout << "Ready to install:\t\t" << BoolToString(readyToInstall);
	} else if (readyToInstall) {
		printInColor(std::format("Ready to install:\t\t{}", BoolToString(readyToInstall)).c_str(), FOREGROUND_GREEN);
	}
	else {
		printInColor(std::format("Ready to install:\t\t{}", BoolToString(readyToInstall)).c_str(), FOREGROUND_RED);
	}

	if (loadedConfig.DebugLogging == false) return;

	std::cout << std::endl << std::endl;
	std::cout << "Resolved offsets:" << std::endl << "---------------------" << std::endl;
	std::cout << "AddRequest:\t\t\t" << loadedConfig.AddRequestOffset << std::endl;
	std::cout << "DelRequest:\t\t\t" << loadedConfig.DelRequestOffset << std::endl;
	std::cout << "ModifyRequest:\t\t\t" << loadedConfig.ModifyRequestOffset << std::endl;
	std::cout << "ModifyDNRequest:\t\t" << loadedConfig.ModifyDNRequestOffset << std::endl;
	std::cout << "SearchRequest:\t\t\t" << loadedConfig.SearchRequestOffset << std::endl;
	std::cout << "SearchRequest Version:\t\t" << loadedConfig.SearchRequestVersion << std::endl;
	std::cout << "CompareRequest:\t\t\t" << loadedConfig.CompareRequestOffset << std::endl;
	std::cout << "ExtendedRequest:\t\t" << loadedConfig.ExtendedRequestOffset << std::endl;
	std::cout << "ExtendedRequest Version:\t" << loadedConfig.ExtendedRequestVersion << std::endl;
	std::cout << "Init:\t\t\t\t" << loadedConfig.InitOffset << std::endl;
	std::cout << "Cleanup:\t\t\t" << loadedConfig.CleanupOffset << std::endl;
	std::cout << "SetSecurityContextAtts:\t\t" << loadedConfig.SetSecurityContextAttsOffset << std::endl;
	std::cout << "GetUserNameA:\t\t\t" << loadedConfig.GetUserNameAOffset << std::endl;
	std::cout << "GetUserSIDFromCurrentToken:\t" << loadedConfig.GetUserSIDFromCurrentTokenOffset;
}

bool verifyArguments(int argc, char* argv[])
{
	for (int i = 1; i < argc; i++) {
		if (std::none_of(ALLOWED_ARGUMENTS.begin(), ALLOWED_ARGUMENTS.end(), compare(argv[i]))) {
			std::cout << "Unknown argument " << argv[i] << std::endl << std::endl;
			printHelp();
			return false;
		}
	}
	

	return true;
}

int main(int argc, char* argv[])
{
	interactive = !setupService();
	
	if (!interactive) return 0;

	if (!verifyArguments(argc, argv)) {
		return -1;
	}

	if (argc == 1 || isFlagInArgs(argc, argv, "/help")) {
		printHelp();
		return 0;
	} else if (isFlagInArgs(argc, argv, "/validate")) {
		std::string config = loadConfigFile();
		validateJsonOrExit(config);
		std::cout << "Config file is valid";
		return 0;
	}

	if (!isRunningAsAdmin()) {
		std::cout << "Please run LDAPFW as administrator." << std::endl;
		exit(-1);
	}

	if (isFlagInArgs(argc, argv, "/status")) {
		printStatus();
		return 0;
	} else if (isFlagInArgs(argc, argv, "/install")) {
		installFirewall();
		serviceInstall(SERVICE_DEMAND_START);
		serviceStart();
		serviceMakeAutostart();
	} else if (isFlagInArgs(argc, argv, "/uninstall")) {
		serviceStop();
		elevateCurrentProcessToSystem();
		uninstallFirewall();
		serviceUninstall();
	} else if (isFlagInArgs(argc, argv, "/update")) {
		elevateCurrentProcessToSystem();

		if (!isLdapFwInstalled()) {
			std::cout << "LDAPFW not installed";
			return 0;
		}

		std::string config = loadConfigFile();
		validateJsonOrExit(config);

		std::cout << "Updating LDAP Firewall configuration..." << std::endl;

		createAllGloblEvents();
		setupNamedPipe();
		writeToNamedPipe(config);
		cleanup();
	} else {
		std::cout << "Can't parse arguments. Run with /help to get a list of valid commands";
		return -1;
	}

	std::cout << "Done.";

    return 0;
}