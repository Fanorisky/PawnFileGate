/*
 * PawnREST - SA-MP shared implementation header
 * Follows Pawn-Randomix style: standalone core + Impl* wrappers.
 */
#pragma once

#ifndef BUILD_SAMP_PLUGIN
#error "pawnrest_impl.hpp is intended for BUILD_SAMP_PLUGIN builds."
#endif

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <direct.h>
    #include <io.h>
    #include <windows.h>
    #define mkdir(path, mode) _mkdir(path)
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <ctime>
#endif

#include "types.inl"
#include "json.inl"
#include "utils.inl"
#include <crc32.hpp>
#include "core_samp.inl"

#include <climits>

namespace PawnREST {
    inline PawnRESTCore& GetCore() {
        static PawnRESTCore instance;
        return instance;
    }
    inline bool g_Initialized = false;
}

inline bool ImplInitialize() {
    if (PawnREST::g_Initialized) return true;
    PawnREST::GetCore().Initialize();
    PawnREST::g_Initialized = true;
    return true;
}

inline void ImplShutdown() {
    if (!PawnREST::g_Initialized) return;
    PawnREST::GetCore().Shutdown();
    PawnREST::g_Initialized = false;
}

inline void ImplSetLogger(void (*sink)(const char*)) {
    PawnREST::GetCore().SetLogger(sink);
}

// -----------------------------------------------------------------------------
// Server control
// -----------------------------------------------------------------------------

inline bool ImplStart(int port) { return PawnREST::GetCore().Start(port); }
inline bool ImplStartTLS(int port, const std::string& certPath, const std::string& keyPath) {
    return PawnREST::GetCore().StartTLS(port, certPath, keyPath);
}
inline bool ImplStop() { return PawnREST::GetCore().Stop(); }
inline bool ImplIsRunning() { return PawnREST::GetCore().IsRunning(); }
inline int ImplGetPort() { return PawnREST::GetCore().GetPort(); }
inline bool ImplIsTLSEnabled() { return PawnREST::GetCore().IsTLSEnabled(); }

// -----------------------------------------------------------------------------
// Incoming file route management
// -----------------------------------------------------------------------------

inline int ImplRegisterUploadRoute(const std::string& endpoint, const std::string& path, const std::string& allowedExts, int maxSizeMb) {
    return PawnREST::GetCore().RegisterRoute(endpoint, path, allowedExts, maxSizeMb);
}
inline bool ImplAddRouteKey(int routeId, const std::string& key) { return PawnREST::GetCore().AddKeyToRoute(routeId, key); }
inline bool ImplRemoveRouteKey(int routeId, const std::string& key) { return PawnREST::GetCore().RemoveKeyFromRoute(routeId, key); }
inline bool ImplSetConflictMode(int routeId, int mode) { return PawnREST::GetCore().SetConflictMode(routeId, mode); }
inline bool ImplSetCorruptAction(int routeId, int action) { return PawnREST::GetCore().SetCorruptAction(routeId, action); }
inline bool ImplSetRequireCRC32(int routeId, bool required) { return PawnREST::GetCore().SetRequireCRC32(routeId, required); }
inline bool ImplRemoveUploadRoute(int routeId) { return PawnREST::GetCore().RemoveRoute(routeId); }
inline bool ImplSetAllowList(int routeId, bool allow) { return PawnREST::GetCore().SetAllowList(routeId, allow); }
inline bool ImplSetAllowDownload(int routeId, bool allow) { return PawnREST::GetCore().SetAllowDownload(routeId, allow); }
inline bool ImplSetAllowDelete(int routeId, bool allow) { return PawnREST::GetCore().SetAllowDelete(routeId, allow); }
inline bool ImplSetAllowInfo(int routeId, bool allow) { return PawnREST::GetCore().SetAllowInfo(routeId, allow); }
inline int ImplGetFileCount(int routeId) { return PawnREST::GetCore().GetRouteFileCount(routeId); }
inline std::string ImplGetFileName(int routeId, int index) { return PawnREST::GetCore().GetRouteFileName(routeId, index); }
inline bool ImplDeleteFile(int routeId, const std::string& filename) { return PawnREST::GetCore().DeleteRouteFile(routeId, filename); }
inline int ImplGetFileSize(int routeId, const std::string& filename) { return static_cast<int>(PawnREST::GetCore().GetRouteFileSize(routeId, filename)); }

