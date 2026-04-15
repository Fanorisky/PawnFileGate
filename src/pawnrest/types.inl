/*
 * PawnREST - Shared Type Definitions
 * Used by both open.mp and SA-MP builds
 */
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <httplib.h>

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

namespace Config {
    inline constexpr int DRAIN_FPS = 30;
    inline constexpr int MAX_CONCURRENT = 16;
    inline constexpr size_t TEMP_CLEANUP_SEC = 3600;
    inline const char* TEMP_DIR = ".tmp/";
    inline constexpr int UPLOAD_TIMEOUT_SEC = 300;
    inline constexpr size_t UPLOAD_CHUNK_SIZE = 65536;
}

// ═══════════════════════════════════════════════════════════════════════════
// Enumerations
// ═══════════════════════════════════════════════════════════════════════════

enum class ConflictMode {
    Rename,
    Overwrite,
    Reject
};

enum class CorruptAction {
    Delete,
    Quarantine,
    Keep
};

enum class UploadStatus {
    Pending = 0,
    Uploading = 1,
    Completed = 2,
    Failed = 3,
    Cancelled = 4
};

enum class UploadMode {
    Multipart = 0,
    Raw = 1
};

enum class RequestStatus {
    Pending = 0,
    Requesting = 1,
    Completed = 2,
    Failed = 3,
    Cancelled = 4
};

enum class HttpMethod {
    GET = 0,
    POST = 1,
    PUT = 2,
    PATCH = 3,
    DELETE_ = 4,
    HEAD = 5,
    OPTIONS = 6
};

// ═══════════════════════════════════════════════════════════════════════════
// Upload Server Structures
// ═══════════════════════════════════════════════════════════════════════════

struct UploadRoute {
    int         routeId = -1;
    std::string endpoint;
    std::string destinationPath;
    std::vector<std::string> allowedExtensions;
    std::unordered_set<std::string> authorizedKeys;
    size_t      maxSizeBytes = 10 * 1024 * 1024;
    ConflictMode onConflict  = ConflictMode::Rename;
    CorruptAction onCorrupt  = CorruptAction::Delete;
    std::string quarantinePath = "quarantine/";
    bool        requireCrc32 = false;
    bool        allowList = false;
    bool        allowDownload = false;
    bool        allowDelete = false;
    bool        allowInfo = false;
};

struct UploadEvent {
    enum class Type { Completed, Failed, Progress };
    Type        type;
    int         uploadId  = -1;
    int         routeId   = -1;
    std::string endpoint;
    std::string filename;
    std::string filepath;
    std::string reason;
    int         progressPct = 0;
    uint32_t    crc32Checksum = 0;
    bool        crc32Matched = true;
    std::string expectedCrc32;
};

struct ValidationResult {
    bool        ok = false;
    std::string reason;
};

// ═══════════════════════════════════════════════════════════════════════════
// Upload Client Structures
// ═══════════════════════════════════════════════════════════════════════════

struct OutgoingUpload {
    int         uploadId = -1;
    std::string url;
    std::string filepath;
    std::string filename;
    std::string authKey;
    std::string customHeader;
    bool        calculateCrc32 = true;
    UploadMode  mode = UploadMode::Multipart;
    bool        verifyTls = true;
    UploadStatus status = UploadStatus::Pending;
    int         progressPct = 0;
    uint32_t    crc32Checksum = 0;
    std::string responseBody;
    int         httpStatus = 0;
    int         errorCode = 0;
    std::string errorType;
    std::string errorMessage;
    size_t      bytesUploaded = 0;
    size_t      totalBytes = 0;
    std::shared_ptr<std::atomic<bool>> cancelToken = std::make_shared<std::atomic<bool>>(false);
};

struct UploadClient {
    int         clientId = -1;
    std::string baseUrl;
    std::unordered_map<std::string, std::string> defaultHeaders;
    bool        verifyTls = true;
};

struct OutgoingUploadEvent {
    enum class Type { Started, Progress, Completed, Failed };
    Type        type;
    int         uploadId = -1;
    int         progressPct = 0;
    uint32_t    crc32Checksum = 0;
    int         httpStatus = 0;
    int         errorCode = 0;
    std::string errorType;
    std::string responseBody;
    std::string errorMessage;
};

// ═══════════════════════════════════════════════════════════════════════════
// HTTP Request Client Structures
// ═══════════════════════════════════════════════════════════════════════════

