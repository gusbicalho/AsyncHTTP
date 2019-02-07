#include <stdio.h>
#include <windows.h>
#include <winhttp.h>

HINTERNET __declspec(dllexport) CreateSession(LPCWSTR agent, DWORD* error);
HINTERNET __declspec(dllexport) ConnectTo(HINTERNET hSession, LPCWSTR host, DWORD* error);
HINTERNET __declspec(dllexport) OpenRequest(HINTERNET hConnect, LPCWSTR object, LPCWSTR method, BOOL secure, DWORD* error);
BOOL __declspec(dllexport) AddRequestHeader(HINTERNET hRequest, LPCWSTR headers, DWORD headersLength, DWORD* error);
BOOL __declspec(dllexport) SendRequest(HINTERNET hRequest, char* done, DWORD* err, char* headerFileName, char* fileName,
				 LPVOID data, DWORD dataLength);
BOOL __declspec(dllexport) CloseHttpHandle(HINTERNET handle, DWORD* error);