// -----------------------------------------------------------------------------
// REST API routes
// -----------------------------------------------------------------------------

inline int ImplRegisterRoute(int method, const std::string& endpoint, const std::string& callback) {
    return PawnREST::GetCore().RegisterApiRoute(method, endpoint, callback);
}
inline bool ImplRemoveRoute(int routeId) { return PawnREST::GetCore().RemoveApiRoute(routeId); }
inline bool ImplRemoveApiRoute(int routeId) { return PawnREST::GetCore().RemoveApiRoute(routeId); }
inline bool ImplSetApiRouteAuth(int routeId, const std::string& key) { return PawnREST::GetCore().SetApiRouteAuth(routeId, key); }

// -----------------------------------------------------------------------------
// Request data
// -----------------------------------------------------------------------------

inline std::string ImplGetRequestIP(int requestId) { return PawnREST::GetCore().GetRequestIP(requestId); }
inline int ImplGetRequestMethod(int requestId) { return PawnREST::GetCore().GetRequestMethod(requestId); }
inline std::string ImplGetRequestPath(int requestId) { return PawnREST::GetCore().GetRequestPath(requestId); }
inline std::string ImplGetRequestBody(int requestId) { return PawnREST::GetCore().GetRequestBody(requestId); }
inline int ImplGetRequestBodyLength(int requestId) { return PawnREST::GetCore().GetRequestBodyLength(requestId); }
inline std::string ImplGetParam(int requestId, const std::string& name) { return PawnREST::GetCore().GetParam(requestId, name); }
inline int ImplGetParamInt(int requestId, const std::string& name) { return PawnREST::GetCore().GetParamInt(requestId, name); }
inline std::string ImplGetQuery(int requestId, const std::string& name) { return PawnREST::GetCore().GetQuery(requestId, name); }
inline int ImplGetQueryInt(int requestId, const std::string& name, int defaultValue) { return PawnREST::GetCore().GetQueryInt(requestId, name, defaultValue); }
inline std::string ImplGetHeader(int requestId, const std::string& name) { return PawnREST::GetCore().GetHeader(requestId, name); }

// -----------------------------------------------------------------------------
// Response
// -----------------------------------------------------------------------------

inline bool ImplRespond(int requestId, int status, const std::string& body, const std::string& contentType) {
    return PawnREST::GetCore().Respond(requestId, status, body, contentType);
}
inline bool ImplRespondJSON(int requestId, int status, const std::string& json) {
    return PawnREST::GetCore().RespondJSON(requestId, status, json);
}
inline bool ImplRespondError(int requestId, int status, const std::string& message) {
    return PawnREST::GetCore().RespondError(requestId, status, message);
}
inline bool ImplRespondNode(int requestId, int status, int nodeId) {
    return PawnREST::GetCore().RespondNode(requestId, status, nodeId);
}
inline bool ImplSetResponseHeader(int requestId, const std::string& name, const std::string& value) {
    return PawnREST::GetCore().SetResponseHeader(requestId, name, value);
}

// -----------------------------------------------------------------------------
// JSON nodes
// -----------------------------------------------------------------------------