struct OutgoingRequest {
    int         requestId = -1;
    std::string url;
    int         method = 0;
    std::string callbackName;
    std::string body;
    std::string customHeader;
    bool        expectJson = false;
    bool        verifyTls = true;
    RequestStatus status = RequestStatus::Pending;
    int         httpStatus = 0;
    int         errorCode = 0;
    std::string errorType;
    std::string errorMessage;
    std::string responseBody;
    std::shared_ptr<std::atomic<bool>> cancelToken = std::make_shared<std::atomic<bool>>(false);
};

struct OutgoingRequestEvent {
    enum class Type { CompletedText, CompletedJson, Failed };
    Type        type = Type::Failed;
    int         requestId = -1;
    std::string callbackName;
    int         httpStatus = 0;
    std::string responseBody;
    int         nodeId = -1;
    int         errorCode = 0;
    std::string errorType;
    std::string errorMessage;
};

// ═══════════════════════════════════════════════════════════════════════════
// WebSocket Structures
// ═══════════════════════════════════════════════════════════════════════════

struct WebSocketConnection {
    int         socketId = -1;
    std::string address;
    std::string callbackName;
    bool        jsonMode = false;
    bool        verifyTls = true;
    std::unordered_map<std::string, std::string> headers;
    std::unique_ptr<httplib::ws::WebSocketClient> client;
    std::shared_ptr<std::atomic<bool>> stopToken = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> disconnectEmitted { false };
    std::thread readThread;
    std::mutex sendMutex;
    int closeStatus = static_cast<int>(httplib::ws::CloseStatus::Normal);
    std::string closeReason;
};

struct WebSocketEvent {
    enum class Type { MessageText, MessageJson, Disconnected };
    Type        type = Type::MessageText;
    int         socketId = -1;
    std::string callbackName;
    bool        isJson = false;
    std::string textPayload;
    int         jsonNodeId = -1;
    int         closeStatus = static_cast<int>(httplib::ws::CloseStatus::Normal);
    std::string closeReason;
    int         errorCode = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
// REST API Structures
// ═══════════════════════════════════════════════════════════════════════════

struct APIRoute {
    int                  routeId = -1;
    HttpMethod           method = HttpMethod::GET;
    std::string          endpoint;
    std::string          callbackName;
    std::unordered_set<std::string> authKeys;
    std::vector<std::string> paramNames;
    std::regex           pattern;
    bool                 requireAuth = false;
};

struct RequestContext {
    int                  requestId = -1;
    HttpMethod           method = HttpMethod::GET;
    std::string          path;
    std::string          body;
    std::string          clientIP;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string> queries;
    std::unordered_map<std::string, std::string> headers;
    std::string          responseJson;
    std::vector<std::string> jsonStack;
    bool                 jsonStarted = false;
    std::unordered_map<std::string, std::string> responseHeaders;
    std::atomic<bool>    responded { false };
    httplib::Response*   httpRes = nullptr;
};

struct APIRequestEvent {
    int         requestId = -1;
    int         routeId = -1;
    std::string callbackName;
};

// ═══════════════════════════════════════════════════════════════════════════
// Platform Abstraction Interface
// ═══════════════════════════════════════════════════════════════════════════

class IPlatformAdapter {
public:
    virtual ~IPlatformAdapter() = default;
    
    // Logging
    virtual void LogInfo(const char* format, ...) = 0;
    virtual void LogError(const char* format, ...) = 0;
    
    // AMX/Script interaction
    virtual bool GetAmxString(void* amx, int idx, std::string& out) = 0;
    virtual bool SetAmxString(void* amx, int idx, const std::string& str, int maxLen) = 0;
    virtual int GetAmxParam(void* amx, int idx) = 0;
    virtual float GetAmxParamFloat(void* amx, int idx) = 0;
    virtual void SetAmxReturnFloat(void* amx, int idx, float val) = 0;
    
    // Callback invocation
    virtual bool FindPublic(void* amx, const char* name, int* idx) = 0;
    virtual bool PushInt(void* amx, int val) = 0;
    virtual bool PushFloat(void* amx, float val) = 0;
    virtual bool PushString(void* amx, const std::string& str) = 0;
    virtual bool Exec(void* amx, int idx, int* retval) = 0;
    
    // Script enumeration (for callbacks)
    virtual std::vector<void*> GetLoadedScripts() = 0;
    
    // Server info
    virtual std::string GetServerRootPath() = 0;
};
