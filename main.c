#include <stdio.h>
#include <windows.h>
#include <winhttp.h>
#include "AsyncHTTP.h"

// This struct holds info about a running request
typedef struct {
	HINTERNET hRequest; // handle
	FILE* headerFile; // file to save the headers
	FILE* file; // file to save the server response
	char* done; // it'll point to a ruby string, then we will change it to
				// notify the end of the request
	DWORD* err; // quite the same as "done", but used to notify errors
} RequestData;

// Creates a Session and returns its handle
// Uses the most common options for WinHttpOpen
// The only info you provide here is your "browser" name
HINTERNET CreateSession(LPCWSTR agent, DWORD* error) {
	int agentLength = wcslen(agent); // gets the length of the wide string
	HINTERNET hSession = NULL;

	// Look at MSDN for documentation on these parameters
	if (!(hSession = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC))) {
		// if WinHttpOpen return 0, there was an error
		*error = GetLastError(); // Send the error info back to Ruby
		return NULL;
	}

	return hSession;
}

// Creates a Connection and returns its handle
// Calls WinHttpConnect with common parameter values
// Here is where you define the Host you will "talk" to
HINTERNET ConnectTo(HINTERNET hSession, LPCWSTR host, DWORD* error) {
	HINTERNET hConnect = NULL;
	if (!(hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_PORT, 0))) {
		*error = GetLastError();
		return NULL;
	}
	return hConnect;
}

// Creates a Request and returns its handle
// This doesn't send anything to the server.
// Here you define the object you want to request (everything in the url after the domain),
// the method (GET or POST) and whether the connection should be secure or not.
HINTERNET OpenRequest(HINTERNET hConnect, LPCWSTR object, LPCWSTR method, BOOL secure, DWORD* error) {
	static LPCWSTR AcceptAllTypes[2] = {L"*/*",NULL};
	HINTERNET hRequest;

	if (!(hRequest = WinHttpOpenRequest(hConnect, method, object, NULL, NULL,
		AcceptAllTypes, WINHTTP_FLAG_REFRESH|(secure?WINHTTP_FLAG_SECURE:0) ))) {
		*error = GetLastError();
		return NULL;
	}

	return hRequest;
}

// This adds a request header if it doesn't exist, or replace it if it does
// This is used to define content-type of Post data and other info that is sent in the
// header of the request. You can google for more info.
BOOL AddRequestHeader(HINTERNET hRequest, LPCWSTR headers, DWORD headersLength, DWORD* error) {
	if (!WinHttpAddRequestHeaders(hRequest, headers, headersLength,
		WINHTTP_ADDREQ_FLAG_ADD|WINHTTP_ADDREQ_FLAG_REPLACE)) {
			*error = GetLastError();
			return FALSE;
	}
	return TRUE;
}

// This sends the error info to ruby through the pointer in the RequestData object
// Just to make code easier to read.
void throwError(RequestData *req, DWORD error) {
	*(req->err) = error;
}