inline int ImplJsonParse(const std::string& json) { return PawnREST::GetCore().ParseJsonNode(json); }
inline int ImplRequestJsonNode(int requestId) { return PawnREST::GetCore().ParseRequestJsonNode(requestId); }
inline int ImplNodeType(int nodeId) { return PawnREST::GetCore().JsonNodeType(nodeId); }
inline std::string ImplNodeStringify(int nodeId) {
    std::string out;
    PawnREST::GetCore().JsonNodeStringify(nodeId, out, INT_MAX);
    return out;
}
inline bool ImplNodeRelease(int nodeId) { return PawnREST::GetCore().ReleaseJsonNode(nodeId); }
inline int ImplNodeObject() { return PawnREST::GetCore().JsonNodeObject(); }
inline int ImplNodeArray() { return PawnREST::GetCore().JsonNodeArray(); }
inline int ImplNodeString(const std::string& value) { return PawnREST::GetCore().JsonNodeString(value); }
inline int ImplNodeInt(int value) { return PawnREST::GetCore().JsonNodeInt(value); }
inline int ImplNodeFloat(float value) { return PawnREST::GetCore().JsonNodeFloat(value); }
inline int ImplNodeBool(bool value) { return PawnREST::GetCore().JsonNodeBool(value); }
inline int ImplNodeNull() { return PawnREST::GetCore().JsonNodeNull(); }
inline int ImplJsonAppend(int leftNodeId, int rightNodeId) { return PawnREST::GetCore().JsonAppend(leftNodeId, rightNodeId); }
inline bool ImplNodeSet(int objectNodeId, const std::string& key, int valueNodeId) { return PawnREST::GetCore().JsonNodeSet(objectNodeId, key, valueNodeId); }
inline bool ImplNodeSetString(int objectNodeId, const std::string& key, const std::string& value) { return PawnREST::GetCore().JsonNodeSetString(objectNodeId, key, value); }
inline bool ImplNodeSetInt(int objectNodeId, const std::string& key, int value) { return PawnREST::GetCore().JsonNodeSetInt(objectNodeId, key, value); }
inline bool ImplNodeSetFloat(int objectNodeId, const std::string& key, float value) { return PawnREST::GetCore().JsonNodeSetFloat(objectNodeId, key, value); }
inline bool ImplNodeSetBool(int objectNodeId, const std::string& key, bool value) { return PawnREST::GetCore().JsonNodeSetBool(objectNodeId, key, value); }
inline bool ImplNodeSetNull(int objectNodeId, const std::string& key) { return PawnREST::GetCore().JsonNodeSetNull(objectNodeId, key); }
inline bool ImplNodeHas(int objectNodeId, const std::string& key) { return PawnREST::GetCore().JsonNodeHas(objectNodeId, key); }
inline int ImplNodeGet(int objectNodeId, const std::string& key) { return PawnREST::GetCore().JsonNodeGet(objectNodeId, key); }
inline std::string ImplNodeGetString(int objectNodeId, const std::string& key) { return PawnREST::GetCore().JsonNodeGetString(objectNodeId, key, ""); }
inline int ImplNodeGetInt(int objectNodeId, const std::string& key, int defaultValue = 0) { return PawnREST::GetCore().JsonNodeGetInt(objectNodeId, key, defaultValue); }
inline float ImplNodeGetFloat(int objectNodeId, const std::string& key, float defaultValue = 0.0f) { return static_cast<float>(PawnREST::GetCore().JsonNodeGetFloat(objectNodeId, key, defaultValue)); }
inline bool ImplNodeGetBool(int objectNodeId, const std::string& key, bool defaultValue = false) { return PawnREST::GetCore().JsonNodeGetBool(objectNodeId, key, defaultValue); }
inline int ImplNodeArrayLength(int arrayNodeId) { return PawnREST::GetCore().JsonNodeArrayLength(arrayNodeId); }
inline int ImplNodeArrayGet(int arrayNodeId, int index) { return PawnREST::GetCore().JsonNodeArrayGet(arrayNodeId, index); }
inline bool ImplNodeArrayPush(int arrayNodeId, int valueNodeId) { return PawnREST::GetCore().JsonNodeArrayPush(arrayNodeId, valueNodeId); }
inline bool ImplNodeArrayPushString(int arrayNodeId, const std::string& value) { return PawnREST::GetCore().JsonNodeArrayPushString(arrayNodeId, value); }
inline bool ImplNodeArrayPushInt(int arrayNodeId, int value) { return PawnREST::GetCore().JsonNodeArrayPushInt(arrayNodeId, value); }
inline bool ImplNodeArrayPushFloat(int arrayNodeId, float value) { return PawnREST::GetCore().JsonNodeArrayPushFloat(arrayNodeId, value); }
inline bool ImplNodeArrayPushBool(int arrayNodeId, bool value) { return PawnREST::GetCore().JsonNodeArrayPushBool(arrayNodeId, value); }
inline bool ImplNodeArrayPushNull(int arrayNodeId) { return PawnREST::GetCore().JsonNodeArrayPushNull(arrayNodeId); }

// -----------------------------------------------------------------------------
// Outgoing upload
// -----------------------------------------------------------------------------

inline int ImplUploadFile(const std::string& url, const std::string& filepath, const std::string& filename, const std::string& authKey, const std::string& customHeaders, bool calculateCrc32, int mode, bool verifyTls) {
    return PawnREST::GetCore().QueueUpload(url, filepath, filename, authKey, customHeaders, calculateCrc32, mode, verifyTls);
}
inline int ImplCreateUploadClient(const std::string& baseUrl, const std::string& defaultHeaders, bool verifyTls) {
    return PawnREST::GetCore().CreateUploadClient(baseUrl, defaultHeaders, verifyTls);
}
inline bool ImplRemoveUploadClient(int clientId) { return PawnREST::GetCore().RemoveUploadClient(clientId); }
inline bool ImplSetUploadClientHeader(int clientId, const std::string& name, const std::string& value) { return PawnREST::GetCore().SetUploadClientHeader(clientId, name, value); }
inline bool ImplRemoveUploadClientHeader(int clientId, const std::string& name) { return PawnREST::GetCore().RemoveUploadClientHeader(clientId, name); }
inline int ImplUploadFileWithClient(int clientId, const std::string& path, const std::string& filepath, const std::string& filename, const std::string& authKey, const std::string& customHeaders, bool calculateCrc32, int mode) {
    return PawnREST::GetCore().QueueUploadWithClient(clientId, path, filepath, filename, authKey, customHeaders, calculateCrc32, mode);
}
inline bool ImplCancelUpload(int uploadId) { return PawnREST::GetCore().CancelUpload(uploadId); }
inline int ImplGetUploadStatus(int uploadId) { return PawnREST::GetCore().GetUploadStatus(uploadId); }
inline int ImplGetUploadProgress(int uploadId) { return PawnREST::GetCore().GetUploadProgress(uploadId); }
inline bool ImplGetUploadResponse(int uploadId, std::string& output, int maxLen) { return PawnREST::GetCore().GetUploadResponse(uploadId, output, maxLen); }
inline int ImplGetUploadErrorCode(int uploadId) { return PawnREST::GetCore().GetUploadErrorCode(uploadId); }
inline bool ImplGetUploadErrorType(int uploadId, std::string& output, int maxLen) { return PawnREST::GetCore().GetUploadErrorType(uploadId, output, maxLen); }
inline int ImplGetUploadHttpStatus(int uploadId) { return PawnREST::GetCore().GetUploadHttpStatus(uploadId); }

// -----------------------------------------------------------------------------
// Outgoing requests
// -----------------------------------------------------------------------------

inline int ImplCreateRequestClient(const std::string& baseUrl, const std::string& defaultHeaders, bool verifyTls) {
    return PawnREST::GetCore().CreateRequestClient(baseUrl, defaultHeaders, verifyTls);
}
inline bool ImplRemoveRequestClient(int clientId) { return PawnREST::GetCore().RemoveRequestClient(clientId); }
inline bool ImplSetRequestClientHeader(int clientId, const std::string& name, const std::string& value) { return PawnREST::GetCore().SetRequestClientHeader(clientId, name, value); }
inline bool ImplRemoveRequestClientHeader(int clientId, const std::string& name) { return PawnREST::GetCore().RemoveRequestClientHeader(clientId, name); }
inline int ImplRequest(int clientId, const std::string& path, int method, const std::string& callback, const std::string& body, const std::string& headers) {
    return PawnREST::GetCore().QueueOutboundRequest(clientId, path, method, callback, body, headers, false);
}
inline int ImplRequestJSON(int clientId, const std::string& path, int method, const std::string& callback, int jsonNodeId, const std::string& headers) {
    std::string body;
    if (jsonNodeId >= 0 && !PawnREST::GetCore().JsonNodeStringify(jsonNodeId, body, INT_MAX)) {
        return -1;
    }
    return PawnREST::GetCore().QueueOutboundRequest(clientId, path, method, callback, body, headers, true);
}
inline bool ImplCancelRequest(int requestId) { return PawnREST::GetCore().CancelOutboundRequest(requestId); }
inline int ImplGetRequestStatus(int requestId) { return PawnREST::GetCore().GetOutboundRequestStatus(requestId); }
inline int ImplGetRequestHttpStatus(int requestId) { return PawnREST::GetCore().GetOutboundRequestHttpStatus(requestId); }
inline int ImplGetRequestErrorCode(int requestId) { return PawnREST::GetCore().GetOutboundRequestErrorCode(requestId); }
inline bool ImplGetRequestErrorType(int requestId, std::string& output, int maxLen) { return PawnREST::GetCore().GetOutboundRequestErrorType(requestId, output, maxLen); }
inline bool ImplGetRequestResponse(int requestId, std::string& output, int maxLen) { return PawnREST::GetCore().GetOutboundRequestResponse(requestId, output, maxLen); }