// This is the Callback function. It does most of the hard work.
//   When you use WinHTTP synchronously, when you send a request, you must make a loop
// reading the incoming data, and each operation takes a long time. When I tried to use
// synchronous operation with RGSS, the game would freeze until the call completed.
//   So i needed to switch to asynchronous operation. Using async (shorter XD) operation,
// you must set a Callback function that will be called whenever something happens, like a
// server response arriving or the connection dying, instead of checking that with a loop.
// There's no way to use a Ruby function as a C pointer-to-function. That would've made
// everything MUCH easier, but it can't be done (not with common Ruby Win32API class, at
// least). So I coded the callback in C, and it "talks" to ruby through two pointers (that
// are actually Ruby strings) stored in the RequestData: the done pointer and the err pointer.
//   Using async operations, the Send, Query and Read methods all return immediately, instead
// of waiting for the task to complete. The callback is called when it happens.
//   The callback does everything it needs to do to save the incoming data and when the job
// is over (or failed) it notifies the RGSS Player.
void CALLBACK Callback(HINTERNET hRequest, DWORD_PTR context, DWORD status, LPVOID statusInfo, DWORD statusInfoLength) {
	// The Context is a RequestData*
	RequestData* req = (RequestData*)context;
	switch (status) { // the status parameter tells us what kind of event happened
	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: // Finished sending the request
		// Now we have to receive the response
		// This tell the server it won't send any more data, so the server can start responding
		if (!WinHttpReceiveResponse(hRequest, 0)) {
			// If WinHttpReceiveResponse returns 0, an error occurred
			// it "throws" the error, closes the files and free the memory used by the context
			throwError(req,GetLastError());
			if (req->headerFile)
				fclose(req->headerFile);
			fclose(req->file);
			free(req);
		}
		return;
	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: // Server has finished sending the response headers
		if (req->headerFile != NULL) { // if it should save the headers
			WCHAR* buff = 0;
			DWORD buffLen = 0;
			WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
				WINHTTP_NO_OUTPUT_BUFFER, &buffLen, WINHTTP_NO_HEADER_INDEX);
				// When we call QueryHeaders without an output buffer, it returns the size of the needed buffer
				// in the variable buffLen.
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
				// ERROR_INSUFICIENT_BUFFER is expected, since we didn't provide a buffer
				// If some other ocurred, then it is thrown and the operation is aborted
				throwError(req,GetLastError());
				fclose(req->headerFile);
				fclose(req->file);
				free(req);
				return;
			}
			// Now that we have the needed buffer length, we allocate the buffer and call QueryHeaders again
			buff = (WCHAR*)malloc(sizeof(WCHAR)*buffLen);
			if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
				buff, &buffLen, WINHTTP_NO_HEADER_INDEX)) {
				// If QueryHeaders return 0, then an error occurred
				throwError(req,GetLastError());
				fclose(req->headerFile);
				fclose(req->file);
				free(req);
				return;
			}
			// Everything went well, so we write the buffer to the file...
			fprintf(req->headerFile,"%ls",buff);
			free(buff); // ...and free it...
			fclose(req->headerFile); // ...and close the file.
			req->headerFile = NULL;
		}
		// Now we start getting actual data. First of all, we ask how much data is available
		if (!WinHttpQueryDataAvailable(hRequest, NULL)) {
			// You already know what is happening here ;)
			throwError(req,GetLastError());
			fclose(req->file);
			free(req);
			return;
		}
		return;
	case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: // There is some data available
		{
			// I open blocks like this so I can declare variables
			// in C, you can declare variables only at the begginig of blocks
			DWORD size = *(LPDWORD)statusInfo; // statusInfo points to the amount of data available
			LPSTR buff = 0;
			if (size == 0) { // if size if 0, the download is complete.
				fclose(req->file);
				*(req->done) = 1; // Notify Ruby that the download is over.
				free(req);
				return;
			}
			// if we still have some data to receive, allocate a buffer to hold it
			buff = (LPSTR)malloc(size);
			ZeroMemory(buff, size);
			if (!WinHttpReadData(hRequest, buff, size, NULL)) { // Let's read the data =P
				throwError(req,GetLastError());
				fclose(req->file);
				free(req);
				return;
			}
		}
		return;
	case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: // Finished reading some amount of data
		if (statusInfoLength != 0) { // This shouldn't be 0, anyway... just to make sure XD
			if (fwrite(statusInfo, 1, statusInfoLength, req->file) != statusInfoLength) { // writes data to file
				// here, some error occurred
				throwError(req,GetLastError());
				fclose(req->file);
				free(req);
				free(statusInfo);
				return;
			}
			free(statusInfo); // frees the buffer
			if (!WinHttpQueryDataAvailable(hRequest, NULL)) { // Queries for more data
				throwError(req,GetLastError());
				fclose(req->file);
				free(req);
				return;
			}
		}
		return;
	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: // An error occured
		throwError(req,((WINHTTP_ASYNC_RESULT *)statusInfo)->dwError);
		if (req->headerFile)
			fclose(req->headerFile);
		fclose(req->file);
		free(req);
		return;
	}
}

// This function prapares and sends the Request.
// Here you define the files where headers and content should be saved.
// You can also provide data to be sent through Post.
BOOL SendRequest(HINTERNET hRequest, char* done, DWORD* err, char* headerFileName, char* fileName,
				 LPVOID data, DWORD dataLength) {
	RequestData* req;

	if (!fileName) {
		*err = ERROR_INVALID_PARAMETER;
		return FALSE;
	}

	WinHttpSetStatusCallback(hRequest, Callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, 0); // set the callback
	req = (RequestData*)malloc(sizeof(RequestData)); // Allocate a RequestData to hold request info
	req->hRequest = hRequest;
	req->done = done;
	req->err = err; // Fill the fields
	if (!headerFileName)
		req->headerFile = NULL;
	else { // Open header file
		req->headerFile = fopen(headerFileName,"w");
		if (!req->headerFile) {
			*err = GetLastError();
			free(req);
			return FALSE;
		}
	}
	req->file = fopen(fileName,"wb"); // open content file
	if (!req->file) {
		*err = GetLastError();
		if (req->headerFile)
			fclose(req->headerFile);
		free(req);
		return FALSE;
	}

	// Now we actually send the Request. It will start the Callback calls.
	if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, -1L, data, dataLength, dataLength, req)) {
		*err = GetLastError();
		if (req->headerFile)
			fclose(req->headerFile);
		fclose(req->file);
		free(req);
		return FALSE;
	}
	return TRUE;
}

// This closes the handles and free the memory used.
BOOL CloseHttpHandle(HINTERNET handle, DWORD* error) {
	if (!WinHttpCloseHandle(handle)) {
		*error = GetLastError();
		return FALSE;
	}
	return TRUE;
}