// -----------------------------------------------------------------------------
// WebSocket
// -----------------------------------------------------------------------------

inline int ImplWebSocketClient(const std::string& address, const std::string& callback, const std::string& headers, bool verifyTls) {
    return PawnREST::GetCore().ConnectWebSocketClient(address, callback, false, headers, verifyTls);
}
inline int ImplJsonWebSocketClient(const std::string& address, const std::string& callback, const std::string& headers, bool verifyTls) {
    return PawnREST::GetCore().ConnectWebSocketClient(address, callback, true, headers, verifyTls);
}
inline bool ImplWebSocketSend(int socketId, const std::string& data) { return PawnREST::GetCore().WebSocketSendText(socketId, data); }
inline bool ImplJsonWebSocketSend(int socketId, int nodeId) { return PawnREST::GetCore().WebSocketSendJson(socketId, nodeId); }
inline bool ImplWebSocketClose(int socketId, int status, const std::string& reason) { return PawnREST::GetCore().CloseWebSocketClient(socketId, status, reason); }
inline bool ImplRemoveWebSocketClient(int socketId) { return PawnREST::GetCore().RemoveWebSocketClient(socketId); }
inline bool ImplIsWebSocketOpen(int socketId) { return PawnREST::GetCore().IsWebSocketOpen(socketId); }

// -----------------------------------------------------------------------------
// CRC32 utilities
// -----------------------------------------------------------------------------

inline int ImplVerifyCRC32(const std::string& filepath, const std::string& expectedCrc) {
    std::string base = FileUtils::GetCurrentWorkingDirectory();
    std::string fullPath = base + filepath;
    if (!FileUtils::FileExists(fullPath)) return -1;

    uint32_t calculated = CRC32::fileChecksum(fullPath);
    uint32_t expected = CRC32::fromHex(expectedCrc);
    return (calculated == expected) ? 1 : 0;
}

inline int ImplGetFileCRC32(const std::string& filepath, std::string& output) {
    std::string base = FileUtils::GetCurrentWorkingDirectory();
    std::string fullPath = base + filepath;
    if (!FileUtils::FileExists(fullPath)) {
        output = "0";
        return 0;
    }

    output = CRC32::toHex(CRC32::fileChecksum(fullPath));
    return 1;
}

inline int ImplCompareFiles(const std::string& path1, const std::string& path2) {
    std::string base = FileUtils::GetCurrentWorkingDirectory();
    std::string fullPath1 = base + path1;
    std::string fullPath2 = base + path2;
    if (!FileUtils::FileExists(fullPath1) || !FileUtils::FileExists(fullPath2)) return -1;
    return CRC32::fileChecksum(fullPath1) == CRC32::fileChecksum(fullPath2) ? 1 : 0;
}

// -----------------------------------------------------------------------------
// Raw event drain (used by SA-MP ProcessTick)
// -----------------------------------------------------------------------------

inline std::vector<APIRequestEvent> ImplDrainApiEvents() { return PawnREST::GetCore().DrainApiEventsRaw(); }
inline std::vector<UploadEvent> ImplDrainUploadEvents() { return PawnREST::GetCore().DrainUploadEventsRaw(); }
inline std::vector<OutgoingUploadEvent> ImplDrainOutgoingUploadEvents() { return PawnREST::GetCore().DrainOutgoingUploadEventsRaw(); }
inline std::vector<OutgoingRequestEvent> ImplDrainRequestEvents() { return PawnREST::GetCore().DrainRequestEventsRaw(); }
inline std::vector<WebSocketEvent> ImplDrainWebSocketEvents() { return PawnREST::GetCore().DrainWebSocketEventsRaw(); }
